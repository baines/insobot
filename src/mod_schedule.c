#include <time.h>
#include <string.h>
#include <ctype.h>
#include <yajl/yajl_tree.h>
#include <yajl/yajl_gen.h>
#include "module.h"
#include "module_msgs.h"
#include "stb_sb.h"
#include "inso_utils.h"
#include "inso_gist.h"
#include "inso_tz.h"

static bool sched_init (const IRCCoreCtx*);
static void sched_cmd  (const char*, const char*, const char*, int);
static void sched_quit (void);
static void sched_mod_msg (const char*, const IRCModMsg*);

enum { SCHED_ADD, SCHED_DEL, SCHED_EDIT, SCHED_SHOW, SCHED_LINK };

const IRCModuleCtx irc_mod_ctx = {
	.name        = "schedule",
	.desc        = "Stores stream schedules",
	.on_init     = &sched_init,
	.on_cmd      = &sched_cmd,
	.on_quit     = &sched_quit,
	.on_mod_msg  = &sched_mod_msg,
	.commands    = DEFINE_CMDS (
		[SCHED_ADD]  = CMD("sched+"),
		[SCHED_DEL]  = CMD("sched-"),
		[SCHED_EDIT] = CMD("schedit"),
		[SCHED_SHOW] = CMD("sched") CMD("sched?"),
		[SCHED_LINK] = CMD("schedlist")
	)
};

static const IRCCoreCtx* ctx;

typedef struct {
	time_t start;
	time_t end;
	char* title;
	uint8_t repeat;
} SchedEntry;

static char**       sched_keys;
static SchedEntry** sched_vals;
static inso_gist*   gist;

enum { MON, TUE, WED, THU, FRI, SAT, SUN, DAYS_IN_WEEK };

static int get_dow(struct tm* tm){
	return tm->tm_wday ? tm->tm_wday - 1 : SUN;
}

static void sched_free(void){
	for(int i = 0; i < sb_count(sched_keys); ++i){
		for(int j = 0; j < sb_count(sched_vals[i]); ++j){
			free(sched_vals[i][j].title);
		}
		sb_free(sched_vals[i]);
		free(sched_keys[i]);
	}
	sb_free(sched_keys);
	sb_free(sched_vals);
}

static int sched_get(const char* name){
	int i;
	for(i = 0; i < sb_count(sched_keys); ++i){
		if(strcmp(sched_keys[i], name) == 0){
			return i;
		}
	}

	sb_push(sched_keys, strdup(name));
	sb_push(sched_vals, 0);

	return i;
}

static bool sched_reload(void){
	inso_gist_file* files = NULL;
	int ret = inso_gist_load(gist, &files);

	if(ret == INSO_GIST_304){
		puts("mod_schedule: not modified.");
		return true;
	}
	
	if(ret != INSO_GIST_OK){
		puts("mod_schedule: gist error.");
		return false;
	}

	puts("mod_schedule: doing full reload");

	char* data = NULL;
	for(inso_gist_file* f = files; f; f = f->next){
		if(strcmp(f->name, "schedule.json") != 0){
			continue;
		} else {
			data = f->content;
			break;
		}
	}

	if(!data){
		puts("mod_schedule: data null?!");
		return false;
	}

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	if(!YAJL_IS_ARRAY(root)){
		puts("mod_schedule: data is not an array?!");
		return false;
	}
	
	sched_free();

	enum { K_USER, K_START, K_END, K_TITLE, K_REPEAT };

	struct {
		const char** path;
		yajl_type type;
	} keys[] = {
		{ (const char*[]){ "user"  , NULL }, yajl_t_string },
		{ (const char*[]){ "start" , NULL }, yajl_t_string },
		{ (const char*[]){ "end"   , NULL }, yajl_t_string },
		{ (const char*[]){ "title" , NULL }, yajl_t_string },
		{ (const char*[]){ "repeat", NULL }, yajl_t_number },
	};

	for(int i = 0; i < root->u.array.len; ++i){
		yajl_val vals[5];
		for(int j = 0; j < ARRAY_SIZE(vals); ++j){
			vals[j] = yajl_tree_get(root->u.array.values[i], keys[j].path, keys[j].type);
			if(!vals[j]) printf("mod_schedule: parse error %d/%d\n", i, j);
		}

		SchedEntry entry = {
			.title  = strdup(vals[K_TITLE]->u.string),
			.repeat = vals[K_REPEAT]->u.number.i,
		};

		struct tm tm = {};

		strptime(vals[K_START]->u.string, "%FT%TZ", &tm);
		entry.start = mktime(&tm);

		strptime(vals[K_END]->u.string, "%FT%TZ", &tm);
		entry.end = mktime(&tm);

		int diff = (entry.end - entry.start) / 60;
		localtime_r(&entry.start, &tm);
		printf("mod_schedule: got entry: [%s] [%02d:%02d] [%dmin] [%x] [%s]\n", vals[0]->u.string, tm.tm_hour, tm.tm_min, diff, entry.repeat, entry.title);

		int index = sched_get(vals[0]->u.string);
		sb_push(sched_vals[index], entry);
	}

	yajl_tree_free(root);
	inso_gist_file_free(files);

	return true;
}

