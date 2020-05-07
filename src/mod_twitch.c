#include "module.h"
#include <curl/curl.h>
#include "stb_sb.h"
#include <time.h>
#include <string.h>
#include <yajl/yajl_tree.h>
#include <ctype.h>
#include "inso_utils.h"
#include "inso_json.h"
#include "inso_tz.h"
#include "module_msgs.h"
#include <argz.h>

static bool twitch_init    (const IRCCoreCtx*);
static void twitch_cmd     (const char*, const char*, const char*, int);
static void twitch_tick    (time_t);
static bool twitch_save    (FILE*);
static void twitch_quit    (void);
static void twitch_mod_msg (const char* sender, const IRCModMsg* msg);
static void twitch_unknown (const char*, const char*, const char**, size_t);
static void twitch_modified(void);

enum { FOLLOW_NOTIFY, UPTIME, TWITCH_VOD, TWITCH_TRACKER, TWITCH_TITLE };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "twitch",
	.desc     = "Functionality specific to twitch.tv",
	.on_init  = twitch_init,
	.on_cmd   = &twitch_cmd,
	.on_tick  = &twitch_tick,
	.on_save  = &twitch_save,
	.on_quit  = &twitch_quit,
	.on_mod_msg = &twitch_mod_msg,
	.on_unknown = &twitch_unknown,
	.on_modified = &twitch_modified,
	.commands = DEFINE_CMDS (
		[FOLLOW_NOTIFY]  = CMD("fnotify"),
		[UPTIME]         = CMD("uptime" ),
		[TWITCH_VOD]     = CMD("vod"   ),
		[TWITCH_TRACKER] = CMD("tracker") CMD("streams"),
		[TWITCH_TITLE]   = CMD("title"  )
	),
	.cmd_help = DEFINE_CMDS (
		[FOLLOW_NOTIFY]  = "<on|off> | Enables/disables the twitch follow notifier, which thanks people for follwing the channel.",
		[UPTIME]         = "[#chan] | Displays the current twitch channel's uptime (or [chan]'s, if given).",
		[TWITCH_VOD]     = "[#chan] | Displays the URL of the channel's most recent VoD (past broadcast).",
		[TWITCH_TRACKER] = "[list|tagme [chans...]|untagme|chans|add <chan> [name]|del <chan>] | Operate the twitch stream tracker (#streams channel on HMN IRC)",
		[TWITCH_TITLE]   = "[title] | Displays the current channel's stream title or, if given and I am made an editor for the channel, sets it to [title]"
	)
};

static const IRCCoreCtx* ctx;

static const long uptime_check_interval = 120;
static const long follower_check_interval = 60;
static const long tracker_update_interval = 50;

static time_t last_uptime_check;
static time_t last_follower_check;
static time_t last_tracker_update;

static CURL* curl;
static struct curl_slist* twitch_headers;

enum stream_state_change {
	SSC_UNCHANGED,
	SSC_LIVE_CHANGE,
	SSC_TITLE_CHANGE,
};

// TwitchInfo -> data for uptime, follow notifier, vod + tracker

typedef struct {
	bool do_follower_notify;
	time_t last_follower_time;

	time_t stream_start;
	time_t last_uptime_check;

	enum stream_state_change live_state_changed;

	time_t prev_stream_start;

	time_t last_vod_check;
	char*  last_vod_msg;

	bool  is_tracked;
	char* tracked_name;
	char* stream_title;

	uint64_t stream_id;

	char* user_id;
} TwitchInfo;

static char**      twitch_keys;
static TwitchInfo* twitch_vals;

// TwitchTag -> holds which channels a person wants to get pinged about in the tracker

typedef struct {
	char* name;
	char* argz;
	size_t argz_len;
} TwitchTag;

enum {
	TAGS_APPEND,
	TAGS_REMOVE,
};

static char**      twitch_tracker_chans;
static TwitchTag*  twitch_tracker_tags;

static bool        first_update = true;

// TwitchUser -> cache for twitch_get_user_date mod_msg

typedef struct {
	char* name;
	time_t created_at;
} TwitchUser;

static TwitchUser* twitch_users;

static TwitchInfo* twitch_get_or_add(const char* chan){

	if(*chan != '#'){
		char* new_chan = alloca(strlen(chan) + 2);
		*new_chan = '#';
		strcpy(new_chan + 1, chan);
		chan = new_chan;
	}

	for(char** c = twitch_keys; c < sb_end(twitch_keys); ++c){
		if(strcmp(*c, chan) == 0){
			return twitch_vals + (c - twitch_keys);
		}
	}

	TwitchInfo ti = {};

	sb_push(twitch_keys, strdup(chan));
	sb_push(twitch_vals, ti);

	return twitch_vals + sb_count(twitch_vals) - 1;
}

static void twitch_load(FILE* f){
	char line[1024];
	time_t now = time(0);

	while(fgets(line, sizeof(line), f)){
		char buffer[256];
		char* arg = NULL;

		if(sscanf(line, "NOTIFY %255s", buffer) == 1){
			TwitchInfo* t = twitch_get_or_add(buffer);

			t->do_follower_notify = true;
			t->last_follower_time = now;

		} else if(sscanf(line, "TRACK %255s %m[^\n]", buffer, &arg) >= 1){
			TwitchInfo* t = twitch_get_or_add(buffer);

			t->is_tracked = true;
			t->tracked_name = arg;

		} else if(sscanf(line, "OUTPUT %255s", buffer) == 1){
			sb_push(twitch_tracker_chans, strdup(buffer));
		} else if(sscanf(line, "TAG %255s :%ms", buffer, &arg) >= 1){
			TwitchTag tag = { .name = strdup(buffer) };
			if(arg){
				argz_create_sep(arg, ':', &tag.argz, &tag.argz_len);
				free(arg);
			}
			sb_push(twitch_tracker_tags, tag);
		}
	}
}

