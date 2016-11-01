#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "module.h"
#include "stb_sb.h"
#include "utils.h"
#include <curl/curl.h>
#include <ftw.h>

static void hmh_cmd     (const char*, const char*, const char*, int);
static bool hmh_init    (const IRCCoreCtx*);
static void hmh_quit    (void);
static void hmh_mod_msg (const char* sender, const IRCModMsg* msg);
static void hmh_ipc     (int who, const uint8_t* ptr, size_t sz);

enum { CMD_SCHEDULE, CMD_TIME, CMD_QA };

const IRCModuleCtx irc_mod_ctx = {
	.name       = "hmh",
	.desc       = "Functionalitty specific to Handmade Hero",
	.on_cmd     = &hmh_cmd,
	.on_init    = &hmh_init,
	.on_quit    = &hmh_quit,
	.on_mod_msg = &hmh_mod_msg,
	.on_ipc     = &hmh_ipc,
	.commands = DEFINE_CMDS (
		[CMD_SCHEDULE] = CONTROL_CHAR "sched " CONTROL_CHAR "schedule",
		[CMD_TIME]     = CONTROL_CHAR "tm "    CONTROL_CHAR "time "     CONTROL_CHAR "when " CONTROL_CHAR "next",
		[CMD_QA]       = "!qa"
	)
};

static const IRCCoreCtx* ctx;

enum { MON, TUE, WED, THU, FRI, SAT, SUN, DAYS_IN_WEEK };

static const char schedule_url[] = "https://handmadehero.org/broadcast.csv";
//static const char schedule_url[] = "http://127.0.0.1:8000/broadcast.csv";
static time_t last_schedule_update;

static struct tm schedule_start = {};
static time_t schedule[DAYS_IN_WEEK] = {};

static char* tz_buf;

static bool is_upcoming_stream(void){
	time_t now = time(0);
	for(int i = 0; i < DAYS_IN_WEEK; ++i){
		if((schedule[i] - now) > 0 || (now - schedule[i]) < 90*60){
			return true;
		}
	}
	return false;
}

// converts tm_wday which uses 0..6 = sun..sat, to 0..6 = mon..sun
static inline int get_dow(struct tm* tm){
	return tm->tm_wday ? tm->tm_wday - 1 : SUN;
}

static bool update_schedule(void){

	char* data = NULL;

	CURL* curl = inso_curl_init(schedule_url, &data);
	curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
	curl_easy_setopt(curl, CURLOPT_TIMEVALUE, (long) last_schedule_update);

	int curl_ret = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(curl);

	sb_push(data, 0);

	if(curl_ret != 0){
		fprintf(stderr, "Error getting schedule: %s\n", curl_easy_strerror(curl_ret));

		if((time(0) - mktime(&schedule_start)) > (6*24*60*60)){
			memset(schedule, 0, sizeof(schedule));
			schedule_start.tm_mday += DAYS_IN_WEEK;
			mktime(&schedule_start);
		}

		sb_free(data);
		return false;
	}

	char* tz = tz_push(":US/Pacific");

	time_t now = time(0);
	struct tm now_tm = {}, prev_tm = {};
	localtime_r(&now, &now_tm);

	if(http_code == 304){
		fprintf(stderr, "mod_hmh: Not modified.\n");
	} else {
		memset(schedule, 0, sizeof(schedule));
		memset(&schedule_start, 0, sizeof(schedule_start));

		int record_idx = -1;
		char *state, *line = strtok_r(data, "\r\n", &state);

		for(; line; line = strtok_r(NULL, "\r\n", &state)){

			struct tm scheduled_tm = {};
			char* title = strptime(line, "%Y-%m-%d,%H:%M", &scheduled_tm);
			if(*title != ','){
				break;
			}

			scheduled_tm.tm_isdst = -1;
			time_t sched = mktime(&scheduled_tm);
			int day_diff = (now - sched) / (24*60*60);

			if(record_idx >= 0){
				int idx_diff =  (get_dow(&scheduled_tm) - get_dow(&prev_tm));
				record_idx += idx_diff;
				if(record_idx >= DAYS_IN_WEEK || idx_diff < 0 || day_diff > (6*24*60*60)){
					if(is_upcoming_stream()){
						break;
					} else {
						memset(schedule, 0, sizeof(schedule));
						record_idx = -1;
					}
				}
			}

			if(record_idx < 0 && day_diff < DAYS_IN_WEEK){
				record_idx = get_dow(&scheduled_tm);
				schedule_start = scheduled_tm;
				schedule_start.tm_mday -= record_idx;
				schedule_start.tm_isdst = -1;
				mktime(&schedule_start);
			}

			if(record_idx >= 0){
				if(strcmp(title + 1, "off") == 0){
					schedule[record_idx] = -1;
				} else {
					schedule[record_idx] = sched;
				}	
			}

			prev_tm = scheduled_tm;
		}
	}

	// skip to the next week if there are no upcoming streams and it's past friday.
	struct tm cutoff_tm = schedule_start;
	cutoff_tm.tm_mday += SAT;
	cutoff_tm.tm_hour = 0;
	cutoff_tm.tm_min = 0;
	cutoff_tm.tm_isdst = -1;
	time_t cutoff = mktime(&cutoff_tm);

	if(!is_upcoming_stream() && now > cutoff){
		memset(schedule, 0, sizeof(schedule));
		schedule_start.tm_mday += DAYS_IN_WEEK;
		mktime(&schedule_start);
	}

	tz_pop(tz);

	sb_free(data);

	return true;
}

