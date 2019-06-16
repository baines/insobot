#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>
#include <ftw.h>
#include "module.h"
#include "module_msgs.h"
#include "stb_sb.h"
#include "inso_utils.h"
#include "inso_tz.h"
#include "inso_xml.h"
#include "inso_json.h"

static void hmh_cmd     (const char*, const char*, const char*, int);
static bool hmh_init    (const IRCCoreCtx*);
static void hmh_quit    (void);
static void hmh_mod_msg (const char* sender, const IRCModMsg* msg);
static void hmh_ipc     (int who, const uint8_t* ptr, size_t sz);
static void hmh_tick    (time_t);


enum { CMD_SCHEDULE, CMD_TIME, CMD_OWLBOT, CMD_OWL_Y, CMD_OWL_N, CMD_QA, CMD_LATEST };

const IRCModuleCtx irc_mod_ctx = {
	.name       = "hmh",
	.desc       = "Functionality specific to Handmade Hero",
	.on_cmd     = &hmh_cmd,
	.on_init    = &hmh_init,
	.on_quit    = &hmh_quit,
	.on_mod_msg = &hmh_mod_msg,
	.on_ipc     = &hmh_ipc,
	.on_tick    = &hmh_tick,
	.commands = DEFINE_CMDS (
		[CMD_SCHEDULE] = CMD("schedule"),
		[CMD_TIME]     = CMD("tm") CMD("time") CMD("when"),
		[CMD_OWLBOT]   = CMD("owlbot"),
		[CMD_OWL_Y]    = CMD("owly") CMD("owlyes") CMD("owlyea"),
		[CMD_OWL_N]    = CMD("owln") CMD("owlno")  CMD("owlnay"),
		[CMD_QA]       = CMD("qa"),
		[CMD_LATEST]   = CMD("latest")
	),
	.cmd_help = DEFINE_CMDS (
		[CMD_SCHEDULE] = "[TZ] | Shows the Handmade Hero schedule (optionally in the tzdb timezone [TZ], e.g. Europe/London",
		[CMD_TIME]     = "| Shows the time to the next Handmade Hero stream, or progress through the current one if it is live.",
		[CMD_OWLBOT]   = "| Start a vote to make Owlbot light up, notifying casey of something important.",
		[CMD_OWL_Y]    = "| Vote yes on an Owlbot vote.",
		[CMD_OWL_N]    = "| Vote no on an Owlbot vote.",
		[CMD_QA]       = "| Let the bot know that the Q&A has started.",
		[CMD_LATEST]   = "| Show info about the most recent episode of hmh."
	)
};

static const IRCCoreCtx* ctx;

static const char schedule_url[] = "https://db.mollyrocket.com/hhlive";

static time_t last_schedule_update;
static sb(time_t) schedule;
static time_t schedule_week;

static char* tz_buf;

static time_t owlbot_timer;
static char** owlbot_voters;
static int    owlbot_yea;
static int    owlbot_nay;

// TODO: factor this into main insobot
enum { SERV_UNKNOWN, SERV_TWITCH, SERV_HMN };
static int irc_server;

static time_t last_yt_fetch;
static char*  latest_ep_str;

static const char* hmh_get_channel(void);

#define CLEAR_SCHEDULE()({ if(schedule){ stb__sbn(schedule) = 0; } })

static bool update_schedule(void){

	time_t now = time(0);
	char* data = NULL;

	CURL* curl = inso_curl_init(schedule_url, &data);

	int curl_ret = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(curl);

	sb_push(data, 0);

	if(curl_ret != 0){
		fprintf(stderr, "Error getting schedule: %s\n", curl_easy_strerror(curl_ret));
		sb_free(data);
		return false;
	}

	char* tz = tz_push(":US/Pacific");

	// update schedule_week
	{
		time_t week_start;
		struct tm tmp = {};
		localtime_r(&now, &tmp);

		tmp.tm_isdst = -1;
		tmp.tm_mday -= get_dow(&tmp);
		tmp.tm_hour = tmp.tm_min = tmp.tm_sec = 0;

		week_start = mktime(&tmp);

		if(week_start != schedule_week){
			CLEAR_SCHEDULE();
		}

		schedule_week = week_start;
	}

	CLEAR_SCHEDULE();

	// load schedule from json

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	if(!YAJL_IS_OBJECT(root)){
		fprintf(stderr, "mod_hmh: curl data not json object.\n");
		goto out;
	}

	yajl_val dates = YAJL_GET(root, yajl_t_array, ("upcomingDates"));
	if(!dates){
		fprintf(stderr, "mod_hmh: couldn't get date array.\n");
		goto out;
	}

	for(size_t i = 0; i < dates->u.array.len; ++i){
		yajl_val date = dates->u.array.values[i];
		if(!YAJL_IS_STRING(date)){
			fprintf(stderr, "mod_hmh: dates array contains non-string?\n");
			break;
		}

		struct tm scheduled_tm = {};
		char* title = strptime(date->u.string, "%Y-%m-%d,%H:%M", &scheduled_tm);
		if(!title || *title != '\0'){
			printf("mod_hmh: error parsing date: [%s]\n", date->u.string);
			break;
		}

		scheduled_tm.tm_isdst = -1;
		time_t sched = mktime(&scheduled_tm);
		sb_push(schedule, sched);
	}

out:
	tz_pop(tz);
	sb_free(data);
	return true;
}

