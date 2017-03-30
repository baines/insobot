#include "module.h"
#include "module_msgs.h"
#include "stb_sb.h"
#include "inso_utils.h"
#include "inso_tz.h"
#include "inso_json.h"
#include <inttypes.h>
#include <curl/curl.h>

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

typedef struct {
	char* twitter;
	char* twitch;
	char* title;
	int duration;
	time_t last_modified;
} TwitterSchedule;

TwitterSchedule* schedules;

enum { MON, TUE, WED, THU, FRI, SAT, SUN, DAYS_IN_WEEK };
static int get_dow(struct tm* tm){
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

	return true;
}

static intptr_t twitter_sched_iter_cb(intptr_t result, intptr_t arg){
	SchedMsg* sched = (SchedMsg*)result;
	TwitterSchedule* ts = (TwitterSchedule*)arg;

	// TODO: need more selective logic here, and to adjust masks?
	//       the repeat mask thing seems more and more annoying...

	if(strcmp(sched->title, ts->title) == 0){
		printf("twitter_iter: delete %s %zu %x\n", sched->user, sched->start, sched->repeat);
		return SCHED_ITER_DELETE;
	} else {
		return SCHED_ITER_CONTINUE;
	}
}

static bool twitter_sched_parse(const TwitterSchedule* ts, const char* _msg){

	time_t now = time(0);
	struct tm* days = NULL;
	char* msg = strdupa(_msg);
	char* p;

	// handmade hero style
	if(strncmp(_msg, "This week", 9) == 0 && (p = strchr(msg, '\n'))){
		char* state;
		char* line = strtok_r(p, "\n", &state);

		while(line){
			struct tm tm = {};
			// XXX: i think using "now" here breaks dst...
			gmtime_r(&now, &tm);
			tm.tm_sec = 0;
			tm.tm_min = tm.tm_hour = -1;

			char* timep = strptime(line, "%b %d", &tm);

			if(timep){
				char* tzp = NULL;

				if(!tzp) tzp = strptime(timep, " %I:%M %p", &tm);
				if(!tzp) tzp = strptime(timep, " %H:%M", &tm);

				if(tzp){
					int off;
					while(*tzp == ' ') ++tzp;

					if(tz_abbr2off(tzp, &off)){
						tm.tm_min -= off;
						timegm(&tm);
					}
				}

				sb_push(days, tm);
			}
			line = strtok_r(NULL, "\n", &state);
		}

		// make sure old schedules are cleared out for this style
		// XXX: the callback will need to be changed in the future!
		if(sb_count(days)){
			MOD_MSG(ctx, "sched_iter", ts->twitch, &twitter_sched_iter_cb, ts);
		}
	}

	// TODO: check cancellations / reschedules / other formats here

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
			// TODO: fix this callback so it is able to delete single days
			//MOD_MSG(ctx, "sched_iter", ts->twitch, &twitter_sched_iter_cb, ts);
		} else {
			time_t t = timegm(days + i);

			SchedMsg sm = {
				.user = ts->twitch,
				.start = t,
				.end   = t + (ts->duration * 60),
				.title = ts->title,
				.repeat = mask,
			};

			printf("twitter: add sched %s %zu %x\n", sm.user, sm.start, sm.repeat);
			MOD_MSG(ctx, "sched_add", &sm, 0, 0);
		}
	}

	bool updated = sb_count(days);
	sb_free(days);

	if(updated){
		MOD_MSG(ctx, "sched_save", 0, 0, 0);
	}

	return updated;
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

				if(created_time > ts->last_modified && twitter_sched_parse(ts, text->u.string)){
					ts->last_modified = created_time;
					modified = true;
				}
			}
		}
	}

	if(modified){
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
}

static bool twitter_save(FILE* f){
	sb_each(s, schedules){
		fprintf(f, "%s %s %d %zu %s\n", s->twitter, s->twitch, s->duration, s->last_modified, s->title);
	}
	return true;
}