static void twitch_modified(void){
	sb_each(t, twitch_vals){
		t->is_tracked = false;
		t->do_follower_notify = false;
		free(t->tracked_name);
		t->tracked_name = NULL;
	}

	sb_each(c, twitch_tracker_chans){
		free(*c);
	}
	sb_free(twitch_tracker_chans);

	sb_each(t, twitch_tracker_tags){
		free(t->name);
		free(t->argz);
	}
	sb_free(twitch_tracker_tags);

	FILE* f = fopen(ctx->get_datafile(), "r");
	twitch_load(f);
	fclose(f);
}

static bool twitch_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	time_t now = time(0);
	last_uptime_check = now;
	last_follower_check = now;
	last_tracker_update = now - 50;

	FILE* f = fopen(ctx->get_datafile(), "r");
	twitch_load(f);
	fclose(f);

	curl = curl_easy_init();

	twitch_headers = curl_slist_append(twitch_headers, "Accept: application/vnd.twitchtv.v5+json");

	const char* client_id = getenv("INSOBOT_TWITCH_CLIENT_ID");
	if(client_id){
		char buf[256];
		snprintf(buf, sizeof(buf), "Client-ID: %s", client_id);
		twitch_headers = curl_slist_append(twitch_headers, buf);
	}

	const char* oauth_token = getenv("INSOBOT_TWITCH_TOKEN");
	if(oauth_token){
		char buf[256];
		snprintf(buf, sizeof(buf), "Authorization: Bearer %s", oauth_token);
		twitch_headers = curl_slist_append(twitch_headers, buf);
	}

	return true;
}

static long __attribute__((format(printf, 3, 4)))
twitch_curl(char** data, long last_time, const char* fmt, ...){
	va_list v;
	va_start(v, fmt);

	char* url;
	if(vasprintf(&url, fmt, v) == -1){
		perror("vasprintf");
		abort();
	}

	va_end(v);

	*data = NULL;
	inso_curl_reset(curl, url, data);

	if(twitch_headers){
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, twitch_headers);
	}

	if(last_time){
		curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
		curl_easy_setopt(curl, CURLOPT_TIMEVALUE, last_time);
	}

	//curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, fwrite);
	//curl_easy_setopt(curl, CURLOPT_HEADERDATA, stderr);

	CURLcode ret = curl_easy_perform(curl);
	free(url);

	if(ret != 0){
		fprintf(stderr, "twitch_curl: error: %s\n", curl_easy_strerror(ret));
		sb_free(*data);
		return -1;
	}

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	if(http_code == 304){
		sb_free(*data);
	} else {
		sb_push(*data, 0);
	}

	return http_code;
}

static void twitch_resolve_user_id_bulk(int* indices, size_t count) {
	size_t total = sb_count(twitch_vals);

	char user_buf[4096];

	char*  user_buf_ptr = user_buf;
	size_t user_buf_sz = sizeof(user_buf);

	for(size_t i = 0; i < count; ++i) {
		int index = indices[i];

		if(index < 0 || index >= (int)total)
			continue;

		char* chan    = twitch_keys[index];
		TwitchInfo* t = twitch_vals + index;

		if(t->user_id)
			continue;

		snprintf_chain(&user_buf_ptr, &user_buf_sz, "&login=%s", chan+1);
	}

	char* data = NULL;
	if(twitch_curl(&data, 0, "https://api.twitch.tv/helix/users?%s", user_buf) == 200) {
		yajl_val root = yajl_tree_parse(data, NULL, 0);

		if(root) {
			yajl_val users = YAJL_GET(root, yajl_t_array, ("data"));
			if(users) {
				for(size_t i = 0; i < users->u.array.len; ++i){
					yajl_val obj = users->u.array.values[i];

					yajl_val name = YAJL_GET(obj, yajl_t_string, ("login"));
					yajl_val id   = YAJL_GET(obj, yajl_t_string, ("id"));

					if(name && id) {
						TwitchInfo* t = twitch_get_or_add(name->u.string);

						printf("mod_twitch: resolved user id [%s] = [%s]\n", name->u.string, id->u.string);

						free(t->user_id);
						t->user_id = strdup(id->u.string);
					}
				}
			}
		}

		yajl_tree_free(root);
	}

	sb_free(data);
}

static bool twitch_resolve_user_id(TwitchInfo* t) {
	int index = t - twitch_vals;
	twitch_resolve_user_id_bulk(&index, 1);
	return t->user_id;
}