static intptr_t check_alias_cb(intptr_t result, intptr_t arg){
	*(int*)arg = result;
	return 0;
}

static void print_schedule(const char* chan, const char* name, const char* arg){
	time_t now = time(0);

	bool empty_sched = sb_count(schedule) == 0;

	const long lim = empty_sched ? 30 : 1800;
	if(now - last_schedule_update > lim && update_schedule()){
		last_schedule_update = now;
	}

	// parse args (timezone and/or 'terse' for old-style day grouping)

	bool terse = false;

	if(*arg == ' ' && strncasecmp(arg + 1, "terse", 5) == 0){
		terse = true;
		arg += 6;
	}

	char* tz;
	if(*arg++ == ' '){
		bool valid = false;
		char timezone[64];

		int r;
		if (!strchr(arg, ':') && !strchr(arg, '.') &&
			(r = snprintf(timezone, sizeof(timezone), ":%s.", arg)) > 0 &&
			r < isizeof(timezone)){

			// hack for Etc/* zones having reversed symbols...
			char* p;
			if((p = strchr(timezone, '+'))){
				*p = '-';
			} else if((p = strchr(timezone, '-'))){
				*p = '+';
			}

			char* ptr = strcasestr(tz_buf, timezone);
			if(!ptr){
				timezone[0] = '/';
				ptr = strcasestr(tz_buf, timezone);
			}

			if(ptr){
				while(*ptr != ':') --ptr;
				char* end = strchr(ptr, '.');
				assert(end);

				*end = 0;
				tz = tz_push(ptr);
				*end = '.';

				valid = true;
			}
		}

		if(!valid){
			ctx->send_msg(chan, "%s: Unknown timezone.", name);
			return;
		}
	} else {
		tz = tz_push(":US/Pacific");
	}

	int time_count = 0;
	bool empty_week = false;
	char msg_buf[256] = {};
	struct tm week_start = {};

	if(sb_count(schedule) > 0 && schedule[0] >= schedule_week + (7*24*60*60)){
		// there's a schedule set, but nothing for this week.
		empty_week = true;
		localtime_r(schedule + 0, &week_start);
	} else {
		// group days by equal times.

		struct time_bucket {
			int hour;
			int min;
			uint8_t bits;
		};

		struct time_bucket* buckets = calloc(sb_count(schedule), sizeof(*buckets));
		struct time_bucket* prev_bucket = NULL;

		sb_each(s, schedule){
			struct tm lt = {};
			localtime_r(s, &lt);

			struct time_bucket* bucket = NULL;

			if(terse){
				for(int i = 0; i < time_count; ++i){
					if(lt.tm_hour == buckets[i].hour && lt.tm_min == buckets[i].min){
						bucket = buckets + i;
						break;
					}
				}
			} else {
				if(prev_bucket && lt.tm_hour == prev_bucket->hour && lt.tm_min == prev_bucket->min){
					bucket = prev_bucket;
				}
			}

			if(!bucket){
				bucket = buckets + time_count++;
				bucket->hour = lt.tm_hour;
				bucket->min = lt.tm_min;
			}

			int day = get_dow(&lt);
			bucket->bits |= (1 << day);

			prev_bucket = bucket;
		}

		// generate the schedule string

		const char* days[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };

		for(int i = 0; i < time_count; ++i){
			inso_strcat(msg_buf, sizeof(msg_buf), "[");

			for(int j = 0; j < DAYS_IN_WEEK; ++j){
				if(buckets[i].bits & (1 << j)){
					inso_strcat(msg_buf, sizeof(msg_buf), days[j]);
					inso_strcat(msg_buf, sizeof(msg_buf), " ");
				}
			}

			char time_buf[32];
			snprintf(time_buf, sizeof(time_buf), "%02d:%02d] ", buckets[i].hour, buckets[i].min);
			inso_strcat(msg_buf, sizeof(msg_buf), time_buf);
		}

		free(buckets);

		localtime_r(&now, &week_start);
	}

	week_start.tm_mday -= get_dow(&week_start);
	week_start.tm_isdst = -1;
	mktime(&week_start);

	char prefix[64], suffix[64];
	strftime(prefix, sizeof(prefix), "%b %d"   , &week_start);
	strftime(suffix, sizeof(suffix), "%Z/UTC%z", &week_start);

	if(empty_week){
		ctx->send_msg(chan, "Streams resume on week of %s.", prefix);
	} else if(time_count != 0){
		ctx->send_msg(chan, "Schedule for week of %s: %s(%s)", prefix, msg_buf, suffix);
	} else {
		ctx->send_msg(chan, "Schedule for week of %s: TBA.", prefix);
	}

	tz_pop(tz);
}

