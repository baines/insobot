#include "module.h"
#include <curl/curl.h>
#include "stb_sb.h"
#include <time.h>
#include <string.h>
#include <yajl/yajl_tree.h>
#include "utils.h"

static bool twitch_init (const IRCCoreCtx*);
static void twitch_cmd  (const char*, const char*, const char*, int);
static void twitch_tick (void);
static bool twitch_save (FILE*);
static void twitch_quit (void);

enum { FOLLOW_NOTIFY, UPTIME, TWITCH_VOD };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "twitch",
	.desc     = "Functionality specific to twitch.tv",
	.on_init  = twitch_init,
	.on_cmd   = &twitch_cmd,
	.on_tick  = &twitch_tick,
	.on_save  = &twitch_save,
	.on_quit  = &twitch_quit,
	.commands = DEFINE_CMDS (
		[FOLLOW_NOTIFY] = CONTROL_CHAR "fnotify",
		[UPTIME]        = CONTROL_CHAR "uptime " CONTROL_CHAR_2 "uptime",
		[TWITCH_VOD]    = CONTROL_CHAR "vod "    CONTROL_CHAR_2 "vod"
	)
};

static const IRCCoreCtx* ctx;

static const size_t uptime_check_interval = 120;
static const size_t follower_check_interval = 60;

static time_t last_follower_check;

static CURL* curl;

typedef struct {
	bool do_follower_notify;
	time_t last_follower_time;

	time_t stream_start;
	time_t last_uptime_check;

	time_t last_vod_check;
	char*  last_vod_msg;
} TwitchInfo;

char**      twitch_keys;
TwitchInfo* twitch_vals;

static TwitchInfo* twitch_get_or_add(const char* chan){
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
	last_follower_check = now;

	FILE* f = fopen(ctx->get_datafile(), "r");
	char chan[256];
	while(fscanf(f, "%255s", chan) == 1){
		TwitchInfo* t = twitch_get_or_add(chan);
		t->do_follower_notify = true;
		t->last_follower_time = now;
	}
	fclose(f);

	curl = curl_easy_init();

	return true;
}

static long twitch_curl(char**, long, const char*, ...) __attribute__((format(printf, 3, 4)));

static long twitch_curl(char** data, long last_time, const char* fmt, ...){
	va_list v;
	va_start(v, fmt);

	char* url;
	vasprintf(&url, fmt, v);

	va_end(v);

	*data = NULL;
	inso_curl_reset(curl, url, data);
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
	}

	return http_code;
}

static void twitch_check_uptime(size_t index){

	char* chan       = twitch_keys[index];
	TwitchInfo* info = twitch_vals + index;

	char* data = NULL;
	yajl_val root = NULL;

	long ret = (twitch_curl(&data, info->last_uptime_check, "https://api.twitch.tv/kraken/streams/%s", chan + 1) == 304);
	if(ret == 304){
		return;
	}

	sb_push(data, 0);

	const char* created_path[] = { "stream", "created_at", NULL };

	if(ret != -1 && (root = yajl_tree_parse(data, NULL, 0))){

		yajl_val start = yajl_tree_get(root, created_path, yajl_t_string);

		if(start){
			struct tm created_tm = {};
			const char* end = strptime(start->u.string, "%Y-%m-%dT%TZ", &created_tm);
			if(end && !*end){
				info->stream_start = timegm(&created_tm);
			} else {
				info->stream_start = 0;
			}
		} else {
			info->stream_start = 0;
		}

		yajl_tree_free(root);

	} else {
		fprintf(stderr, "mod_twitch: error getting uptime.\n");
	}

	sb_free(data);
}

static bool twitch_check_live(size_t index){
	time_t now = time(0);

	TwitchInfo* t = twitch_vals + index;

	if(now - t->last_uptime_check > uptime_check_interval){
		twitch_check_uptime(index);
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
	if(ret == 304){
		if(t->last_vod_msg){
			ctx->send_msg(send_chan, "%s: %s", name, t->last_vod_msg);
		}
		return;
	} else if(ret == -1){
		goto out;
	}

	sb_push(data, 0);

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	if(!root){
		fprintf(stderr, "twitch_print_vod: root null\n");
		goto out;
	}

	const char* videos_path[]      = { "videos", NULL };
	const char* vod_url_path[]     = { "url", NULL };
	const char* vod_title_path[]   = { "title", NULL };
//	const char* vod_created_path[] = { "created_at", NULL };

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
//	yajl_val vod_date  = yajl_tree_get(vods, vod_created_path, yajl_t_string);

	if(!vod_url){
		fprintf(stderr, "twitch_print_vod: url/date null\n");
		goto out;
	}

	if(t->last_vod_msg){
		free(t->last_vod_msg);
	}

	const char* title = vod_title->u.string ?: "untitled";

	asprintf(&t->last_vod_msg, "%s's last VoD: %s [%s]", chan + 1, vod_url->u.string, title);
	ctx->send_msg(send_chan, "%s: %s", name, t->last_vod_msg);

out:
	if(data) sb_free(data);
	if(root) yajl_tree_free(root);
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

		sb_push(data, 0);
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

	curl_easy_cleanup(curl);
}

static void twitch_tick(void){
	time_t now = time(0);

	if(sb_count(twitch_keys) && (now - last_follower_check > follower_check_interval)){
		puts("Checking twitch followers...");
		twitch_check_followers();
		last_follower_check = now;
	}
}

static bool twitch_save(FILE* f){
	for(TwitchInfo* t = twitch_vals; t < sb_end(twitch_vals); ++t){
		if(t->do_follower_notify){
			fprintf(f, "%s\n", twitch_keys[t - twitch_vals]);
		}
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

	curl_easy_cleanup(curl);
}