static bool sched_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	char* gist_id = getenv("INSOBOT_SCHED_GIST_ID");
	if(!gist_id || !*gist_id){
		fputs("mod_schedule: INSOBOT_SCHED_GIST_ID undefined, can't continue.\n", stderr);
		return false;
	}

	char* gist_user = getenv("INSOBOT_GIST_USER");
	if(!gist_user || !*gist_user){
		fputs("mod_schedule: No INSOBOT_GIST_USER env, can't continue.\n", stderr);
		return false;
	}
	
	char* gist_token = getenv("INSOBOT_GIST_TOKEN");
	if(!gist_token || !*gist_token){
		fputs("mod_schedule: No INSOBOT_GIST_TOKEN env, can't continue.\n", stderr);
		return false;
	}

	gist = inso_gist_open(gist_id, gist_user, gist_token);
	return sched_reload();
}

static void sched_upload(void){
	inso_gist_file* file = NULL;

	yajl_gen json = yajl_gen_alloc(NULL);
	yajl_gen_config(json, yajl_gen_beautify, 1);

	char time_buf[256] = {};

	yajl_gen_array_open(json);
	for(int i = 0; i < sb_count(sched_keys); ++i){
		for(int j = 0; j < sb_count(sched_vals[i]); ++j){
			yajl_gen_map_open(json);

			yajl_gen_string(json, "user", 4);
			yajl_gen_string(json, sched_keys[i], strlen(sched_keys[i]));

			strftime(time_buf, sizeof(time_buf), "%FT%TZ", gmtime(&sched_vals[i][j].start));
			yajl_gen_string(json, "start", 5);
			yajl_gen_string(json, time_buf, strlen(time_buf));

			strftime(time_buf, sizeof(time_buf), "%FT%TZ", gmtime(&sched_vals[i][j].end));
			yajl_gen_string(json, "end", 3);
			yajl_gen_string(json, time_buf, strlen(time_buf));

			yajl_gen_string(json, "title", 5);
			yajl_gen_string(json, sched_vals[i][j].title, strlen(sched_vals[i][j].title));

			yajl_gen_string(json, "repeat", 6);
			yajl_gen_integer(json, sched_vals[i][j].repeat);

			yajl_gen_map_close(json);
		}
	}
	yajl_gen_array_close(json);

	size_t json_len = 0;
	const unsigned char* json_data = NULL;
	yajl_gen_get_buf(json, &json_data, &json_len);
	inso_gist_file_add(&file, "schedule.json", json_data);
	yajl_gen_free(json);

	inso_gist_save(gist, "insobot stream schedule", file);
	inso_gist_file_free(file);
}

