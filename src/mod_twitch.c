#include "module.h"
#include <curl/curl.h>
#include "stb_sb.h"
#include <time.h>
#include <string.h>
#include <yajl/yajl_tree.h>
#include "utils.h"

static bool twitch_init    (const IRCCoreCtx*);
static void twitch_cmd     (const char*, const char*, const char*, int);
static void twitch_tick    (void);
static bool twitch_save    (FILE*);
static void twitch_quit    (void);
static void twitch_mod_msg (const char* sender, const IRCModMsg* msg);

enum { FOLLOW_NOTIFY, UPTIME, TWITCH_VOD, TWITCH_TRACKER };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "twitch",
	.desc     = "Functionality specific to twitch.tv",
	.on_init  = twitch_init,
	.on_cmd   = &twitch_cmd,
	.on_tick  = &twitch_tick,
	.on_save  = &twitch_save,
	.on_quit  = &twitch_quit,
	.on_mod_msg = &twitch_mod_msg,
	.commands = DEFINE_CMDS (
		[FOLLOW_NOTIFY]  = CONTROL_CHAR "fnotify",
		[UPTIME]         = CONTROL_CHAR "uptime "  CONTROL_CHAR_2 "uptime",
		[TWITCH_VOD]     = CONTROL_CHAR "vod "     CONTROL_CHAR_2 "vod",
		[TWITCH_TRACKER] = CONTROL_CHAR "tracker " CONTROL_CHAR_2 "tracker " CONTROL_CHAR "streams " CONTROL_CHAR_2 "streams"
	)
};

static const IRCCoreCtx* ctx;

static const size_t uptime_check_interval = 120;
static const size_t follower_check_interval = 60;
static const size_t tracker_update_interval = 60;

static time_t last_uptime_check;
static time_t last_follower_check;
static time_t last_tracker_update;

static CURL* curl;
static struct curl_slist* twitch_cid_header;

typedef struct {
	bool do_follower_notify;
	time_t last_follower_time;

	time_t stream_start;
	time_t last_uptime_check;
	bool live_state_changed;

	time_t last_vod_check;
	char*  last_vod_msg;

	bool  is_tracked;
	char* tracked_name;
	char* stream_title;
} TwitchInfo;

static char**      twitch_keys;
static TwitchInfo* twitch_vals;

static char**      twitch_tracker_chans;
static char**      twitch_tracker_tags;

static bool        first_update = true;

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

static bool twitch_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	time_t now = time(0);
	last_uptime_check = now;
	last_follower_check = now;
	last_tracker_update = now - 50;

	FILE* f = fopen(ctx->get_datafile(), "r");

	char line[1024];
	while(fgets(line, sizeof(line), f)){
		char buffer[256];
		char* tracked_name = NULL;

		if(sscanf(line, "NOTIFY %s", buffer) == 1){
			TwitchInfo* t = twitch_get_or_add(buffer);
			
			t->do_follower_notify = true;
			t->last_follower_time = now;

		} else if(sscanf(line, "TRACK %s %m[^\n]", buffer, &tracked_name) >= 1){
			TwitchInfo* t = twitch_get_or_add(buffer);

			t->is_tracked = true;
			t->tracked_name = tracked_name;

		} else if(sscanf(line, "OUTPUT %s", buffer) == 1){
			sb_push(twitch_tracker_chans, strdup(buffer));
		} else if(sscanf(line, "TAG %s", buffer) == 1){
			sb_push(twitch_tracker_tags, strdup(buffer));
		}
	}
	fclose(f);

	curl = curl_easy_init();

	const char* client_id = getenv("INSOBOT_TWITCH_CLIENT_ID");
	if(client_id){
		char buf[256];
		snprintf(buf, sizeof(buf), "Client-ID: %s", client_id);
		twitch_cid_header = curl_slist_append(NULL, buf);
	}

	return true;
}

static long twitch_curl(char**, long, const char*, ...) __attribute__((format(printf, 3, 4)));