static bool is_during_stream(void){
	time_t now = time(0);

	bool live = false;
	sb_each(s, schedule){
		int diff = now - *s;
		if(diff > 0 && diff < (60*90)){
			live = true;
			break;
		}
	}

	return live;
}

static intptr_t note_callback(intptr_t result, intptr_t arg){
	if(result) *(time_t*)arg = result;
	return 0;
}

static intptr_t twitch_callback(intptr_t result, intptr_t arg){
	memcpy((TwitchInfoMsg*)arg, (TwitchInfoMsg*)result, sizeof(TwitchInfoMsg));
	return 0;
}

static void print_time(const char* chan, const char* name){
	time_t now = time(0);

	// test for empty schedule, and attempt update
	{
		bool empty_sched = sb_count(schedule) == 0;
		const long lim = empty_sched ? 30 : 1800;
		if(now - last_schedule_update > lim && update_schedule()){
			last_schedule_update = now;
		}
	}

	enum { SCHED_UNKNOWN = 0, SCHED_OFF = (1 << 0), SCHED_OLD = (1 << 1) };

	int stream_duration_mins;
	{
		char* tz = tz_push(":US/Pacific");
		struct tm* lt = localtime(&now);
		const bool is_weekend = get_dow(lt) >= 5;
		stream_duration_mins = is_weekend ? 150 : 90;
		tz_pop(tz);
	}

	uint32_t schedule_flags = 0;
	int index = -1;

	sb_each(s, schedule){
		if(!*s) continue;

		if(*s == -1){
			schedule_flags |= SCHED_OFF;
		} else if(*s > (now - (stream_duration_mins*60))){
			index = s - schedule;
			break;
		} else {
			schedule_flags |= SCHED_OLD;
		}
	}

	// see if someone did a NOTE(annotator): start
	time_t note_time = 0;
	MOD_MSG(ctx, "note_get_stream_start", "#handmade_hero #hero", &note_callback, &note_time);

	// see if the stream is live and if so, when it started.
	TwitchInfoMsg twitch_info = {};
	MOD_MSG(ctx, "twitch_get_stream_info", "#handmade_hero", &twitch_callback, &twitch_info);

	int secs_into_stream;
	const char* source;

	if(now - note_time < (stream_duration_mins-15)*60){ // recent note found

		// add 15 mins since the note marks the end of the pre-stream.
		secs_into_stream = (now - note_time) + (15*60);
		source = "NOTE";

	} else {
		note_time = 0;

		if(twitch_info.start){ // no note, but stream is live.

			secs_into_stream = now - twitch_info.start;
			source = "uptime";

		} else if(index == -1){ // no note, and no streams

			if(schedule_flags & SCHED_OLD){
				ctx->send_msg(chan, "No more streams this week.");
			} else if(schedule_flags & SCHED_OFF){
				ctx->send_msg(chan, "The stream is off this week, see twitter for details.");
			} else {
				ctx->send_msg(chan, "The schedule hasn't been updated yet.");
			}

			return;

		} else { // no note, but either an upcoming stream or during one (not found by mod_twitch)
			secs_into_stream = now - schedule[index];
			source = "schedule";
		}
	}

	if(secs_into_stream < 0){ // upcoming stream
		int until = -secs_into_stream;

		if(until / (60*60*24) == 1){
			ctx->send_msg(chan, "No stream today, next one tomorrow.");
		} else if(until / (60*60*24) > 1){
			ctx->send_msg(chan, "No stream today, next one in %d days.", until / (60*60*24));
		} else {
			char  time_buf[256];
			char* time_ptr = time_buf;
			size_t time_sz = sizeof(time_buf);

			if(until >= (60*60)){
				int hours = until / (60*60);
				snprintf_chain(&time_ptr, &time_sz, "%d hour%s, ", hours, hours == 1 ? "" : "s");
				until %= (60*60);
			}

			if(time_ptr != time_buf || until >= 60){
				int mins = until / 60;
				snprintf_chain(&time_ptr, &time_sz, "%d minute%s", mins, mins == 1 ? "" : "s");
				until %= (60*60*24);
			}

			if(time_ptr == time_buf){
				sprintf(time_buf, "%d second%s", until, until == 1 ? "" : "s");
			}

			ctx->send_msg(chan, "Next stream in %s.", time_buf);
		}
	} else { // during stream
		char* format;
		int mins_in = secs_into_stream / 60;
		int mins_to_go = 15;

		int non_pre_duration = stream_duration_mins - 15;

		// account for long pre-streams
		if(note_time && twitch_info.start){
			non_pre_duration -= ((note_time - twitch_info.start) / 60 - 15);
		}

		printf("in: %d, to go: %d, msd: %d\n", mins_in, mins_to_go, non_pre_duration);

		if(mins_in < 15){
			format = "%d %s into the pre-stream Q&A. %d until start. (based on %s)";
		} else if(mins_in < non_pre_duration){
			mins_in -= 15;
			mins_to_go = non_pre_duration - 15;
			format = "%d %s into the main stream. %d until Q&A. (based on %s)";
		} else {
			mins_in -= non_pre_duration;
			format = "%d %s into the Q&A, %d until end. (based on %s)";
		}

		const char* time_unit = mins_in == 1 ? "minute" : "minutes";
		ctx->send_msg(chan, format, mins_in, time_unit, mins_to_go - mins_in, source);
	}
}

