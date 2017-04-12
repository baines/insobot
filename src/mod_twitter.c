#include "module.h"
#include "module_msgs.h"
#include "stb_sb.h"
#include "inso_utils.h"
#include "inso_tz.h"
#include "inso_json.h"
#include <inttypes.h>
#include <curl/curl.h>
#include "twc.h"

// NOTE:
// Currently this is just for searching twitter for schedule information
// and passing it on to mod_schedule.
// 
// It should probably be expanded to do other twitter things, or be renamed.

// TODO: add a command to add/remove links at runtime

static bool twitter_init (const IRCCoreCtx*);
static void twitter_tick (time_t);
static void twitter_quit (void);
static bool twitter_save (FILE*);

const IRCModuleCtx irc_mod_ctx = {
	.name    = "twitter",
	.desc    = "Get stream schedules from twitter",
	.flags   = IRC_MOD_GLOBAL,
	.on_init = &twitter_init,
	.on_tick = &twitter_tick,
	.on_quit = &twitter_quit,
	.on_save = &twitter_save
};

static const IRCCoreCtx* ctx;
static time_t last_update;
static CURL* curl;
static struct curl_slist* twitter_headers;
static uint64_t twitter_since_id;

static twc_state twc;
static bool using_twc;

typedef struct {
	char* twitter;
	char* twitch;
	char* title;
	int duration;
	time_t last_modified;
} TwitterSchedule;

TwitterSchedule* schedules;

enum { MON, TUE, WED, THU, FRI, SAT, SUN, DAYS_IN_WEEK };
static int get_dow(const struct tm* tm){
	return tm->tm_wday ? tm->tm_wday - 1 : SUN;
}

static bool twitter_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	const char* token = getenv("INSOBOT_TWITTER_TOKEN");
	if(!token){
		fprintf(stderr, "mod_twitter: no token, exiting.\n");
		return false;
	}

	curl = curl_easy_init();
	{
		char* h;
		asprintf_check(&h, "Authorization: Bearer %s", token);
		twitter_headers = curl_slist_append(NULL, h);
		free(h);
	}

	last_update = time(0) - (14*60+40);

	FILE* f = fopen(ctx->get_datafile(), "r");
	TwitterSchedule ts = {};
	size_t last_mod = 0;
	
	while(fscanf(f, "%ms %ms %d %zu %m[^\n]", &ts.twitter, &ts.twitch, &ts.duration, &last_mod, &ts.title) == 5){
		printf("mod_twitter: Adding schedule link [%s] -> [%s] [%s]\n", ts.twitter, ts.twitch, ts.title);
		ts.last_modified = last_mod;
		sb_push(schedules, ts);
	}
	fclose(f);

	twc_oauth_keys keys = {
		getenv("INSOBOT_TWITTER_OAUTH_CKEY"),
		getenv("INSOBOT_TWITTER_OAUTH_CSEC"),
		getenv("INSOBOT_TWITTER_OAUTH_TKEY"),
		getenv("INSOBOT_TWITTER_OAUTH_TSEC"),
	};

	if(keys.ConsumerKey && keys.ConsumerSecret && keys.TokenKey && keys.TokenSecret){
		twc_Init(&twc, keys);
		using_twc = true;
	}

	return true;
}

static bool sched_has_date(const SchedMsg* sched, const struct tm* date, int* day_out){

	struct tm sched_utc = {}, sched_local = {};
	gmtime_r(&sched->start, &sched_utc);

	char* tz = tz_push_off(date->tm_gmtoff);
	localtime_r(&sched->start, &sched_local);
	tz_pop(tz);

	// we have to do some awkward adjusting here to make sure "today" in some
	// arbitrary timezone lines up with "today" in UTC (stored in the mask)

	int day = get_dow(date);
	if(sched_local.tm_mday > sched_utc.tm_mday){
		day = (day + 6) % 7;
	} else if(sched_local.tm_mday < sched_utc.tm_mday){
		day = (day + 1) % 7;
	}

	if(sched->repeat & (1 << day)){
		if(day_out) *day_out = day;
		return true;
	}

	if(sched_local.tm_year == date->tm_year && sched_local.tm_yday == date->tm_yday){
		if(day_out) *day_out = -1;
		return true;
	}

	return false;
}

typedef struct {
	const char* title; // name of schedule to remove
	struct tm* sched;  // year+month+day to remove, or NULL to remove all
	bool modified;     // [out] set by the callback if it modified anything
} SchedIterInfo;

static intptr_t twitter_sched_iter_cb(intptr_t result, intptr_t arg){
	SchedMsg*      sched = (SchedMsg*)result;
	SchedIterInfo* info  = (SchedIterInfo*)arg;

	int day;
	if(strcmp(sched->title, info->title) == 0){
		if(info->sched == NULL){
			info->modified = true;
			return SCHED_ITER_DELETE;
		} else if(sched_has_date(sched, info->sched, &day)){
			info->modified = true;

			if(day == -1 || __builtin_popcount(sched->repeat) == 1){
				return SCHED_ITER_DELETE;
			}

			sched->repeat &= ~(1 << day);

			int start_day = __builtin_ffs(sched->repeat) - 1;
			if(start_day > day){
				int diff = (start_day - day) * (60*60*24);

				sched->start += diff;
				sched->end   += diff;
			}
		}
	}

	return SCHED_ITER_CONTINUE;
}