static void sched_add(const char* chan, const char* name, const char* _arg){
	if(!*_arg++){
		ctx->send_msg(
			chan,
			"%s: usage: " CONTROL_CHAR "sched+ [#chan] [days] [<HH:MM>[-HH:MM][TZ]] [Title]. "
			"'days' can be a list like 'mon,tue,fri', strings like 'daily', 'weekends' etc, or a date like '2016-03-14'.",
			name
		);
		return;
	}

	char* arg_state;
	char* arg = strtok_r(strdupa(_arg), " \t", &arg_state);
	if(!arg){
		ctx->send_msg(chan, "Unable to parse time.");
		return;
	}

	// parse optional channel

	char sched_user[64];
	if(sscanf(arg, "#%63s", sched_user) == 1){
		if(!(arg = strtok_r(NULL, " \t", &arg_state))){
			ctx->send_msg(chan, "Unable to parse time.");
			return;
		}
	} else {
		*stpncpy(sched_user, name, sizeof(sched_user)) = '\0';
	}

	for(char* c = sched_user; *c; ++c){
		*c = tolower(*c);
	}

	// parse days spec, special tokens

	time_t now = time(0);

	struct tm utc;
	gmtime_r(&now, &utc);
	utc.tm_hour = 0;
	utc.tm_min = 0;
	utc.tm_sec = 0;

	struct {
		const char* text;
		int repeat_val;
	} repeat_tokens[] = {
		{ "today"   , 0    },
		{ "daily"   , 0x7F },
		{ "weekdays", 0x1F },
		{ "weekends", 0x60 },
		{ "weekly"  , 1 << get_dow(&utc) },
	};

	SchedEntry sched = {};
	sched.start = sched.end = timegm(&utc);

	bool found_days = false;

	for(int i = 0; i < ARRAY_SIZE(repeat_tokens); ++i){
		if(strcasecmp(arg, repeat_tokens[i].text) == 0){
			sched.repeat = repeat_tokens[i].repeat_val;
			found_days = true;
			break;
		}
	}

	// parse days spec, comma separated days (if no special token found)

	const char* days[] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };

	if(!found_days){
		char* day_state;
		char* day = strtok_r(arg, ",", &day_state);
		while(day){
			for(int i = 0; i < ARRAY_SIZE(days); ++i){
				if(strcasecmp(day, days[i]) == 0){
					sched.repeat |= (1 << i);
					found_days = true;
					break;
				}
			}
			day = strtok_r(NULL, ",", &day_state);
		}
	}

	// if found, make sure the start date is on one of the repeat days

	if(sched.repeat){
		int today = get_dow(&utc);
		if(!(sched.repeat & (1 << today))){
			for(int i = 0; i < 7; ++i){
				if(sched.repeat & (1 << i)){
					int diff = i - today;
					sched.start += (60*60*24*diff);
					sched.end   += (60*60*24*diff);
					break;
				}
			}
		}
	}

	// parse days spec, explicit date.

	if(!found_days){
		struct tm start_date = {};
		char* p = strptime(arg, "%F", &start_date);
		if(p && p != arg && !*p){
			start_date.tm_hour = 0;
			start_date.tm_min = 0;
			sched.start = timegm(&start_date);
			found_days = true;
		}
	}

	if(found_days){
		if(!(arg = strtok_r(NULL, " \t", &arg_state))){
			ctx->send_msg(chan, "Unable to parse time.");
			return;
		}
	}

	// parse timestamp

	int time_pieces[4] = {};
	int read_count[2] = {};

	int time_count = sscanf(
		arg,
		"%d:%d%n-%d:%d%n",
		time_pieces + 0,
		time_pieces + 1,
		read_count  + 0,
		time_pieces + 2,
		time_pieces + 3,
		read_count  + 1
	);

	if(time_count == 0){
		ctx->send_msg(chan, "Unable to parse time.");
		return;
	}

	if(time_count <= 2){
		time_pieces[2] = time_pieces[0] + 1;
		time_pieces[3] = time_pieces[1];
	}

	int start_mins = (time_pieces[0]*60) + time_pieces[1];
	int end_mins   = (time_pieces[2]*60) + time_pieces[3];

	if(end_mins < start_mins){
		end_mins += (24*60);
	}

	sched.start += start_mins * 60;
	sched.end   += end_mins   * 60;

	// parse timezone

	if(time_count == 2 || time_count == 4){
		const char* tz_name = arg + read_count[time_count / 4];
		int tz_offset;

		if(tz_name && tz_abbr2off(tz_name, &tz_offset)){
			sched.start -= (tz_offset * 60);
			sched.end   -= (tz_offset * 60);
		}
	}

	// parse title

	char* title = NULL;

	while((arg = strtok_r(NULL, " \t", &arg_state))){
		if(title){
			sb_push(title, ' ');
		}
		size_t len = strlen(arg);
		memcpy(sb_add(title, len), arg, len);
	}

	if(!title){
		sched.title = strdup("Untitled stream");
	} else {
		sched.title = strndup(title, sb_count(title));
		sb_free(title);
	}

	ctx->send_msg(chan, "Added schedule for %s's [%s] stream.", sched_user, sched.title);

	// add it

	int index = sched_get(sched_user);
	sb_push(sched_vals[index], sched);

	sched_upload();
}