static void print_schedule(const char* chan, const char* name, const char* arg){
	time_t now = time(0);

	bool empty_sched = true;
	for(int i = 0; i < DAYS_IN_WEEK; ++i){
		if(schedule[i] != 0){
			empty_sched = false;
			break;
		}
	}

	const size_t lim = empty_sched ? 30 : 1800;

	if(now - last_schedule_update > lim){
		if(update_schedule()){
			last_schedule_update = now;
		} else if(schedule_start.tm_year == 0){
			ctx->send_msg(chan, "Error retrieving schedule :(");
			return;
		}
	}

	//FIXME: None of this crap should be done here, do it in update_schedule!
	
	enum { TIME_UNKNOWN = -1, TIME_OFF = -2 };

	struct {
		int hour;
		int min;
		uint8_t bits;
	} times[DAYS_IN_WEEK] = {};

	int time_count = 0;
	int prev_bucket = -1;
	bool terse = false;
	
	if(*arg == ' ' && strncasecmp(arg + 1, "terse", 5) == 0){
		terse = true;
		arg += 6;
	}

	char* tz;
	if(*arg++ == ' '){
		bool valid = false;
		char timezone[64];

		if(snprintf(timezone, sizeof(timezone), ":%s.", arg) < sizeof(timezone)){

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

	// group days by equal times
	for(int i = 0; i < DAYS_IN_WEEK; ++i){
		struct tm lt = {};

		switch(schedule[i]){
			case 0 : lt.tm_hour = lt.tm_min = TIME_UNKNOWN; break;
			case -1: lt.tm_hour = lt.tm_min = TIME_OFF; break;
			default: localtime_r(schedule + i, &lt); break;
		}
		
		int time_bucket = -1;

		if(terse){
			for(int j = 0; j < time_count; ++j){
				if(lt.tm_hour == times[j].hour && lt.tm_min == times[j].min){
					time_bucket = j;
					break;
				}
			}
		} else {
			if(prev_bucket != -1 && lt.tm_hour == times[prev_bucket].hour && lt.tm_min == times[prev_bucket].min){
				time_bucket = prev_bucket;
			}
		}

		if(time_bucket < 0){
			time_bucket = time_count++;
			times[time_bucket].hour = lt.tm_hour;
			times[time_bucket].min = lt.tm_min;
		}

		times[time_bucket].bits |= (1 << i);

		prev_bucket = time_bucket;
	}

	empty_sched = true;
	const char* days[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	char msg_buf[256] = {};

	for(int i = 0; i < time_count; ++i){
		if(times[i].hour == TIME_UNKNOWN) continue;
		empty_sched = false;

		inso_strcat(msg_buf, sizeof(msg_buf), "[");
		for(int j = 0; j < DAYS_IN_WEEK; ++j){
			if(times[i].bits & (1 << j)){
				inso_strcat(msg_buf, sizeof(msg_buf), days[j]);
				inso_strcat(msg_buf, sizeof(msg_buf), " ");
			}
		}

		if(times[i].hour == TIME_OFF){
			inso_strcat(msg_buf, sizeof(msg_buf), "OFF] ");
		} else {
			char time_buf[32];
			snprintf(time_buf, sizeof(time_buf), "%02d:%02d] ", times[i].hour, times[i].min);
			inso_strcat(msg_buf, sizeof(msg_buf), time_buf);
		}
	}

	char prefix[64], suffix[64];
	mktime(&schedule_start);
	strftime(prefix, sizeof(prefix), "%b %d", &schedule_start);
	strftime(suffix, sizeof(suffix), "%Z/UTC%z", &schedule_start);
	
	if(!empty_sched){
		ctx->send_msg(chan, "Schedule for week of %s: %s(%s)", prefix, msg_buf, suffix);
	} else {
		ctx->send_msg(chan, "Schedule for week of %s: TBA.", prefix);
	}

	tz_pop(tz);
}

static bool is_during_stream(void){
	time_t now = time(0);

	bool live = false;
	for(int i = 0; i < DAYS_IN_WEEK; ++i){
		int diff = now - schedule[i];
		if(diff > 0 && diff < (60*90)){
			live = true;
			break;
		}
	}

	return live;
}

static void note_callback(intptr_t result, intptr_t arg){
	if(result) *(time_t*)arg = result;
}

static void print_time(const char* chan, const char* name){
	time_t now = time(0);

	if(now - last_schedule_update > 1800){
		if(update_schedule()){
			last_schedule_update = now;
		} else if(schedule_start.tm_year == 0){
			ctx->send_msg(chan, "Error retrieving time :(");
			return;
		}
	}

	enum { SCHED_UNKNOWN = 0, SCHED_OFF = (1 << 0), SCHED_OLD = (1 << 1) };

	uint32_t schedule_flags = 0;
	int index = -1;
	for(int i = 0; i < DAYS_IN_WEEK; ++i){
		if(schedule[i] == 0) continue;

		if(schedule[i] == -1){
			schedule_flags |= SCHED_OFF;
		} else if(schedule[i] > (now - (90*60))){
			index = i;
			break;
		} else {
			schedule_flags |= SCHED_OLD;
		}
	}

	time_t note_time = 0;
	MOD_MSG(ctx, "note_get_stream_start", "#handmade_hero #hero", &note_callback, &note_time);

	int diff = now - note_time;
	if(diff < (75*60)){
		// add 15 mins since the note marks the end of the prestream.
		diff += (15*60);
	} else if(index == -1){
		if(schedule_flags & SCHED_OLD){
			ctx->send_msg(chan, "No more streams this week.");
		} else if(schedule_flags & SCHED_OFF){
			ctx->send_msg(chan, "The stream is off this week, see twitter for details.");
		} else {
			ctx->send_msg(chan, "The schedule hasn't been updated yet.");
		}
		return;
	} else {
		note_time = 0;
		diff = now - schedule[index];
	}

	if(diff < 0){
		int until = -diff;

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
	} else {
		char* format;
		int into = diff / 60;
		int duration = 15;

		if(into < 15){
			format = "%d %s into the pre-stream Q&A. %d until start. %s";
		} else if(into < 75){
			into -= 15;
			duration = 60;
			format = "%d %s into the main stream. %d until Q&A. %s";
		} else {
			into -= 75;
			format = "%d %s into the Q&A, %d until end. %s";
		}

		const char* suffix = note_time ? "(based on NOTE)" : "(based on schedule)";
		const char* time_unit = into == 1 ? "minute" : "minutes";
		ctx->send_msg(chan, format, into, time_unit, duration - into, suffix);
	}
}

static void hmh_cmd(const char* chan, const char* name, const char* arg, int cmd){

	switch(cmd){
		case CMD_SCHEDULE: {
			print_schedule(chan, name, arg);
		} break;

		case CMD_TIME: {
			print_time(chan, name);
		} break;

		case CMD_QA: {
			if(getenv("IRC_IS_TWITCH") || !inso_is_wlist(ctx, name)) return;
			ctx->send_ipc(0, &cmd, sizeof(cmd));
		} break;
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
	return true;
}

static void hmh_quit(void){
	sb_free(tz_buf);
}

static void hmh_mod_msg(const char* sender, const IRCModMsg* msg){
	if(strcmp(msg->cmd, "is_hmh_live") == 0){
		msg->callback(is_during_stream(), msg->cb_arg);
	}
}

static void hmh_ipc(int who, const uint8_t* ptr, size_t sz){
	if(!getenv("IRC_IS_TWITCH")) return;
	ctx->send_msg("#handmade_hero", "!qa");
}
