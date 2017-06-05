#include "module.h"
#include "inso_utils.h"
#include "inso_json.h"
#include "stb_sb.h"
#include <time.h>
#include <string.h>
#include <regex.h>
#include <curl/curl.h>
#include <assert.h>
#include <inttypes.h>
#include "module_msgs.h"

static bool clip_init (const IRCCoreCtx*);
static void clip_cmd  (const char*, const char*, const char*, int);
static void clip_quit (void);

enum { CLIP_CREATE };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "twitch-clip",
	.desc     = "Auto create twitch clips",
	.on_init  = &clip_init,
	.on_cmd   = &clip_cmd,
	.on_quit  = &clip_quit,
	.commands = DEFINE_CMDS (
		[CLIP_CREATE]  = CMD("clip")
	),
};

#define USER_AGENT "Mozilla/5.0 (Windows NT 10.0; WOW64; Trident/7.0; rv:11.0) like Gecko"

static const IRCCoreCtx* ctx;
static CURL* curl;
static regex_t login_regexen[4];

static char**  clip_chans;
static time_t* clip_times;

static size_t clip_login_loc_cb(char* buf, size_t sz, size_t n, void* arg){
	char* location = NULL;
	if(buf && sscanf(buf, "Location: %m[^\r\n]", &location) == 1){
		*(char**)arg = location;
	}
	return sz*n;
}

static bool clip_login(const char* user, const char* pass){
	char* location = NULL;
	char* data = NULL;
	bool result = false;

	curl_easy_setopt(curl, CURLOPT_URL, "https://www.twitch.tv/login");
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &clip_login_loc_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &location);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);

	char* login_params[4] = {};
	regmatch_t m[2];

	long ret = inso_curl_perform(curl, &data);
	if(ret == 302 && location){
		for(int i = 0; i < 3; ++i){
			if(regexec(login_regexen + i, location, 2, m, 0) == 0){
				login_params[i] = strndupa(location + m[1].rm_so, m[1].rm_eo - m[1].rm_so);
			}
		}

		curl_easy_setopt(curl, CURLOPT_URL, location);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, NULL);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);
		sb_free(data);

		ret = inso_curl_perform(curl, &data);
		if(ret == 200 && regexec(login_regexen + 3, data, 2, m, 0) == 0){
			login_params[3] = strndupa(data + m[1].rm_so, m[1].rm_eo - m[1].rm_so);
		}
	}

	if(login_params[0] && login_params[1] && login_params[2] && login_params[3]){

		char* post;
		asprintf_check(&post,
		               "username=%s&"
		               "password=%s&"
		               "client_id=%s&"
		               "nonce=%s&"
					   "scope=openid&"
					   "state=%s&"
					   "redirect_url=https%%3A%%2F%%2Fwww.twitch.tv%%2F&"
					   "response_type=code&"
					   "request_id=%s&"
					   "embed=false&"
					   "time_to_submit=%d.%03d&"
					   "captcha=",
					   user, pass,
					   login_params[0], login_params[1], login_params[2], login_params[3],
					   rand()%3+2, rand()%1000);

		printf("POST = [%s]\n", post);

		usleep(3*1000*1000);

		curl_easy_setopt(curl, CURLOPT_URL, "https://passport.twitch.tv/authentications/new");
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
		sb_free(data);

		ret = inso_curl_perform(curl, &data);

		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, NULL);
		curl_easy_setopt(curl, CURLOPT_POST, 0L);
		free(post);

		if(ret == 200){
			yajl_val root  = yajl_tree_parse(data, NULL, 0);
			yajl_val redir = YAJL_GET(root, yajl_t_string, ("redirect"));

			if(redir){
				curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
				curl_easy_setopt(curl, CURLOPT_URL, redir->u.string);
				sb_free(data);

				ret = inso_curl_perform(curl, &data);
				if(ret >= 200 && ret < 400){
					result = true;
				}
			}

			yajl_tree_free(root);
		}
	}

	free(location);
	sb_free(data);

	return result;
}