static void twitch_check_uptime(size_t count, size_t* indices){
	if(count == 0) return;

	char chan_buffer[4096] = {};
	for(size_t i = 0; i < count; ++i){
		inso_strcat(chan_buffer, sizeof(chan_buffer), "&user_login=");
		inso_strcat(chan_buffer, sizeof(chan_buffer), twitch_keys[indices[i]] + 1);
	}

	char* data = NULL;
	yajl_val root = NULL;
	time_t now = time(0);

	printf("mod_twitch: doing uptime check [%s]\n", chan_buffer);

	// TODO: pagination

	long ret = twitch_curl(&data, last_uptime_check, "https://api.twitch.tv/helix/streams?%s", chan_buffer);
	last_uptime_check = now;

	if(ret == 304){
		goto unchanged;
	}

	if(ret != 200 || !(root = yajl_tree_parse(data, NULL, 0))){
		fprintf(stderr, "mod_twitch: error getting uptime. (%ld)\n", ret);
		goto unchanged;
	}

	const char* streams_path[] = { "data", NULL };
	yajl_val streams = yajl_tree_get(root, streams_path, yajl_t_array);

	if(streams){
		for(size_t i = 0; i < streams->u.array.len; ++i){
			yajl_val obj = streams->u.array.values[i];

			yajl_val name  = YAJL_GET(obj, yajl_t_string, ("user_name"));
			yajl_val start = YAJL_GET(obj, yajl_t_string, ("started_at"));
			yajl_val title = YAJL_GET(obj, yajl_t_string, ("title"));
			yajl_val id    = YAJL_GET(obj, yajl_t_number, ("id"));

			if(name && start){
				for(char* c = name->u.string; *c; ++c) {
					*c = tolower(*c);
				}

				TwitchInfo* info = twitch_get_or_add(name->u.string);

				struct tm created_tm = {};
				strptime(start->u.string, "%Y-%m-%dT%TZ", &created_tm);

				time_t new_stream_start = timegm(&created_tm);

				// state_change cleared to 0 if already live, set to 1 (SSC_LIVE_CHANGE) if not.
				info->live_state_changed = info->stream_start == 0;

				info->stream_start = new_stream_start;
				info->stream_id = YAJL_IS_INTEGER(id) ? id->u.number.i : 0;

				printf("mod_twitch: stream [%s] state_change = %d, live = %ld\n", name->u.string, info->live_state_changed, info->stream_start);

				if(title){
					if(info->stream_title){
						if(strcmp(info->stream_title, title->u.string) != 0){
							info->live_state_changed |= SSC_TITLE_CHANGE;
						}
						free(info->stream_title);
					}
					info->stream_title = strdup(title->u.string);
				}

				info->last_uptime_check = now;
			}
		}
	}

	// process all the requested channels that were not present in the twitch api response.
	for(size_t i = 0; i < count; ++i){
		TwitchInfo* t = twitch_vals + indices[i];

		if(t->last_uptime_check != now){
			// state_changed set to SSC_LIVE_CHANGE if was previously live (now offline)
			t->live_state_changed = t->stream_start != 0;

			t->prev_stream_start = t->stream_start;
			t->stream_start = 0;
			t->last_uptime_check = now;
		}
	}

	yajl_tree_free(root);
	sb_free(data);
	return;

unchanged:
	for(size_t i = 0; i < count; ++i){
		twitch_vals[indices[i]].live_state_changed = SSC_UNCHANGED;
	}
}

static bool twitch_check_live(size_t index){
	time_t now = time(0);

	TwitchInfo* t = twitch_vals + index;

	if(now - t->last_uptime_check > uptime_check_interval){
		twitch_check_uptime(1, (size_t[]){ index });
		t->last_uptime_check = now;
	}

	return t->stream_start != 0;
}

static const char* twitch_display_name(const char* fallback){
	int i = 0;
	const char *k, *v;
	static char caps_buffer[256];

	while(ctx->get_tag(i++, &k, &v)){
		if(strcmp(k, "display-name") == 0){
			if(*v) return v;
			else { // When twitch returns an empty tag, the web UI shows the first char capitalized.
				strncpy(caps_buffer, fallback, 255);
				caps_buffer[0] = toupper(caps_buffer[0]);
				return caps_buffer;
			}
		}
	}

	return fallback;
}

static intptr_t check_alias_cb(intptr_t result, intptr_t arg){
	*(int*)arg = result;
	return 0;
}

static void twitch_print_vod(size_t index, const char* send_chan, const char* name, bool check_alias){

	char* chan    = twitch_keys[index];
	TwitchInfo* t = twitch_vals + index;

	if(!twitch_resolve_user_id(t))
		return;

	char* data = NULL;
	const char url_fmt[] = "https://api.twitch.tv/helix/videos?user_id=%s&sort=time&type=archive&first=1";

	long ret = twitch_curl(&data, t->last_vod_check, url_fmt, t->user_id);
	if(ret == 304 || ret == -1){
		if(t->last_vod_msg){
			ctx->send_msg(send_chan, "%s: %s", name, t->last_vod_msg);
		}
		return;
	}

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	if(!root){
		fprintf(stderr, "twitch_print_vod: root null\n");
		goto out;
	}

	const char* videos_path[]    = { "data", NULL };

	yajl_val videos = yajl_tree_get(root, videos_path, yajl_t_array);
	if(!videos || !YAJL_IS_ARRAY(videos)){
		fprintf(stderr, "twitch_print_vod: videos null\n");
		goto out;
	}

	if(videos->u.array.len == 0){
		int alias_exists = 0;

		if(check_alias){
			const char* args[] = { "vod", send_chan };
			MOD_MSG(ctx, "alias_exists", args, &check_alias_cb, &alias_exists);
		}

		if(!alias_exists){
			ctx->send_msg(send_chan, "%s: No recent VoD found for %s.", name, chan + 1);
		}

		goto out;
	}

	yajl_val vods = videos->u.array.values[0];

	yajl_val vod_url   = YAJL_GET(vods, yajl_t_string, ("url"));
	yajl_val vod_title = YAJL_GET(vods, yajl_t_string, ("title"));
	yajl_val vod_dur   = YAJL_GET(vods, yajl_t_string, ("duration"));

	if(!vod_url){
		fprintf(stderr, "twitch_print_vod: url/date null\n");
		goto out;
	}

	if(t->last_vod_msg){
		free(t->last_vod_msg);
	}

	const char* title = vod_title->u.string ?: "untitled";
	const char* duration = vod_dur ? vod_dur->u.string : "??h??m??s";
	const char* dispname = twitch_display_name(name);

	asprintf_check(&t->last_vod_msg, "%s's last VoD: %s [%s] [%s]", chan + 1, vod_url->u.string, title, duration);
	ctx->send_msg(send_chan, "%s: %s", dispname, t->last_vod_msg);

out:
	if(data) sb_free(data);
	if(root) yajl_tree_free(root);
}