static bool twitter_parse_time(const char* msg, struct tm* out){
	const char* tzp = NULL;

	if(!tzp) tzp = strptime(msg, " %I:%M %p", out);
	if(!tzp) tzp = strptime(msg, " %H:%M", out);

	if(tzp){
		int off;
		while(*tzp == ' ') ++tzp;

		if(tz_abbr2off(tzp, &off)){
			out->tm_min -= off;
			timegm(out);
		}

		return true;
	}

	return false;
}

static bool twitter_parse_datetime(const char* msg, struct tm* out){
	struct tm tm = {};
	time_t now = time(0);

	// XXX: i think using "now" here might break dst...
	gmtime_r(&now, &tm);
	tm.tm_sec = tm.tm_min = tm.tm_hour = 0;

	char* timep = strptime(msg, "%b %d", &tm);
	if(timep){
		if(!twitter_parse_time(timep, &tm)){
			tm.tm_min = tm.tm_hour = -1;
		}
		tm.tm_gmtoff = 0;
		memcpy(out, &tm, sizeof(*out));
		return true;
	}

	return false;
}

static bool twitter_parse_bulk(const TwitterSchedule* ts, const char* _msg, struct tm** days){
	char* msg = strdupa(_msg);
	char* p;

	if(strncasecmp(_msg, "This week", 9) == 0 && (p = strchr(msg, '\n'))){
		char* state;
		char* line = strtok_r(p, "\n", &state);

		while(line){
			struct tm tm = {};
			if(twitter_parse_datetime(line, &tm)){
				sb_push(*days, tm);
			}

			line = strtok_r(NULL, "\n", &state);
		}

		// make sure old schedules are cleared out for this style
		if(sb_count(*days)){
			SchedIterInfo info = { ts->title, NULL };
			MOD_MSG(ctx, "sched_iter", ts->twitch, &twitter_sched_iter_cb, &info);
			return true;
		}
	}

	return false;
}

static bool twitter_strfind(const char* haystack, const char** needles){
	for(const char** n = needles; *n; ++n){
		if(strcasestr(haystack, *n)){
			return true;
		}
	}

	return false;
}

static bool twitter_parse_reschedule(const TwitterSchedule* ts, const char* msg, yajl_val data, time_t tweet_time, struct tm** days){

	static const char* today_words[] = {
		"today", "tonight", "this morning", "this afternoon", "this evening", NULL
	};
	static const char* resched_words[] = {
		"cancel", "missed", "no stream", "moved", "moving", "chang", NULL
	};

	struct tm tm = {};

	// get local day (might be wrong if the twitter user hasn't set their timezone :/)
	int utc_offset = 0;
	yajl_val json_off = YAJL_GET(data, yajl_t_number, ("user", "utc_offset"));
	if(YAJL_IS_INTEGER(json_off)){
		utc_offset -= (json_off->u.number.i / 60);
	}

	char* tz = tz_push_off(utc_offset);
	localtime_r(&tweet_time, &tm);
	tm.tm_hour = tm.tm_min = -1;
	tm.tm_sec = 0;
	tm.tm_gmtoff = utc_offset;

	bool found = false;

	if(twitter_strfind(msg, today_words) && twitter_strfind(msg, resched_words)){
		found = true;

		// add a delete entry for today
		sb_push(*days, tm);
		printf("mod_twitter: Found cancel [%d-%d] [%ld]\n", tm.tm_mon, tm.tm_mday, tm.tm_gmtoff);

		// since the twitter_parse_time stuff works in UTC, change tm to that.
		// this might be unnecessary but i think there could be some edge cases with day switchover...
		tm.tm_gmtoff = 0;
		gmtime_r(&tweet_time, &tm);
		tm.tm_hour = tm.tm_min = tm.tm_sec = 0;

		// look for a rescheduled time, push the new time into days
		// XXX: assumes any timestamp it hits refers to the same day
		for(const char* p = msg; *p; ++p){
			if(twitter_parse_time(p, &tm)){
				sb_push(*days, tm);
				printf("mod_twitter: Found reschedule [%d-%d] -> [%d:%d]\n", tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min);
				break;
			}
		}
	}

	tz_pop(tz);
	return found;
}

