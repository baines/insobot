#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "module.h"
#include "stb_sb.h"
#include "utils.h"
#include <curl/curl.h>

static void hmh_cmd  (const char*, const char*, const char*, int);
static bool hmh_init (const IRCCoreCtx*);

enum { CMD_SCHEDULE, CMD_TIME };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "hmh",
	.desc     = "Functionalitty specific to Handmade Hero",
	.on_cmd   = &hmh_cmd,
	.on_init  = &hmh_init,
	.commands = DEFINE_CMDS (
		[CMD_SCHEDULE] = "!sched \\sched \\schedule",
		[CMD_TIME]     = "!tm    \\tm    \\time"
	)
};

static const IRCCoreCtx* ctx;

enum { MON, TUE, WED, THU, FRI, SAT, SUN, DAYS_IN_WEEK };

static const char schedule_url[] = "https://handmadehero.org/broadcast.csv";
//static const char schedule_url[] = "http://127.0.0.1:8000/broadcast.csv";
static time_t last_schedule_update;

static struct tm schedule_start = {};
static time_t schedule[DAYS_IN_WEEK] = {};

static size_t curl_callback(char* ptr, size_t sz, size_t nmemb, void* data){
	char** out = (char**)data;
	const size_t total = sz * nmemb;

	memcpy(sb_add(*out, total), ptr, total);

	return total;
}

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

	CURL* curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, schedule_url);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "insobot");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
	curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
	curl_easy_setopt(curl, CURLOPT_TIMEVALUE, (long) last_schedule_update);

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

	if(http_code == 304){
		fprintf(stderr, "mod_hmh: Not modified.\n");
		sb_free(data);
		return true;
	}

	memset(schedule, 0, sizeof(schedule));
	memset(&schedule_start, 0, sizeof(schedule_start));

	char* tz = tz_push(":US/Pacific");

	time_t now = time(0);
	struct tm now_tm = {}, prev_tm = {};
	localtime_r(&now, &now_tm);

	int record_idx = -1;
	char *state, *line = strtok_r(data, "\r\n", &state);

	for(; line; line = strtok_r(NULL, "\r\n", &state)){

		struct tm scheduled_tm = {};
		char* title = strptime(line, "%Y-%m-%d,%H:%M", &scheduled_tm);
		if(*title != ','){
			break;
		}

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

	// skip to the next week if there are no upcoming streams and it's past friday.
	struct tm cutoff_tm = schedule_start;
	cutoff_tm.tm_mday += FRI;
	cutoff_tm.tm_hour = 17;
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

static void print_schedule(const char* chan, const char* name){
	time_t now = time(0);

	if(now - last_schedule_update > 1800){
		if(update_schedule()) last_schedule_update = now;
	}

	//FIXME: None of this crap should be done here, do it in update_schedule!
	
	enum { TIME_UNKNOWN = -1, TIME_OFF = -2 };

	struct {
		int hour;
		int min;
		uint8_t bits;
	} times[DAYS_IN_WEEK] = {};

	int time_count = 0;

	char* tz = tz_push(":US/Pacific");
	
	// group days by equal times
	for(int i = 0; i < DAYS_IN_WEEK; ++i){
		struct tm lt = {};

		switch(schedule[i]){
			case 0 : lt.tm_hour = lt.tm_min = TIME_UNKNOWN; break;
			case -1: lt.tm_hour = lt.tm_min = TIME_OFF; break;
			default: localtime_r(schedule + i, &lt); break;
		}
		
		int time_bucket = -1;

		for(int j = 0; j < time_count; ++j){
			if(lt.tm_hour == times[j].hour && lt.tm_min == times[j].min){
				time_bucket = j;
				break;
			}
		}

		if(time_bucket < 0){
			time_bucket = time_count++;
			times[time_bucket].hour = lt.tm_hour;
			times[time_bucket].min = lt.tm_min;
		}

		times[time_bucket].bits |= (1 << i);
	}

	bool something_scheduled = false;
	const char* days[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	char msg_buf[256] = {};

	for(int i = 0; i < time_count; ++i){
		if(times[i].hour == TIME_UNKNOWN) continue;
		something_scheduled = true;

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
	strftime(prefix, sizeof(prefix), "%b %d", &schedule_start);
	strftime(suffix, sizeof(suffix), "%Z/UTC%z", &schedule_start);
	
	if(something_scheduled){
		ctx->send_msg(chan, "Schedule for week of %s: %s(%s)", prefix, msg_buf, suffix);
	} else {
		ctx->send_msg(chan, "Schedule for week of %s: TBA.", prefix);
	}

	tz_pop(tz);
}

static void print_time(const char* chan, const char* name){
	time_t now = time(0);

	if(now - last_schedule_update > 1800){
		if(update_schedule()) last_schedule_update = now;
	}

	bool found = false;
	enum { SEC_IN_MIN = 60, SEC_IN_HOUR = 60*60, SEC_IN_DAY = 24*60*60 };

	for(int i = 0; i < DAYS_IN_WEEK; ++i){
		
		int live_test = now - schedule[i];
		if(live_test > 0 && live_test < SEC_IN_HOUR){
			found = true;

			int m = live_test / SEC_IN_MIN;
			ctx->send_msg(
				chan,
				"%d minute%s into the stream, %d until Q&A. (if Casey is on schedule)",
				m,
				m > 1 ? "s" : "",
				60 - m
			);
			break;
		}

		if(live_test >= SEC_IN_HOUR && live_test < (SEC_IN_HOUR*1.5)){
			found = true;

			int m = (live_test - SEC_IN_HOUR) / SEC_IN_MIN;
			ctx->send_msg(
				chan,
				"%d minute%s into the Q&A, %d until end. (if Casey is on schedule)",
				m,
				m > 1 ? "s" : "",
				30 - m
			);
			break;
		}

		if(schedule[i] > now){
			found = true;

			int diff = schedule[i] - now;

			if(diff < SEC_IN_MIN){
				ctx->send_msg(chan, "Next stream in %d second%s.", diff, diff > 1 ? "s" : "");
				break;
			}

			if(diff < SEC_IN_HOUR){
				diff /= SEC_IN_MIN;
				ctx->send_msg(chan, "Next stream in %d minute%s.", diff, diff > 1 ? "s" : "");
				break;
			}

			if(diff < SEC_IN_DAY){
				int h = diff / SEC_IN_HOUR;
				int m = (diff % SEC_IN_HOUR) / SEC_IN_MIN;
				ctx->send_msg(
					chan,
					"Next stream in %d hour%s, %d minute%s.",
					h,
					h > 1 ? "s" : "",
					m,
					m > 1 ? "s" : ""
				);
				break;
			}

			int d = diff / SEC_IN_DAY;
			ctx->send_msg(chan, "Next stream in %d day%s.", d, d > 1 ? "s" : "");
			break;
		}
	}

	if(!found){
		ctx->send_msg(chan, "No more streams scheduled, try checking handmadehero.org or @handmade_hero on twitter");
	}

}

static void hmh_cmd(const char* chan, const char* name, const char* msg, int cmd){

	switch(cmd){
		case CMD_SCHEDULE: {
			print_schedule(chan, name);
		} break;

		case CMD_TIME: {
			print_time(chan, name);
		} break;
	}
}

static bool hmh_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}