#define TWITCH_TRACKER_MSG(fmt, ...) \
	for(char** c = twitch_tracker_chans; c < sb_end(twitch_tracker_chans); ++c) ctx->send_msg(*c, fmt, __VA_ARGS__)

static void twitch_tags_print(const char* prefix, const char* stream){
	char tag_buf[512];

	for(char** chan = twitch_tracker_chans; chan < sb_end(twitch_tracker_chans); ++chan){
		*tag_buf = 0;

		int nick_count;
		const char** nicks = ctx->get_nicks(*chan, &nick_count);

		for(TwitchTag* tag = twitch_tracker_tags; tag < sb_end(twitch_tracker_tags); ++tag){
			// check if stream is one that this user wants to be tagged for, or if they want all (argz null)
			if(tag->argz){
				bool found = false;

				char* p = NULL;
				while((p = argz_next(tag->argz, tag->argz_len, p))){
					if(strcasecmp(stream, p) == 0){
						found = true;
						break;
					}
				}

				if(!found){
					continue;
				}
			}

			// check if this user is actually in the channel, and append their name to tag_buf if so
			for(int i = 0; i < nick_count; ++i){
				if(strcasecmp(tag->name, nicks[i]) == 0){
					inso_strcat(tag_buf, sizeof(tag_buf), tag->name);
					inso_strcat(tag_buf, sizeof(tag_buf), " ");
					break;
				}
			}
		}

		if(*tag_buf){
			ctx->send_msg(*chan, "%s%s", prefix, tag_buf);
		}
	}
}

static void twitch_tags_process(TwitchTag* tag, const char* chan, const char* name, int mode, const char* arg){

	// make sure tag is valid
	if(!tag){
		if(mode == TAGS_APPEND){
			TwitchTag newtag = { .name = strdup(name) };
			sb_push(twitch_tracker_tags, newtag);
			tag = &sb_last(twitch_tracker_tags);
		} else {
			ctx->send_msg(chan, "%s: You're already not tagged for any channels.", name);
			return;
		}
	}

	if(*arg){ // selective append / remove
		char* list = strdupa(arg);
		for(char* c = list; *c; ++c) *c = tolower(*c);

		char* state;
		for(char* c = strtok_r(list, " \t,#", &state); c; c = strtok_r(NULL, " \t,#", &state)){

			bool is_tracked = false;
			for(size_t i = 0; i < sb_count(twitch_keys); ++i){
				if(twitch_vals[i].is_tracked && strcasecmp(c, twitch_keys[i]+1) == 0){
					is_tracked = true;
					break;
				}
			}

			if(!is_tracked){
				// XXX: Probably should buffer these channels up and send one message at the end instead.
				//      This would prevent people making the bot spam loads of lines with a space-laden arg.
				ctx->send_msg(chan, "%s: Warning: '%s' is not being tracked, skipped.", name, c);
				continue;
			}

			char* entry = NULL;

			{ // find given stream in current list of tags
				char* p = NULL;
				while((p = argz_next(tag->argz, tag->argz_len, p))){
					if(strcmp(p, c) == 0){
						entry = p;
						break;
					}
				}
			}

			if(mode == TAGS_APPEND){
				if(entry) continue;
				argz_add(&tag->argz, &tag->argz_len, c);
			} else {
				if(!entry) continue;
				argz_delete(&tag->argz, &tag->argz_len, entry);
			}
		}
	}

	if(!*arg || !tag->argz){ // untag / tag for every channel
		free(tag->argz);
		tag->argz_len = 0;

		if(mode == TAGS_REMOVE){
			free(tag->name);
			sb_erase(twitch_tracker_tags, tag - twitch_tracker_tags);
			ctx->send_msg(chan, "%s: You will no longer be tagged for any channels.", name);
		} else {
			ctx->send_msg(chan, "%s: You'll now be tagged for all channels.", name);
		}
	} else {
		ctx->send_msg(chan, "%s: Successfully updated your tags.", name);
	}

	ctx->save_me();
}

typedef struct TwitchSchedArg {
	TwitchInfo* info;
	bool found;
} TwitchSchedArg;

static intptr_t twitch_sched_cb(SchedMsg* sched, TwitchSchedArg* arg){
	TwitchInfo* t = arg->info;

	struct tm twitch_tm;
	struct tm sched_tm;

	gmtime_r(&t->prev_stream_start, &twitch_tm);
	gmtime_r(&sched->start, &sched_tm);

	if(!sched_has_date(sched, &twitch_tm, NULL)){
		return SCHED_ITER_CONTINUE;
	}

	int twitch_mins = twitch_tm.tm_hour * 60 + twitch_tm.tm_min;
	int sched_mins = sched_tm.tm_hour * 60 + sched_tm.tm_min;
	int diff = labs(twitch_mins - sched_mins) % 1440;

	printf("sched_cb: has date t=[%d:%d] s=[%d:%d] d=%d\n", twitch_tm.tm_hour, twitch_tm.tm_min, sched_tm.tm_hour, sched_tm.tm_min, diff);

	if(diff > 70){
		return SCHED_ITER_CONTINUE;
	}

	arg->found = true;
	return SCHED_ITER_STOP;
}

static void twitch_tracker_schedule_update(TwitchInfo* t){
	const char* name = twitch_keys[t-twitch_vals]+1;
	TwitchSchedArg arg = { t };

	time_t now = time(0);

	if(now - t->prev_stream_start < (15*60)){
		return;
	}

	MOD_MSG(ctx, "sched_iter", twitch_keys[t-twitch_vals]+1, &twitch_sched_cb, &arg);
	if(arg.found){
		return;
	}

	SchedMsg new_sched = {
		.user = name,
		.start = t->prev_stream_start,
		.end = time(0),
		.title = t->stream_title,
	};

	// TODO: link to vod?
	asprintf_check(&new_sched.source, "Twitch IRC tracker");
	MOD_MSG(ctx, "sched_add", &new_sched, NULL, NULL);
}