static bool twitter_sched_parse(const TwitterSchedule* ts, const char* msg, yajl_val data, time_t tweet_time){
	struct tm* days = NULL;
	bool parsed = false;
	bool modified = false;

	printf("mod_twitter: begin parse for [%s] [%s]\n", ts->twitter, msg);

	if(!parsed){
		parsed = twitter_parse_bulk(ts, msg, &days);
		// XXX: assume the bulk ones always modify if they parsed
		modified = parsed;
	}

	if(!parsed){
		parsed = twitter_parse_reschedule(ts, msg, data, tweet_time, &days);
	}

	// send update to mod_schedule
	for(int i = 0; i < sb_count(days); ++i){
		uint8_t mask = (1 << get_dow(days + i));

		for(int j = i+1; j < sb_count(days); ++j){
			if (days[j].tm_min  == days[i].tm_min &&
				days[j].tm_hour == days[i].tm_hour){
				
				mask |= (1 << get_dow(days + j));
				sb_erase(days, j);
				--j;
			}
		}

		if(days[i].tm_min == -1){
			SchedIterInfo info = { ts->title, days + i };
			MOD_MSG(ctx, "sched_iter", ts->twitch, &twitter_sched_iter_cb, &info);
			modified = info.modified;
		} else {
			time_t t = timegm(days + i);

			SchedMsg sm = {
				.user = ts->twitch,
				.start = t,
				.end   = t + (ts->duration * 60),
				.title = ts->title,
				.repeat = mask,
			};

			printf("twitter: add sched %s %zu %x\n", sm.user, (size_t)sm.start, sm.repeat);
			MOD_MSG(ctx, "sched_add", &sm, 0, 0);
			modified = true; // XXX: we could pass a callback to sched_add to be 100% sure?
		}
	}

	sb_free(days);

	return modified;
}

static void twitter_tick(time_t now){
	if(now - last_update < (15*60)) return;
	last_update = now;

	if(!sb_count(schedules)) return;

	char* url;
	{
		char query_buf[256];
		char* p = query_buf;
		size_t sz = sizeof(query_buf);

		snprintf_chain(&p, &sz, "live OR stream OR twitch.tv");
		sb_each(s, schedules){
			snprintf_chain(&p, &sz, "%sfrom:%s", s == schedules ? " " : " OR ", s->twitter);
		}

		char id_buf[64] = {};
		if(twitter_since_id){
			snprintf(id_buf, sizeof(id_buf), "&since_id=%" PRIu64, twitter_since_id);
		}

		char* query = curl_easy_escape(curl, query_buf, 0);
		asprintf_check(
			&url,
			"https://api.twitter.com/1.1/search/tweets.json?result_type=recent&include_entities=0&q=%s%s",
			query,
			id_buf
		);
		curl_free(query);
	}

	char* data = NULL;
	inso_curl_reset(curl, url, &data);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, twitter_headers);
	inso_curl_perform(curl, &data);

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	if(!root || !YAJL_IS_OBJECT(root)) goto end;

	yajl_val list = YAJL_GET(root, yajl_t_array, ("statuses"));
	if(!list) goto end;

	bool modified = false;

	twitter_since_id = 0;
	for(size_t i = 0; i < list->u.array.len; ++i){
		yajl_val obj = list->u.array.values[i];

		yajl_val id      = YAJL_GET(obj, yajl_t_number, ("id"));
		yajl_val text    = YAJL_GET(obj, yajl_t_string, ("text"));
		yajl_val author  = YAJL_GET(obj, yajl_t_string, ("user", "screen_name"));
		yajl_val created = YAJL_GET(obj, yajl_t_string, ("created_at"));

		if(YAJL_IS_INTEGER(id) && id->u.number.i > twitter_since_id){
			twitter_since_id = id->u.number.i;
		}

		if(text && author && created){
			TwitterSchedule* ts = NULL;
			sb_each(s, schedules){
				if(strcasecmp(s->twitter, author->u.string) == 0){
					ts = s;
					break;
				}
			}

			if(ts){
				struct tm tm = {};
				strptime(created->u.string, "%a %b %d %T %z %Y", &tm);
				time_t created_time = mktime(&tm);

				if(created_time > ts->last_modified && twitter_sched_parse(ts, text->u.string, obj, created_time)){
					ts->last_modified = created_time;
					modified = true;

					printf("mod_twitter: Sending tweet to %s.\n", author->u.string);
					if(using_twc){
						char tweet[256];
						snprintf(tweet, sizeof(tweet), "@%s Schedule registered: " SCHEDULE_URL " 🤖", author->u.string);

						twc_statuses_update_params params = {
							.InReplyToStatusId = { .Exists = true, .Value = id->u.number.i }
						};

						twc_call_result res = twc_Statuses_Update(&twc, twc_ToString(tweet), params);
						free(res.Data.Ptr);
					}
				}
			}
		}
	}

	if(modified){
		MOD_MSG(ctx, "sched_save", 0, 0, 0);
		ctx->save_me();
	}

end:
	yajl_tree_free(root);
	free(url);
	sb_free(data);
}

static void twitter_quit(void){
	sb_each(s, schedules){
		free(s->twitter);
		free(s->twitch);
		free(s->title);
	}
	sb_free(schedules);

	curl_slist_free_all(twitter_headers);
	curl_easy_cleanup(curl);

	if(using_twc){
		twc_Close(&twc);
	}
}

static bool twitter_save(FILE* f){
	sb_each(s, schedules){
		fprintf(f, "%s %s %d %zu %s\n", s->twitter, s->twitch, s->duration, (size_t)s->last_modified, s->title);
	}
	return true;
}