static bool check_for_alias(const char* keys, const char* chan, const char* cc){
	if(strcmp(cc, "!") == 0){
		int alias_exists = 0;
		const char* args[] = { keys, chan };
		MOD_MSG(ctx, "alias_exists", args, &check_alias_cb, &alias_exists);
		return alias_exists != 0;
	} else {
		return false;
	}
}

#define HMH_MSG(...) ({ ctx->send_msg(hmh_get_channel(), __VA_ARGS__); })

static void hmh_owlbot_start(void){
	owlbot_timer = time(0);
	owlbot_yea = owlbot_nay = 0;
	HMH_MSG("(/o.o): Owl vote started. Use !owly or !owln to vote whether or not to light The Owl and notify Casey of something important.");
}

static void hmh_fetch_latest(void){
	sb(char) data = NULL;
	CURL* curl = inso_curl_init("https://www.youtube.com/feeds/videos.xml?channel_id=UCaTznQhurW5AaiYPbhEA-KA", &data);

	curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
	curl_easy_setopt(curl, CURLOPT_TIMEVALUE, last_yt_fetch);

	long status = inso_curl_perform(curl, &data);

	if(status == 200){
		uintptr_t* tokens = calloc(0x1000, sizeof(uintptr_t));
		ixt_tokenize(data, tokens, 0x1000, IXTF_SKIP_BLANK | IXTF_TRIM);

		static const char pre[] = "handmade hero day ";
		bool found_entry = false;

		for(uintptr_t* t = tokens; *t; ++t){

			if(!found_entry && ixt_match(t, IXT_TAG_OPEN, "entry", NULL)){
				found_entry = true;
			} else if(found_entry && ixt_match(t, IXT_TAG_OPEN, "title", IXT_CONTENT, NULL)){
				const char* title = (char*)t[3];

				if(strncasecmp(title, pre, sizeof(pre)-1) == 0){
					title += sizeof(pre) - 1;
				}

				free(latest_ep_str);
				latest_ep_str = strdup(title);
				last_yt_fetch = time(0);

				break;
			}
		}

		free(tokens);
	} else if(status == 304){
		last_yt_fetch = time(0);
	}

	curl_easy_cleanup(curl);
	sb_free(data);
}

static const char* hmh_get_channel(void){
	return irc_server == SERV_TWITCH
		? "#handmade_hero"
		: irc_server == SERV_HMN
		? "#hero"
		: NULL
		;
}

static void hmh_qa_msg(void){
	const char* chan = hmh_get_channel();
	if(chan){
		ctx->send_msg(chan, "=== Q&A Session. Prefix questions with Q: ===");
	}
}