static void twitch_tracker_update(void){
	char topic[1024] = "\002\0030,4[LIVE]\017 ";

	bool any_changed = false;
	bool any_live = false;
	bool sent_ping = false;
	bool sched_needs_save = false;

	size_t* track_indices = NULL;
	size_t index_count = 0;

	for(size_t i = 0; i < sb_count(twitch_keys); ++i){
		if(twitch_vals[i].is_tracked){
			sb_push(track_indices, i);
			index_count++;
		}
	}

	twitch_check_uptime(index_count, track_indices);
	sb_free(track_indices);

	// don't output on the first update, to avoid duplication in case the bot has been restarted.
	if(first_update){
		first_update = false;
		return;
	}

	for(size_t i = 0; i < sb_count(twitch_keys); ++i){
		const char* chan = twitch_keys[i];
		TwitchInfo* t    = twitch_vals + i;

		if(!t->is_tracked)
			continue;

		enum stream_state_change changed = t->live_state_changed;

		if(t->stream_start != 0){
			any_live = true;
			inso_strcat(topic, sizeof(topic), chan + 1);
			inso_strcat(topic, sizeof(topic), " ");

			if(changed & SSC_LIVE_CHANGE){
				if(!sent_ping){
					twitch_tags_print("FAO: ", chan + 1);
					sent_ping = true;
				}
				any_changed = true;

				const char* display_name = t->tracked_name ? t->tracked_name : chan + 1;
				TWITCH_TRACKER_MSG("\0038%s\017 is now live -\00310 http://twitch.tv/%s \017- \"%s\"", display_name, chan + 1, t->stream_title ? t->stream_title : "");
			} else if(changed & SSC_TITLE_CHANGE){
				const char* display_name = t->tracked_name ? t->tracked_name : chan + 1;
				TWITCH_TRACKER_MSG("\0033%s\017 (\00310http://twitch.tv/%s)\017 changed title: \"%s\"", display_name, chan + 1, t->stream_title ? t->stream_title : "");
			}
		} else if(changed & SSC_LIVE_CHANGE){
			any_changed = true;
			if(t->tracked_name){
				TWITCH_TRACKER_MSG("\00314%s (%s) is no longer live.", t->tracked_name, chan + 1);
			} else {
				TWITCH_TRACKER_MSG("\00314%s is no longer live.", chan + 1);
			}

			twitch_tracker_schedule_update(t);
			sched_needs_save = true;
		}
	}

	if(!any_changed){
		return;
	}

	if(sched_needs_save){
		MOD_MSG(ctx, "sched_save", NULL, NULL, NULL);
	}

	char topic_cmd[1024];
	for(char** c = twitch_tracker_chans; c < sb_end(twitch_tracker_chans); ++c){
		if(any_live){
			snprintf(topic_cmd, sizeof(topic_cmd), "TOPIC %s :\00311[!streams]\017 %s", *c, topic);
		} else {
			snprintf(topic_cmd, sizeof(topic_cmd), "TOPIC %s :\00311[!streams]\017 No streams currently live.", *c);
		}
		ctx->send_raw(topic_cmd);
	}
}