static bool clip_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;

	assert(!regcomp(login_regexen + 0, "client_id=([^&]+)", REG_EXTENDED | REG_ICASE));
	assert(!regcomp(login_regexen + 1, "nonce=([^&]+)", REG_EXTENDED | REG_ICASE));
	assert(!regcomp(login_regexen + 2, "state=([^&]+)", REG_EXTENDED | REG_ICASE));
	assert(!regcomp(login_regexen + 3, "request_id\" value=\"([^\"]+)\"", REG_EXTENDED | REG_ICASE));

	curl = inso_curl_init(NULL, NULL);

	const char* filename = ctx->get_datafile();
	curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(curl, CURLOPT_COOKIEFILE, filename);
	curl_easy_setopt(curl, CURLOPT_COOKIEJAR, filename);
	curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST, "TLSv1+AES+EECDH");

	const char* user = getenv("INSOBOT_TWITCH_CLIP_USER");
	const char* pass = getenv("INSOBOT_TWITCH_CLIP_PASS"); //XXX: URL encoding?

	if(!user || !pass) return false;

	if(clip_login(user, pass)){
		puts("twitch_clip: Everything worked in theory.");
	} else {
		puts("twitch_clip: RIP");
		clip_quit();
		return false;
	}

	inso_curl_reset(curl, NULL, NULL);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(curl, CURLOPT_COOKIEFILE, filename);
	curl_easy_setopt(curl, CURLOPT_COOKIEJAR, filename);
	curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST, "TLSv1+AES+EECDH");

	return true;
}

static time_t* clip_chan_time(const char* chan){
	sb_each(c, clip_chans){
		if(strcmp(*c, chan) == 0){
			return clip_times + (c - clip_chans);
		}
	}

	sb_push(clip_chans, strdup(chan));
	sb_push(clip_times, 0);

	return &sb_last(clip_times);;
}

static intptr_t clip_stream_info_cb(intptr_t result, intptr_t arg){
	memcpy((TwitchInfoMsg*)arg, (TwitchInfoMsg*)result, sizeof(TwitchInfoMsg));
	return 0;
}

static void clip_gen_id(char buf[static 32]){
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	for(size_t i = 0; i < 32; ++i){
		buf[i] = alphabet[rand()%ARRAY_SIZE(alphabet)];
	}
}

static void clip_cmd(const char* chan, const char* name, const char* msg, int cmd){
	if(!inso_is_wlist(ctx, name)) return;

	time_t  now  = time(0);
	time_t* last = clip_chan_time(chan);

	if(now - *last < 20){
		// TODO: store last clip URL and repeat it?
		return;
	}

	TwitchInfoMsg info;
	MOD_MSG(ctx, "twitch_get_stream_info", chan, &clip_stream_info_cb, &info);

	if(!info.id || !info.start) return;
	int offset = INSO_MAX(0, now - info.start);

	char sess_id[32];
	clip_gen_id(sess_id);

	char* post;
	asprintf_check(&post,
	               "player_backend_type=player-core&"
	               "channel=%s&"
				   "offset=%d&"
				   "broadcast_id=%" PRIu64 "&"
				   "vod_id=&"
				   "play_session_id=%.32s&"
				   "show_view_page=false",
				   chan+1, offset, info.id, sess_id);

	char* data = NULL;
	char* location = NULL;

	curl_easy_setopt(curl, CURLOPT_URL, "https://clips.twitch.tv/clips");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &clip_login_loc_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &location);

	long ret = inso_curl_perform(curl, &data);
	if(ret >= 200 && ret < 400 && location){
		char* p = strrchr(location, '/');
		if(p && strncmp(p, "/edit", 5) == 0){
			*p = 0;
		}
		ctx->send_msg(chan, "@%s: %s", name, location);
		*last = time(0);
	} else {
		printf("clips: RIP %ld\n", ret);
	}

	free(location);
	sb_free(data);
}

static void clip_quit(void){
	sb_each(c, clip_chans){
		free(*c);
	}
	sb_free(clip_chans);
	sb_free(clip_times);

	for(size_t i = 0; i < ARRAY_SIZE(login_regexen); ++i){
		regfree(login_regexen + i);
	}

	curl_easy_cleanup(curl);
}