static long twitch_curl(char** data, long last_time, const char* fmt, ...){
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

	if(twitch_cid_header){
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, twitch_cid_header);
	}
	curl_easy_setopt(curl, CURLOPT_TIMECONDITION, CURL_TIMECOND_IFMODSINCE);
	curl_easy_setopt(curl, CURLOPT_TIMEVALUE, last_time);

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

static void twitch_check_uptime(size_t count, size_t* indices){
	if(count == 0) return;

	char chan_buffer[1024] = {};
	for(size_t i = 0; i < count; ++i){
		inso_strcat(chan_buffer, sizeof(chan_buffer), twitch_keys[indices[i]] + 1);
		inso_strcat(chan_buffer, sizeof(chan_buffer), ",");
	}

	char* data = NULL;
	yajl_val root = NULL;
	time_t now = time(0);
	
	long ret = twitch_curl(&data, last_uptime_check, "https://api.twitch.tv/kraken/streams?channel=%s", chan_buffer);
	last_uptime_check = now;

	if(ret == 304){
		return;
	}

	if(ret == -1 || !(root = yajl_tree_parse(data, NULL, 0))){
		fprintf(stderr, "mod_twitch: error getting uptime.\n");
		return;
	}

	const char* streams_path[] = { "streams", NULL };
	yajl_val streams = yajl_tree_get(root, streams_path, yajl_t_array);

	if(streams){
		const char* name_path[]    = { "channel", "name", NULL };
		const char* title_path[]   = { "channel", "status", NULL };
		const char* created_path[] = { "created_at", NULL };

		for(int i = 0; i < streams->u.array.len; ++i){
			yajl_val obj = streams->u.array.values[i];

			yajl_val name  = yajl_tree_get(obj, name_path, yajl_t_string);
			yajl_val start = yajl_tree_get(obj, created_path, yajl_t_string);
			yajl_val title = yajl_tree_get(obj, title_path, yajl_t_string);

			if(name && start){
				TwitchInfo* info = twitch_get_or_add(name->u.string);

				struct tm created_tm = {};
				const char* end = strptime(start->u.string, "%Y-%m-%dT%TZ", &created_tm);

				time_t new_stream_start;
				if(end && !*end){
					new_stream_start = timegm(&created_tm);
				} else {
					new_stream_start = 0;
				}

				info->live_state_changed = new_stream_start != info->stream_start;
				info->stream_start = new_stream_start;

				if(title){
					if(info->stream_title){
						free(info->stream_title);
					}
					info->stream_title = strdup(title->u.string);
				}

				info->last_uptime_check = now;
			}
		}
	}

	for(size_t i = 0; i < count; ++i){
		TwitchInfo* t = twitch_vals + indices[i];
		if(t->last_uptime_check != now){
			t->live_state_changed = t->stream_start != 0;
			t->stream_start = 0;
			t->last_uptime_check = now;
		}
	}

	sb_free(data);
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

static void twitch_print_vod(size_t index, const char* send_chan, const char* name){

	char* chan    = twitch_keys[index];
	TwitchInfo* t = twitch_vals + index;

	char* data = NULL;
	const char url_fmt[] = "https://api.twitch.tv/kraken/channels/%s/videos?broadcasts=true&limit=1";

	long ret = twitch_curl(&data, t->last_vod_check, url_fmt, chan + 1);
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

	const char* videos_path[]      = { "videos", NULL };
	const char* vod_url_path[]     = { "url", NULL };
	const char* vod_title_path[]   = { "title", NULL };

	yajl_val videos = yajl_tree_get(root, videos_path, yajl_t_array);
	if(!videos || !YAJL_IS_ARRAY(videos)){
		fprintf(stderr, "twitch_print_vod: videos null\n");
		goto out;
	}

	if(videos->u.array.len < 1){
		fprintf(stderr, "twitch_print_vod: videos empty");
		goto out;
	}

	yajl_val vods = videos->u.array.values[0];

	yajl_val vod_url   = yajl_tree_get(vods, vod_url_path, yajl_t_string);
	yajl_val vod_title = yajl_tree_get(vods, vod_title_path, yajl_t_string);

	if(!vod_url){
		fprintf(stderr, "twitch_print_vod: url/date null\n");
		goto out;
	}

	if(t->last_vod_msg){
		free(t->last_vod_msg);
	}

	const char* title = vod_title->u.string ?: "untitled";

	asprintf_check(&t->last_vod_msg, "%s's last VoD: %s [%s]", chan + 1, vod_url->u.string, title);
	ctx->send_msg(send_chan, "%s: %s", name, t->last_vod_msg);

out:
	if(data) sb_free(data);
	if(root) yajl_tree_free(root);
}

#define TWITCH_TRACKER_MSG(fmt, ...) \
	for(char** c = twitch_tracker_chans; c < sb_end(twitch_tracker_chans); ++c) ctx->send_msg(*c, fmt, __VA_ARGS__)

static void twitch_tracker_update(void){

	char topic[1024] = "\002\0030,4[LIVE]\017 ";

	bool any_changed = false;
	bool any_live = false;

	size_t* track_indices = NULL;
	size_t index_count = 0;

	for(int i = 0; i < sb_count(twitch_keys); ++i){
		if(twitch_vals[i].is_tracked){
			sb_push(track_indices, i);
			index_count++;
		}
	}

	twitch_check_uptime(index_count, track_indices);
	sb_free(track_indices);

	char tag_buf[1024] = {};
	for(char** c = twitch_tracker_tags; c < sb_end(twitch_tracker_tags); ++c){
		inso_strcat(tag_buf, sizeof(tag_buf), *c);
		inso_strcat(tag_buf, sizeof(tag_buf), " ");
	}

	for(int i = 0; i < sb_count(twitch_keys); ++i){
		if(!twitch_vals[i].is_tracked) continue;

		const char* chan = twitch_keys[i];
		TwitchInfo* t    = twitch_vals + i;

		bool changed = t->live_state_changed;

		if(t->stream_start != 0){
			any_live = true;
			inso_strcat(topic, sizeof(topic), chan + 1);
			inso_strcat(topic, sizeof(topic), " ");

			if(changed){
				if(!any_changed && *tag_buf){
					TWITCH_TRACKER_MSG("FAO %s:", tag_buf);
				}
				any_changed = true;

				const char* display_name = t->tracked_name ? t->tracked_name : chan + 1;
				TWITCH_TRACKER_MSG("\0038%s\017 is now live -\00310 http://twitch.tv/%s \017- \"%s\"", display_name, chan + 1, t->stream_title ? t->stream_title : "");
			}
		} else if(changed){
			any_changed = true;
			if(t->tracked_name){
				TWITCH_TRACKER_MSG("\00314%s (%s) is no longer live.", t->tracked_name, chan + 1);
			} else {
				TWITCH_TRACKER_MSG("\00314%s is no longer live.", chan + 1);
			}
		}
	}

	if(!any_changed && !first_update) return;

	char topic_cmd[1024];
	for(char** c = twitch_tracker_chans; c < sb_end(twitch_tracker_chans); ++c){
		if(any_live){
			snprintf(topic_cmd, sizeof(topic_cmd), "TOPIC %s :\00311[!streams]\017 %s", *c, topic);
		} else {
			snprintf(topic_cmd, sizeof(topic_cmd), "TOPIC %s :\00311[!streams]\017 No streams currently live.", *c);
		}
		ctx->send_raw(topic_cmd);
	}

	first_update = false;
}

static void twitch_tracker_cmd(const char* chan, const char* name, const char* arg, bool wlist){

	int enabled_index = -1;
	for(char** c = twitch_tracker_chans; c < sb_end(twitch_tracker_chans); ++c){
		if(strcmp(*c, chan) == 0){
			enabled_index = c - twitch_tracker_chans;
			break;
		}
	}

	if(strcmp(name, BOT_OWNER) == 0){
		if(strcasecmp(arg, " enable") == 0 && enabled_index == -1){
			sb_push(twitch_tracker_chans, strdup(chan));
			ctx->send_msg(chan, "Enabled twitch tracker.");
			twitch_tracker_update();
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

	if(wlist && sscanf(arg, " add %s %m[^\n]", buffer, &optional_name) >= 1){
		
		TwitchInfo* t = twitch_get_or_add(buffer);
		t->is_tracked = true;
		if(t->tracked_name){
			free(t->tracked_name);
		}
		t->tracked_name = optional_name;

		ctx->send_msg(chan, "Now tracking channel %s", buffer);
		ctx->save_me();

	} else if(wlist && sscanf(arg, " del %s", buffer) == 1){
		
		TwitchInfo* t = twitch_get_or_add(buffer);
		t->is_tracked = false;
		
		ctx->send_msg(chan, "Untracked channel %s", buffer);
		ctx->save_me();

	} else {
	
		int tag_index = -1;
		for(char** tag = twitch_tracker_tags; tag < sb_end(twitch_tracker_tags); ++tag){
			if(strcasecmp(*tag, name) == 0){
				tag_index = tag - twitch_tracker_chans;
				break;
			}
		}

		if(strcasecmp(arg, " tagme") == 0){

			if(tag_index == -1){
				sb_push(twitch_tracker_tags, strdup(name));
				ctx->send_msg(chan, "%s: You'll now be tagged when streams go live!", name);
				ctx->save_me();
			} else {
				ctx->send_msg(chan, "%s: You're already tagged, my friend.", name);
			}

		} else if(strcasecmp(arg, " untagme") == 0){

			if(tag_index != -1){
				sb_erase(twitch_tracker_tags, tag_index);
				ctx->send_msg(chan, "%s: OK, I've untagged you.", name);
				ctx->save_me();
			} else {
				ctx->send_msg(chan, "%s: You're already not tagged.", name);
			}
		} else if(strcasecmp(arg, " list") == 0){

			int max_display_len = 0;
			int max_chan_len = 0;

			for(TwitchInfo* t = twitch_vals; t < sb_end(twitch_vals); ++t){
				if(!t->is_tracked || !t->stream_start) continue;
				
				size_t chan_len = strlen(twitch_keys[t - twitch_vals]) - 1;
				max_chan_len = INSO_MAX(max_chan_len, chan_len);

				if(t->tracked_name){
					max_display_len = INSO_MAX(max_display_len, strlen(t->tracked_name));
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
				inso_strcat(chan_buf, sizeof(chan_buf), " ");
			}
			ctx->send_msg(chan, "Tracked channels: %s", chan_buf);

		} else {
			ctx->send_msg(chan, "%s: Usage: !streams [list|tagme|untagme|chans|add <chan> [name]|del <chan>]", name);
		}
	}
}

static void twitch_cmd(const char* chan, const char* name, const char* arg, int cmd){
	bool is_admin = strcasecmp(chan + 1, name) == 0 || inso_is_admin(ctx, name);
	bool is_wlist = is_admin || inso_is_wlist(ctx, name);

	switch(cmd){
		case FOLLOW_NOTIFY: {
			if(!is_admin) return;
			if(!*arg++) break;

			TwitchInfo* t = twitch_get_or_add(chan);

			if(strcasecmp(arg, "off") == 0){
				if(t->do_follower_notify){
					ctx->send_msg(chan, "%s: Disabled follow notifier.", name);
					t->do_follower_notify = false;
				} else {
					ctx->send_msg(chan, "%s: It's already disabled.", name);
				}
			} else if(strcasecmp(arg, "on") == 0){
				if(t->do_follower_notify){
					ctx->send_msg(chan, "%s: It's already enabled.", name);
				} else {
					ctx->send_msg(chan, "%s: Enabled follow notifier.", name);
					t->do_follower_notify = true;
				}
			}
		} break;

		case UPTIME: {
			time_t now = time(0);

			TwitchInfo* t = twitch_get_or_add(chan);

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

				ctx->send_msg(chan, "%s: The stream has been live for %s", name, time_buf);
			} else {
				ctx->send_msg(chan, "%s: The stream is not live.", name);
			}
		} break;

		case TWITCH_VOD: {
			TwitchInfo* t;

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

			twitch_print_vod(t - twitch_vals, chan, name);
		} break;

		case TWITCH_TRACKER: {
			twitch_tracker_cmd(chan, name, arg, is_wlist);
		} break;
	}
}

static const char twitch_api_template[] = "https://api.twitch.tv/kraken/channels/%s/follows?limit=10";
static const char* follows_path[] = { "follows", NULL };
static const char* date_path[] = { "created_at", NULL };
static const char* name_path[] = { "user", "display_name", NULL };

static void twitch_check_followers(void){

	char* data = NULL;
	yajl_val root = NULL;

	for(size_t i = 0; i < sb_count(twitch_keys); ++i){

		char* chan    = twitch_keys[i];
		TwitchInfo* t = twitch_vals + i;

		if(!t->do_follower_notify || !twitch_check_live(i)){
			continue;
		}

		long ret = twitch_curl(&data, last_follower_check, twitch_api_template, chan + 1);
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

		yajl_val follows = yajl_tree_get(root, follows_path, yajl_t_array);
		if(!follows){
			fprintf(stderr, "mod_twitch: follows not array!\n");
			goto out;
		}

		char msg_buf[256] = {};
		size_t new_follow_count = 0;
		time_t new_time = t->last_follower_time;

		for(size_t j = 0; j < follows->u.array.len; ++j){
			yajl_val user = follows->u.array.values[j];

			yajl_val date = yajl_tree_get(user, date_path, yajl_t_string);
			if(!date){
				fprintf(stderr, "mod_twitch date object null!\n");
				goto out;
			}

			yajl_val name = yajl_tree_get(user, name_path, yajl_t_string);
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
	if(data) sb_free(data);
	if(root) yajl_tree_free(root);
}

static void twitch_tick(void){
	time_t now = time(0);

	if(now - last_tracker_update > tracker_update_interval){
		twitch_tracker_update();
		last_tracker_update = now;
	}

	if(sb_count(twitch_keys) && (now - last_follower_check > follower_check_interval)){
		puts("Checking twitch followers...");
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

	for(char** tag = twitch_tracker_tags; tag < sb_end(twitch_tracker_tags); ++tag){
		fprintf(f, "TAG\t%s\n", *tag);
	}

	return true;
}

static void twitch_quit(void){
	for(size_t i = 0; i < sb_count(twitch_keys); ++i){
		free(twitch_keys[i]);
		if(twitch_vals[i].last_vod_msg){
			free(twitch_vals[i].last_vod_msg);
		}
	}
	sb_free(twitch_keys);
	sb_free(twitch_vals);

	if(twitch_cid_header){
		curl_slist_free_all(twitch_cid_header);
	}

	curl_easy_cleanup(curl);
}

static TwitchUser* twitch_get_user(const char* name){

	for(int i = 0; i < sb_count(twitch_users); ++i){
		if(strcasecmp(name, twitch_users[i].name) == 0){
			return twitch_users + i;
		}
	}

	char* data = NULL;
	if(twitch_curl(&data, 0, "https://api.twitch.tv/kraken/users/%s", name) != 200) return NULL;

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	if(!root) return NULL;

	const char* created_path[] = { "created_at", NULL };
	yajl_val created = yajl_tree_get(root, created_path, yajl_t_string);
	if(!created) return NULL;

	struct tm user_time = {};
	char* end = strptime(created->u.string, "%Y-%m-%dT%TZ", &user_time);
	if(!end || *end) return NULL;

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
	}
}