static void twitch_tracker_cmd(const char* chan, const char* name, const char* arg, int perms){

	int enabled_index = -1;
	for(char** c = twitch_tracker_chans; c < sb_end(twitch_tracker_chans); ++c){
		if(strcmp(*c, chan) == 0){
			enabled_index = c - twitch_tracker_chans;
			break;
		}
	}

	if(perms == 2){ // admin
		if(strcasecmp(arg, " enable") == 0 && enabled_index == -1){
			sb_push(twitch_tracker_chans, strdup(chan));
			ctx->send_msg(chan, "Enabled twitch tracker.");
			ctx->save_me();
			return;
		}
		if(strcasecmp(arg, " disable") == 0 && enabled_index != -1){
			free(twitch_tracker_chans[enabled_index]);
			sb_erase(twitch_tracker_chans, enabled_index);
			ctx->send_msg(chan, "Disabled twitch tracker.");
			ctx->save_me();
			return;
		}
	}

	if(enabled_index == -1) return;

	char buffer[256];
	char* optional_name = NULL;

	if(perms && sscanf(arg, " add %255s %m[^\n]", buffer, &optional_name) >= 1){

		TwitchInfo* t = twitch_get_or_add(buffer);
		t->is_tracked = true;
		if(t->tracked_name){
			free(t->tracked_name);
		}
		t->tracked_name = optional_name;

		ctx->send_msg(chan, "Now tracking channel %s", buffer);
		ctx->save_me();

	} else if(perms && sscanf(arg, " del %255s", buffer) == 1){

		TwitchInfo* t = twitch_get_or_add(buffer);
		t->is_tracked = false;

		ctx->send_msg(chan, "Untracked channel %s", buffer);
		ctx->save_me();

	} else {

		const char* dispname = twitch_display_name(name);

		TwitchTag* tag = NULL;
		for(TwitchTag* t = twitch_tracker_tags; t < sb_end(twitch_tracker_tags); ++t){
			if(strcasecmp(t->name, name) == 0){
				tag = t;
				break;
			}
		}

		if(strcmp(arg, " tagged") == 0){

			if(tag){
				if(tag->argz){
					char* list = alloca(tag->argz_len);
					memcpy(list, tag->argz, tag->argz_len);
					argz_stringify(list, tag->argz_len, ' ');
					ctx->send_msg(chan, "%s: You are tagged for: %s", dispname, list);
				} else {
					ctx->send_msg(chan, "%s: You are tagged for all channels.", dispname);
				}
			} else {
				ctx->send_msg(chan, "%s: You aren't tagged for any channels.", dispname);
			}

		} else if(strncasecmp(arg, " tagme", 6) == 0){

			twitch_tags_process(tag, chan, name, TAGS_APPEND, arg + 6);

		} else if(strncasecmp(arg, " untagme", 8) == 0){

			twitch_tags_process(tag, chan, name, TAGS_REMOVE, arg + 8);

		} else if(strcasecmp(arg, " list") == 0){

			int max_display_len = 0;
			int max_chan_len = 0;

			for(TwitchInfo* t = twitch_vals; t < sb_end(twitch_vals); ++t){
				if(!t->is_tracked || !t->stream_start) continue;

				int chan_len = strlen(twitch_keys[t - twitch_vals]) - 1;
				max_chan_len = INSO_MAX(max_chan_len, chan_len);

				if(t->tracked_name){
					max_display_len = INSO_MAX(max_display_len, (int)strlen(t->tracked_name));
				} else {
					max_display_len = INSO_MAX(max_display_len, chan_len);
				}
			}

			if(max_display_len == 0){
				ctx->send_msg(chan, "No streams are currently live.");
			} else {
				ctx->send_msg(chan, "\002\037Currently live streams");

				for(TwitchInfo* t = twitch_vals; t < sb_end(twitch_vals); ++t){
					if(!t->is_tracked || !t->stream_start) continue;
					char* channel_name = twitch_keys[t - twitch_vals];
					char* display_name = t->tracked_name ? t->tracked_name : channel_name + 1;

					ctx->send_msg(
						chan,
						"\0038%*s\017 -\00310 http://twitch.tv/%-*s \017- %s",
						max_display_len,
						display_name,
						max_chan_len,
						channel_name + 1,
						t->stream_title
					);
				}
			}

		} else if(strcasecmp(arg, " chans") == 0){

			char chan_buf[1024] = {};
			for(TwitchInfo* t = twitch_vals; t < sb_end(twitch_vals); ++t){
				if(!t->is_tracked) continue;
				inso_strcat(chan_buf, sizeof(chan_buf), twitch_keys[t - twitch_vals] + 1);
				inso_strcat(chan_buf, sizeof(chan_buf), "|");
			}
			ctx->send_msg(chan, "Tracked channels: %s", chan_buf);

		} else {
			ctx->send_msg(chan, "%s: Usage: !streams [list|tagged|chans|tagme [chans...]|untagme [chans...]|add <chan> [name]|del <chan>]", dispname);
		}
	}
}

static void twitch_get_title(const char* chan, const char* name){
	char* data = NULL;
	static const char* status_path[] = { "status", NULL };

	TwitchInfo* t = twitch_get_or_add(chan);
	if(!twitch_resolve_user_id(t)) {
		ctx->send_msg(chan, "Can't resolve user_id for channel '%s'.", chan+1);
		return;
	}

	// XXX: can't find helix version 2019-07-27
	if(twitch_curl(&data, 0, "https://api.twitch.tv/kraken/channels/%s", t->user_id) == 200){
		yajl_val root   = yajl_tree_parse(data, NULL, 0);
		yajl_val status = yajl_tree_get(root, status_path, yajl_t_string);

		if(status){
			ctx->send_msg(chan, "%s: Current title for %s: [%s].", inso_dispname(ctx, name), chan, status->u.string);
		}

		yajl_tree_free(root);
	}

	sb_free(data);
}

static void twitch_set_title(const char* chan, const char* name, const char* msg){

	TwitchInfo* t = twitch_get_or_add(chan);
	if(!twitch_resolve_user_id(t)) {
		ctx->send_msg(chan, "Can't resolve user_id for channel '%s' :(", chan);
		return;
	}

	char* title = curl_easy_escape(curl, msg, 0);

	char *url, *data;

	// XXX: can't find helix version 2019-07-27
	asprintf_check(&url , "https://api.twitch.tv/kraken/channels/%s", t->user_id);
	asprintf_check(&data, "channel[status]=%s", title);

	curl_free(title);

	char* response = NULL;
	inso_curl_reset(curl, url, &response);

	curl_easy_setopt(curl, CURLOPT_POST, 1);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, twitch_headers);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");

	CURLcode err = curl_easy_perform(curl);

	free(url);
	free(data);

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	const char* dispname = twitch_display_name(name);

	if(http_code == 200){
		ctx->send_msg(chan, "%s: Title updated successfully.", dispname);
	} else if(http_code == 403){
		ctx->send_msg(chan, "%s: I don't have permission to update the title.", dispname);
	} else {
		ctx->send_msg(chan, "%s: Error updating title for channel (%ld) \"%s\".", dispname, http_code, chan+1);
		fprintf(stderr, "response: [%s], curl=[%d] [%s]\n", response, err, curl_easy_strerror(err));
	}

	sb_free(response);
}