static void sched_del(const char* chan, const char* name, const char* arg){
	if(!*arg++){
		ctx->send_msg(chan, "%s: usage: " CONTROL_CHAR "sched- [#chan] <schedule_id>", name);
		return;
	}

	char sched_user[64];
	if(sscanf(arg, "#%63s ", sched_user) == 1){
		arg += strlen(sched_user) + 2;
	} else {
		*stpncpy(sched_user, name, sizeof(sched_user)) = '\0';
	}

	for(char* c = sched_user; *c; ++c){
		*c = tolower(*c);
	}

	int index = -1;
	for(int i = 0; i < sb_count(sched_keys); ++i){
		if(strcmp(sched_keys[i], sched_user) == 0){
			index = i;
			break;
		}
	}

	if(index == -1){
		ctx->send_msg(chan, "%s: I don't have any schedule info for '%s'", name, sched_user);
		return;
	}

	int id = strtol(arg, NULL, 0);
	if(id < 0 || id >= sb_count(sched_vals[index])){
		ctx->send_msg(chan, "%s: %s has %d schedules. I can't delete number %d.", name, sched_user, sb_count(sched_vals[index]), id);
		return;
	}

	free(sched_vals[index][id].title);
	sb_erase(sched_vals[index], id);
	if(sb_count(sched_vals[index]) == 0){
		free(sched_keys[index]);
		sb_free(sched_vals[index]);

		sb_erase(sched_keys, index);
		sb_erase(sched_vals, index);
	}

	sched_upload();

	ctx->send_msg(chan, "%s: Deleted %s's schedule #%d.", name, sched_user, id);
}

static void sched_show(const char* chan, const char* name, const char* arg){
	const char* sched_user = name;

	if(arg[0] == ' ' && arg[1] == '#'){
		sched_user = arg + 2;
	} else if(arg[0]){
		sched_user = arg + 1;
	}

	int index = -1;
	for(int i = 0; i < sb_count(sched_keys); ++i){
		if(strcasecmp(sched_keys[i], sched_user) == 0){
			index = i;
			break;
		}
	}

	if(index == -1){
		ctx->send_msg(chan, "%s: I don't have any schedules for %s", name, sched_user);
		return;
	}

	char* sched_buf = NULL;
	for(int i = 0; i < sb_count(sched_vals[index]); ++i){
		if(sched_buf){
			sb_push(sched_buf, ' ');
		}

		char tmp[256];
		int len = snprintf(tmp, sizeof(tmp), "[%d: %s]", i, sched_vals[index][i].title);
		if(len >= sizeof(tmp)){
			len = sizeof(tmp) - 1;
		}
		memcpy(sb_add(sched_buf, len), tmp, len);
	}
	sb_push(sched_buf, 0);

	ctx->send_msg(chan, "%s: %s's schedules: %s", name, sched_user, sched_buf);
	sb_free(sched_buf);
}

static void sched_cmd(const char* chan, const char* name, const char* arg, int cmd){
	switch(cmd){
		case SCHED_ADD: {
			if(inso_is_wlist(ctx, name)){
				inso_gist_lock(gist);
				sched_reload();
				sched_add(chan, name, arg);
				inso_gist_unlock(gist);
			}
		} break;
		
		case SCHED_DEL: {
			if(inso_is_wlist(ctx, name)){
				inso_gist_lock(gist);
				sched_reload();
				sched_del(chan, name, arg);
				inso_gist_unlock(gist);
			}
		} break;

		case SCHED_SHOW: {
			if(inso_is_wlist(ctx, name)){
				sched_reload();
				sched_show(chan, name, arg);
			}
		} break;

		case SCHED_LINK: {
			ctx->send_msg(chan, "%s: You can view all known schedules here: https://abaines.me.uk/insobot/schedule/", name);
		} break;
	}
}

static void sched_quit(void){
	sched_free();
	inso_gist_close(gist);
}

static void sched_mod_msg(const char* sender, const IRCModMsg* msg){
	if(strcmp(msg->cmd, "sched_get") == 0){
		const char* name = (const char*)msg->arg;

		int index = -1;
		for(int i = 0; i < sb_count(sched_keys); ++i){
			if(strcmp(sched_keys[i], name) == 0){
				index = i;
				break;
			}
		}

		if(index != -1){
			SchedMsg result = {
				.user = sched_keys[index],
			};

			for(int i = 0; i < sb_count(sched_keys[index]); ++i){
				result.sched_id = i;
				result.start  = sched_vals[index][i].start;
				result.end    = sched_vals[index][i].end;
				result.title  = sched_vals[index][i].title;
				result.repeat = sched_vals[index][i].repeat;

				msg->callback((intptr_t)&result, msg->cb_arg);
			}
		}

		return;
	}

	// TODO
	if(strcmp(msg->cmd, "sched_set") == 0){
		SchedMsg* request = (SchedMsg*)msg->arg;

		return;
	}
}