static void hmh_cmd(const char* chan, const char* name, const char* arg, int cmd){

	int* owl_vote = &owlbot_nay;

	switch(cmd){
		case CMD_SCHEDULE: {
			if(!check_for_alias("schedule sched", chan, CONTROL_CHAR)){
				print_schedule(chan, name, arg);
			}
		} break;

		case CMD_TIME: {
			if(!check_for_alias("tm time when next", chan, CONTROL_CHAR)){
				print_time(chan, name);
			}
		} break;

		case CMD_OWLBOT: {
			if(owlbot_timer || !inso_is_wlist(ctx, name))
				break;

			const char* c = hmh_get_channel();
			if(!c || strcmp(c, chan) != 0)
				break;

			ctx->send_ipc(0, "owl !", 6);
			hmh_owlbot_start();
		} break;

		case CMD_OWL_Y:
			owl_vote = &owlbot_yea;
			/* fallthru */
		case CMD_OWL_N:	{
			if(!owlbot_timer) break;

			sb_each(v, owlbot_voters){
				if(strcmp(*v, name) == 0) return;
			}

			ctx->send_ipc(0, owl_vote == &owlbot_yea ? "owl y" : "owl n", 6);
			++*owl_vote;
			sb_push(owlbot_voters, strdup(name));
		} break;

		case CMD_QA: {
			if(inso_is_wlist(ctx, name)){
				ctx->send_ipc(0, &cmd, sizeof(cmd));
				hmh_qa_msg();
			}
		} break;

		case CMD_LATEST: {
			if(time(0) - last_yt_fetch > (30*60)){
				hmh_fetch_latest();
			}

			if(latest_ep_str){
				ctx->send_msg(chan, "\035Previously on Handmade Hero...\035 [#%s]", latest_ep_str);
			} else {
				ctx->send_msg(chan, "@%s: Error fetching latest ep info :(", name);
			}
		} break;
	}
}

static void hmh_tick(time_t now){
	if(owlbot_timer && now > owlbot_timer + 60){
		if((owlbot_nay + owlbot_yea) >= 3){
			if(owlbot_yea > owlbot_nay){
				HMH_MSG("(/o.o): The owl will now be signalled. (votes: [Yea: %d, Nay: %d])", owlbot_yea, owlbot_nay);

				if(irc_server == SERV_HMN){
					size_t id = ctx->send_msg("#hero", "@Owlbot: By popular demand, please become illuminated.");
					MOD_MSG(ctx, "filter_permit", id, NULL, NULL);
				}

			} else if(owlbot_nay > owlbot_yea){
				HMH_MSG("(/x.x): The owl will not be lit. (votes: [Yea: %d, Nay: %d])", owlbot_yea, owlbot_nay);
			} else {
				HMH_MSG("(/o.o): It's a tie (%d votes each). The owl will remain unlit.", owlbot_yea);
			}
		} else {
			HMH_MSG("(/x.x): Not enough votes after 60 seconds. Owl signal cancelled.");
		}

		sb_each(v, owlbot_voters){
			free(*v);
		}
		sb_free(owlbot_voters);
		owlbot_timer = 0;
	}
}

static int ftw_cb(const char* path, const struct stat* st, int type){
	if(type & FTW_D) return 0;
	if(strncmp(path, "/usr/share/zoneinfo/posix/", 26) != 0) return 0;
	if(strstr(path, "Factory")) return 0;

	size_t plen = strlen(path) - 26;
	sb_push(tz_buf, ':');
	memcpy(sb_add(tz_buf, plen), path + 26, plen);
	sb_push(tz_buf, '.');

	return 0;
}

static bool hmh_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	ftw("/usr/share/zoneinfo/posix/", &ftw_cb, 10);
	sb_push(tz_buf, 0);

	char* id = getenv("INSOBOT_ID");
	if(id && strcmp(id, "twitch") == 0){
		irc_server = SERV_TWITCH;
	} else if(id && strcmp(id, "hmn") == 0){
		irc_server = SERV_HMN;
	}

	return true;
}

static void hmh_quit(void){
	sb_free(tz_buf);
}

static void hmh_mod_msg(const char* sender, const IRCModMsg* msg){
	if(strcmp(msg->cmd, "hmh_is_live") == 0){
		msg->callback(is_during_stream(), msg->cb_arg);
	}
}

static void hmh_ipc(int who, const uint8_t* ptr, size_t sz){
	if(sz == 6 && memcmp(ptr, "owl", 3) == 0){
		switch(ptr[4]){
			case '!': hmh_owlbot_start(); break;
			case 'y': owlbot_yea++; break;
			case 'n': owlbot_nay++; break;
		}
	} else if(sz == sizeof(int)){

		int cmd;
		memcpy(&cmd, ptr, sizeof(int));

		if(cmd == CMD_QA){
			hmh_qa_msg();
		}
	}
}