static void twitch_cmd(const char* chan, const char* name, const char* arg, int cmd){
	bool is_admin = strcasecmp(chan + 1, name) == 0 || inso_is_admin(ctx, name);
	bool is_wlist = is_admin || inso_is_wlist(ctx, name);

	const char* dispname = twitch_display_name(name);

	switch(cmd){
		case FOLLOW_NOTIFY: {
			if(!is_admin) return;
			if(!*arg++) break;

			TwitchInfo* t = twitch_get_or_add(chan);

			if(strcasecmp(arg, "off") == 0){
				if(t->do_follower_notify){
					ctx->send_msg(chan, "%s: Disabled follow notifier.", dispname);
					t->do_follower_notify = false;
				} else {
					ctx->send_msg(chan, "%s: It's already disabled.", dispname);
				}
			} else if(strcasecmp(arg, "on") == 0){
				if(t->do_follower_notify){
					ctx->send_msg(chan, "%s: It's already enabled.", dispname);
				} else {
					ctx->send_msg(chan, "%s: Enabled follow notifier.", dispname);
					t->do_follower_notify = true;
				}
			}
		} break;

		case UPTIME: {
			char chan_buf[64];
			char* c = chan_buf;

			if(is_wlist && *arg++){
				if(*arg != '#') *c++ = '#';
				*stpncpy(c, arg, sizeof(chan_buf)-2) = 0;
			} else {
				*stpncpy(c, chan, sizeof(chan_buf)-1) = 0;
			}

			time_t now = time(0);

			TwitchInfo* t = twitch_get_or_add(c);

			if(twitch_check_live(t - twitch_vals)){
				int minutes = (now - t->stream_start) / 60;
				char time_buf[256];
				char *time_ptr = time_buf;
				size_t time_sz = sizeof(time_buf);

				if(minutes > 60){
					int h = minutes / 60;
					snprintf_chain(&time_ptr, &time_sz, "%d hour%s, ", h, h == 1 ? "" : "s");
					minutes %= 60;
				}
				snprintf_chain(&time_ptr, &time_sz, "%d minute%s.", minutes, minutes == 1 ? "" : "s");

				ctx->send_msg(chan, "%s: The stream has been live for %s", dispname, time_buf);
			} else {
				ctx->send_msg(chan, "%s: The stream is not live.", dispname);
			}
		} break;

		case TWITCH_VOD: {
			TwitchInfo* t;
			const char* c = arg;

			if(*arg++){
				if(!is_wlist) break;
				size_t len = strlen(arg);
				char* chan_arg = alloca(len + 2);
				*chan_arg = '#';
				memcpy(chan_arg + 1, arg, len + 1);

				if(*arg == '#') chan_arg++;

				t = twitch_get_or_add(chan_arg);
			} else {
				t = twitch_get_or_add(chan);
			}

			while(*--c);
			twitch_print_vod(t - twitch_vals, chan, name, c[1] == '!');
		} break;

		case TWITCH_TRACKER: {
			twitch_tracker_cmd(chan, name, arg, is_admin + is_wlist);
		} break;

		case TWITCH_TITLE: {
			bool have_arg = *arg++ == ' ';

			if(is_admin && have_arg){
				twitch_set_title(chan, name, arg);
			} else if(is_wlist && !have_arg){
				twitch_get_title(chan, name);
			}
		} break;
	}
}

static void twitch_check_followers(void){

	char* data = NULL;
	yajl_val root = NULL;

	size_t twitch_key_count = sb_count(twitch_keys);

	int* user_index_list = calloc(twitch_key_count, sizeof(int));
	int* user_index_ptr = user_index_list;

	for(size_t i = 0; i < twitch_key_count; ++i){
		TwitchInfo* t = twitch_vals + i;

		if(!t->do_follower_notify || !twitch_check_live(i)){
			continue;
		}

		*user_index_ptr++ = i;
	}

	twitch_resolve_user_id_bulk(user_index_list, user_index_ptr - user_index_list);

	for(int* iptr = user_index_list; iptr < user_index_ptr; ++iptr) {
		int i = *iptr;

		char* chan    = twitch_keys[i];
		TwitchInfo* t = twitch_vals + i;

		if(!t->user_id) {
			continue;
		}

		static const char url[] = "https://api.twitch.tv/helix/users/follows?to_id=%s&first=10";
		long ret = twitch_curl(&data, last_follower_check, url, t->user_id);

		if(ret == 304){
			continue;
		} else if(ret == -1){
			goto out;
		}

		root = yajl_tree_parse(data, NULL, 0);

		if(!YAJL_IS_OBJECT(root)){
			fprintf(stderr, "mod_twitch: root not object!\n");
			goto out;
		}

		yajl_val follows = YAJL_GET(root, yajl_t_array, ("data"));
		if(!follows){
			fprintf(stderr, "mod_twitch: follows not array!\n");
			goto out;
		}

		char msg_buf[256] = {};
		size_t new_follow_count = 0;
		time_t new_time = t->last_follower_time;

		for(size_t j = 0; j < follows->u.array.len; ++j){
			yajl_val user = follows->u.array.values[j];

			yajl_val date = YAJL_GET(user, yajl_t_string, ("followed_at"));
			if(!date){
				fprintf(stderr, "mod_twitch date object null!\n");
				goto out;
			}

			yajl_val name = YAJL_GET(user, yajl_t_string, ("from_name"));
			if(!name){
				fprintf(stderr, "mod_twitch name object null!\n");
				goto out;
			}

			struct tm follow_tm = {};
			char* end = strptime(date->u.string, "%Y-%m-%dT%TZ", &follow_tm);
			if(!end || *end){
				fprintf(stderr, "mod_twitch wrong date format?!\n");
				goto out;
			}

			time_t follow_time = mktime(&follow_tm);

			if(follow_time > t->last_follower_time){
				++new_follow_count;
				if(j){
					inso_strcat(msg_buf, sizeof(msg_buf), ", ");
				}
				inso_strcat(msg_buf, sizeof(msg_buf), name->u.string);

				if(follow_time > new_time) new_time = follow_time;
			}
		}

		t->last_follower_time = new_time;

		if(new_follow_count == 1){
			ctx->send_msg(chan, "Thank you to %s for following the channel! <3", msg_buf);
		} else if(new_follow_count > 1){
			ctx->send_msg(chan, "Thank you new followers: %s! <3", msg_buf);
		}

		sb_free(data);
		data = NULL;

		yajl_tree_free(root);
		root = NULL;
	}

out:
	free(user_index_list);
	if(data) sb_free(data);
	if(root) yajl_tree_free(root);
}

