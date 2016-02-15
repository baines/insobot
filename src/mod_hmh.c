#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "module.h"
#include "stb_sb.h"
#include <curl/curl.h>

static void hmh_msg  (const char*, const char*, const char*);
static bool hmh_init (const IRCCoreCtx*);

const IRCModuleCtx irc_mod_ctx = {
	.name     = "hmh",
	.desc     = "Functionalitty specific to Handmade Hero",
	.on_msg   = &hmh_msg,
	.on_init  = &hmh_init,
};

static const IRCCoreCtx* ctx;

enum { SCHEDULE_DAYS = 7 };

static const char schedule_url[] = "https://handmadehero.org/broadcast.csv";
//static const char schedule_url[] = "http://127.0.0.1:8000/broadcast.csv";
static time_t last_schedule_update;

static struct tm schedule_start = {};
static time_t schedule[SCHEDULE_DAYS] = {};

static size_t curl_callback(char* ptr, size_t sz, size_t nmemb, void* data){
	char** out = (char**)data;
	const size_t total = sz * nmemb;

	memcpy(sb_add(*out, total), ptr, total);

	return total;
}

char* tz_push(const char* tz){
	char* oldtz = getenv("TZ");
	if(oldtz) oldtz = strdup(oldtz);

	setenv("TZ", tz, 1);
	tzset();

	return oldtz;
}

void tz_pop(char* oldtz){
	if(oldtz){
		setenv("TZ", oldtz, 1);
		free(oldtz);
	} else {
		unsetenv("TZ");
	}
	tzset();
}

static bool is_upcoming_stream(void){
	time_t now = time(0);
	for(int i = 0; i < SCHEDULE_DAYS; ++i){
		if((schedule[i] - now) > 0 || (now - schedule[i]) < 90*60){
			return true;
		}
	}
	return false;
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

	int curl_ret = curl_easy_perform(curl);

	curl_easy_cleanup(curl);

	sb_push(data, 0);

	if(curl_ret != 0){
		fprintf(stderr, "Error getting schedule: %s\n", curl_easy_strerror(curl_ret));
		return false;
	}

	memset(schedule, 0, sizeof(schedule));
	memset(&schedule_start, 0, sizeof(schedule_start));

	// this follows the struct tm format of sunday being 0, but schedule uses monday as 0
	enum { SUN, MON, TUE, WED, THU, FRI, SAT, DAYS_IN_WEEK };
	
	char* tz = tz_push(":US/Pacific");

	time_t our_time_t = time(0);
	struct tm scheduled_time = {};

	int record_idx = -1;
	char *state, *line = strtok_r(data, "\r\n", &state);

	for(; line; line = strtok_r(NULL, "\r\n", &state)){

		char* title = strptime(line, "%Y-%m-%d,%H:%M", &scheduled_time);
		if(*title != ','){
			break;
		}

		time_t sched_time_t = mktime(&scheduled_time);
		
		if(record_idx >= 0){
			record_idx += roundf((sched_time_t - schedule[record_idx]) / (float)(24*60*60));
			if(record_idx >= DAYS_IN_WEEK){
				if(is_upcoming_stream()){
					break;
				} else {
					memset(schedule, 0, sizeof(schedule));
					record_idx = -1;
				}
			}
		}

		int day_diff = (our_time_t - sched_time_t) / (24*60*60);
		
		if(record_idx < 0 && day_diff < DAYS_IN_WEEK){
			record_idx = scheduled_time.tm_wday ? scheduled_time.tm_wday - 1 : SUN;
			schedule_start = scheduled_time;
			schedule_start.tm_mday -= record_idx;
 			schedule_start.tm_isdst = -1;
			mktime(&schedule_start);
		}

		if(record_idx >= 0){
			if(strcmp(title + 1, "off") == 0){
				schedule[record_idx] = -1;
			} else {
				schedule[record_idx] = sched_time_t;
			}	
		}
	}

	// skip to the next week if there are no upcoming streams.
	if(!is_upcoming_stream()){
		memset(schedule, 0, sizeof(schedule));
		localtime_r(&our_time_t, &schedule_start);
		int day_off = schedule_start.tm_wday ? 8 - schedule_start.tm_wday : 1;
		schedule_start.tm_mday += day_off;
	}

	tz_pop(tz);

	sb_free(data);

	return true;
}

// returns num bytes copied, or -(num reuired) and doesn't copy anything if not enough space.
static int inso_strcat(char* buf, size_t sz, const char* str){
	char* p = buf;
	while(*p) ++p;

	sz -= (p - buf);

	size_t len = strlen(str) + 1;

	if(len <= sz){
		memcpy(p, str, len);
		return len;
	} else {
		return sz - len;
	}
}

static void print_schedule(const char* chan, const char* name, const char* msg){
	time_t now = time(0);

	if(now - last_schedule_update > 3600){
		if(update_schedule()) last_schedule_update = now;
	}

	//FIXME: None of this crap should be done here, do it in update_schedule!
	
	enum { TIME_UNKNOWN = -1, TIME_OFF = -2 };

	struct {
		int hour;
		int min;
		uint8_t bits;
	} times[SCHEDULE_DAYS] = {};

	int time_count = 0;

	char* tz = tz_push(":US/Pacific");
	
	// group days by equal times
	for(int i = 0; i < SCHEDULE_DAYS; ++i){
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
		for(int j = 0; j < SCHEDULE_DAYS; ++j){
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

static void print_time(const char* chan, const char* name, const char* msg){
	time_t now = time(0);

	if(now - last_schedule_update > 3600){
		if(update_schedule()) last_schedule_update = now;
	}

	bool found = false;
	enum { SEC_IN_MIN = 60, SEC_IN_HOUR = 60*60, SEC_IN_DAY = 24*60*60 };

	for(int i = 0; i < SCHEDULE_DAYS; ++i){
		
		int live_test = now - schedule[i];
		if(live_test > 0 && live_test < SEC_IN_HOUR){
			found = true;

			int m = live_test / SEC_IN_MIN;
			int qa = 60 - m;
			ctx->send_msg(chan, "%d minutes into the stream, %d until Q&A. (if Casey is on schedule)", m, qa);
			break;
		}

		if(live_test >= SEC_IN_HOUR && live_test < (SEC_IN_HOUR*1.5)){
			found = true;

			int m = (live_test - SEC_IN_HOUR) / SEC_IN_MIN;
			int e = 30 - m;
			ctx->send_msg(chan, "%d minutes into the Q&A, %d until end. (if Casey is on schedule)", m, e);
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

static void hmh_msg(const char* chan, const char* name, const char* msg){
	if(strcasecmp(msg, "!sched") == 0){
		print_schedule(chan, name, msg);
	}

	if(strcasecmp(msg, "!tm") == 0){
		print_time(chan, name, msg);
	}
}

static bool hmh_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}