static void twitch_tick(time_t now){

	if(now - last_tracker_update > tracker_update_interval){
//		puts("mod_twitch: tracker update...");
		twitch_tracker_update();
		last_tracker_update = now;
	}

	if(sb_count(twitch_keys) && (now - last_follower_check > follower_check_interval)){
//		puts("mod_twitch: checking new followers...");
		twitch_check_followers();
		last_follower_check = now;
	}

}

static bool twitch_save(FILE* f){
	for(TwitchInfo* t = twitch_vals; t < sb_end(twitch_vals); ++t){
		char* key = twitch_keys[t - twitch_vals];
		if(t->do_follower_notify){
			fprintf(f, "NOTIFY\t%s\n", key);
		}
		if(t->is_tracked){
			fprintf(f, "TRACK\t%s %s\n", key, t->tracked_name ?: "");
		}
	}

	for(char** chan = twitch_tracker_chans; chan < sb_end(twitch_tracker_chans); ++chan){
		fprintf(f, "OUTPUT\t%s\n", *chan);
	}

	for(TwitchTag* tag = twitch_tracker_tags; tag < sb_end(twitch_tracker_tags); ++tag){
		fprintf(f, "TAG\t%s", tag->name);

		if(tag->argz){
			fputc(' ', f);
			char* p = NULL;
			while((p = argz_next(tag->argz, tag->argz_len, p))){
				fprintf(f, ":%s", p);
			}
		}
		fputc('\n', f);
	}

	return true;
}

static void twitch_quit(void){
	for(size_t i = 0; i < sb_count(twitch_keys); ++i){
		free(twitch_keys[i]);
		free(twitch_vals[i].last_vod_msg);
		free(twitch_vals[i].stream_title);
		free(twitch_vals[i].tracked_name);
		free(twitch_vals[i].user_id);
	}
	sb_free(twitch_keys);
	sb_free(twitch_vals);

	for(char** c = twitch_tracker_chans; c < sb_end(twitch_tracker_chans); ++c) free(*c);
	sb_free(twitch_tracker_chans);

	for(TwitchTag* t = twitch_tracker_tags; t < sb_end(twitch_tracker_tags); ++t){
		free(t->name);
		free(t->argz);
	}
	sb_free(twitch_tracker_tags);

	for(TwitchUser* u = twitch_users; u < sb_end(twitch_users); ++u) free(u->name);
	sb_free(twitch_users);

	if(twitch_headers){
		curl_slist_free_all(twitch_headers);
	}

	curl_easy_cleanup(curl);
}

static TwitchUser* twitch_get_user(const char* name){

	for(size_t i = 0; i < sb_count(twitch_users); ++i){
		if(strcasecmp(name, twitch_users[i].name) == 0){
			return twitch_users + i;
		}
	}

	char* data = NULL;

	// XXX: helix api does not have created_at
	if(twitch_curl(&data, 0, "https://api.twitch.tv/kraken/users?login=%s", name) != 200) return NULL;

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	if(!root) return NULL;

	yajl_val users = YAJL_GET(root, yajl_t_array, ("users"));
	if(!users || users->u.array.len < 1) return NULL;

	yajl_val user = users->u.array.values[0];

	yajl_val created = YAJL_GET(user, yajl_t_string, ("created_at"));
	if(!created) return NULL;

	struct tm user_time = {};
	char* end = strptime(created->u.string, "%Y-%m-%dT%T", &user_time);
	if(!end || (*end != '.' && *end)) return NULL;

	TwitchUser u = {
		.name = strdup(name),
		.created_at = timegm(&user_time)
	};

	sb_push(twitch_users, u);

	return &sb_last(twitch_users);
}

static void twitch_mod_msg(const char* sender, const IRCModMsg* msg){
	if(strcmp(msg->cmd, "twitch_get_user_date") == 0){
		TwitchUser* u = twitch_get_user((char*)msg->arg);
		if(u){
			msg->callback(u->created_at, msg->cb_arg);
		}
	} else if(strcmp(msg->cmd, "twitch_is_live") == 0){
		const char* prev_p = (const char*)msg->arg;
		const char* p;
		bool live = false;

		do {
			p = strchrnul(prev_p, ' ');
			char* chan = strndupa(prev_p, p - prev_p);
			prev_p = p+1;

			TwitchInfo* t = twitch_get_or_add(chan);
			if(twitch_check_live(t - twitch_vals)){
				live = true;
				break;
			}
		} while(*p);

		msg->callback(live, msg->cb_arg);
	} else if(strcmp(msg->cmd, "display_name") == 0){

		const char* dispname = twitch_display_name((const char*)msg->arg);
		msg->callback((intptr_t)dispname, msg->cb_arg);

	} else if(strcmp(msg->cmd, "twitch_get_stream_info") == 0){
		TwitchInfo* t = twitch_get_or_add((char*)msg->arg);
		twitch_check_live(t - twitch_vals);

		TwitchInfoMsg info = {
			.id    = t->stream_id,
			.start = t->stream_start,
		};

		msg->callback((intptr_t)&info, msg->cb_arg);
	}
}

static void twitch_unknown(const char* ev, const char* origin, const char** params, size_t nparams){
	if(nparams < 2 || strcmp(ev, "USERNOTICE") != 0) return;
	const char* chan = params[0];
	const char* name = NULL;

	size_t i = 0;
	const char *k, *v;
	while(ctx->get_tag(i++, &k, &v)){
		if(strcmp(k, "msg-id") == 0 && strcmp(v, "ritual") != 0) return;
		if(strcmp(k, "msg-param-ritual-name") == 0 && strcmp(v, "new_chatter") != 0) return;
		if(strcmp(k, "display-name") == 0){
			name = v;
		}
	}

	if(name){
		ctx->send_msg(chan, "@%s: Welcome! VoHiYo", name);
	}
}
