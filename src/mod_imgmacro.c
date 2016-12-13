#include "module.h"
#include "stb_sb.h"
#include "inso_utils.h"
#include <yajl/yajl_tree.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <cairo/cairo.h>
#include <ctype.h>
#include <glob.h>

static bool im_init (const IRCCoreCtx*);
static void im_cmd  (const char*, const char*, const char*, int);
static void im_pm   (const char*, const char*);
static bool im_save (FILE*);
static void im_quit (void);
static void im_ipc  (int, const uint8_t*, size_t);

enum { IM_CREATE, IM_SHOW, IM_LIST, IM_AUTO };

const IRCModuleCtx irc_mod_ctx = {
	.name        = "imgmacro",
	.desc        = "Creates image macros / \"memes\"",
	.on_init     = &im_init,
	.on_cmd      = &im_cmd,
	.on_pm       = &im_pm,
	.on_save     = &im_save,
	.on_quit     = &im_quit,
	.on_ipc      = &im_ipc,
	.commands    = DEFINE_CMDS (
		[IM_CREATE] = CMD("newimg")  CMD("mkmeme"),
		[IM_SHOW]   = CMD("img")     CMD("meme"),
		[IM_LIST]   = CMD("lsimg")   CMD("memelist"),
		[IM_AUTO]   = CMD("autoimg") CMD("automeme")
	)
};

static const IRCCoreCtx* ctx;

typedef struct IMEntry_ {
	int id;
	char* url;
	char* text;
	char* del;
} IMEntry;

static IMEntry* im_entries;

static const char* imgur_client_id;
static const char* imgur_album_id;
static const char* imgur_album_hash;
static struct curl_slist* imgur_curl_headers;

static char* im_base_dir;

static char* im_get_template(const char* name){
	char dir_buf[PATH_MAX];
	dir_buf[PATH_MAX - 1] = 0;

	strncpy(dir_buf, im_base_dir, sizeof(dir_buf) - 1);

	if(name){
		if(strchr(name, '.')) return NULL;
		if(inso_strcat(dir_buf, sizeof(dir_buf), name) < 0) return NULL;
		if(inso_strcat(dir_buf, sizeof(dir_buf), ".png") < 0) return NULL;

		printf("imgmacro template: [%s]\n", dir_buf);

		struct stat st;
		if(stat(dir_buf, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
	} else {
		inso_strcat(dir_buf, sizeof(dir_buf), "*.png");

		glob_t glob_data;
		if(glob(dir_buf, 0, NULL, &glob_data) != 0 || glob_data.gl_pathc == 0){
			return NULL;
		}
		char* path = glob_data.gl_pathv[rand() % glob_data.gl_pathc];
		strcpy(dir_buf, path);

		globfree(&glob_data);
	}

	return strdup(dir_buf);
}

static char* im_lookup(int id){
	for(IMEntry* i = im_entries; i < sb_end(im_entries); ++i){
		if(i->id == id) return i->url;
	}
	return NULL;
}

static bool im_upload(const uint8_t* png, unsigned int png_len, IMEntry* e){

#if 0
	FILE* f = fopen("debug-image.png", "wb");
	fwrite(png, png_len, 1, f);
	fflush(f);
	fclose(f);
	return false;
#endif

	char title[32];
	snprintf(title, sizeof(title), "%d", e->id);

	struct curl_httppost *form = NULL, *last = NULL;

	curl_formadd(&form, &last,
				 CURLFORM_PTRNAME, "image",
				 CURLFORM_BUFFER, "image.png",
				 CURLFORM_BUFFERPTR, png,
				 CURLFORM_BUFFERLENGTH, png_len,
				 CURLFORM_END);

	curl_formadd(&form, &last,
				 CURLFORM_PTRNAME, "title",
				 CURLFORM_PTRCONTENTS, title,
				 CURLFORM_END);

	curl_formadd(&form, &last,
				 CURLFORM_PTRNAME, "description",
				 CURLFORM_PTRCONTENTS, e->text,
				 CURLFORM_END);

#if 0
	curl_formadd(&form, &last,
				 CURLFORM_PTRNAME, "album",
				 CURLFORM_PTRCONTENTS, imgur_album_hash,
				 CURLFORM_END);
#endif

	char* data = NULL;
	CURL* curl = inso_curl_init("https://api.imgur.com/3/image", &data);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, imgur_curl_headers);
	curl_easy_setopt(curl, CURLOPT_HTTPPOST, form);
	CURLcode c = curl_easy_perform(curl);
	if(c != CURLE_OK) printf("mod_imgmacro: curl error: %s\n", curl_easy_strerror(c));
	sb_push(data, 0);

	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_HTTP_CODE, &http_code);

	curl_easy_cleanup(curl);
	curl_formfree(form);

	static const char* id_path[]  = { "data", "id", NULL };
	static const char* del_path[] = { "data", "deletehash", NULL };

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	yajl_val id   = yajl_tree_get(root, id_path, yajl_t_string);
	yajl_val del  = yajl_tree_get(root, del_path, yajl_t_string);

	if(root && id && del){
		printf("DELETE HASH: [%s] = [%s]\n", id->u.string, del->u.string);
		asprintf_check(&e->url, "https://i.imgur.com/%s.png", id->u.string);
		e->del = strdup(del->u.string);
		ctx->save_me();
	} else {
		printf("mod_imgmacro: root/id/del null\n");
		http_code = 0;
	}

	yajl_tree_free(root);
	sb_free(data);

	return http_code == 200;
}

static cairo_status_t im_png_write(void* arg, const uint8_t* data, unsigned int data_len){
	char** out = arg;
	memcpy(sb_add(*out, data_len), data, data_len);
	return CAIRO_STATUS_SUCCESS;
}

enum { IM_TEXT_TOP, IM_TEXT_BOTTOM };

static void im_draw_text(cairo_t* cairo, double w, double h, const char* text, int where){
	if(!text) return;

	cairo_save(cairo);

	cairo_text_extents_t te;
	cairo_text_extents(cairo, text, &te);

	cairo_translate(cairo, w / 2.0, h / 2.0);

	double scale = te.width > w ? w / te.width : 1.0;
	scale *= 0.95;

	if(scale < 0.1) scale = 0.1;

	cairo_scale(cairo, scale, scale);

	double offset = h / (2.1 * scale);

	if(where == IM_TEXT_TOP){
		offset = -offset - te.y_bearing / 2.0;
	} else {
		offset = offset + te.y_bearing / 2.0;
	}

	cairo_move_to(
		cairo,
		-(te.width / 2 + te.x_bearing),
		-(te.height / 2 + te.y_bearing) + offset
	);
	
	cairo_text_path(cairo, text);
	cairo_set_source_rgb(cairo, 1, 1, 1);
	cairo_fill_preserve(cairo);
	cairo_set_source_rgb(cairo, 0, 0, 0);
	cairo_stroke(cairo);

	cairo_restore(cairo);
}

static IMEntry* im_create(const char* template, const char* top, const char* bot){
	
	// TODO: check if one already exists first, cmp top / bot with IMEntry.text

	cairo_surface_t* img = cairo_image_surface_create_from_png(template);
	cairo_t* cairo = cairo_create(img);

	double img_w = cairo_image_surface_get_width(img);
	double img_h = cairo_image_surface_get_height(img);

	cairo_select_font_face(cairo, "Impact", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

	double font_sz = img_w < img_h ? img_w / 8.0 : img_h / 8.0;
	cairo_set_font_size(cairo, font_sz);
	
	cairo_set_line_width(cairo, font_sz / 24.0);
	cairo_set_line_cap  (cairo, CAIRO_LINE_CAP_SQUARE);
	cairo_set_line_join (cairo, CAIRO_LINE_JOIN_BEVEL);

	im_draw_text(cairo, img_w, img_h, top, IM_TEXT_TOP);
	im_draw_text(cairo, img_w, img_h, bot, IM_TEXT_BOTTOM);

	cairo_surface_flush(img);

	int next_id = sb_count(im_entries) ? sb_last(im_entries).id + 1 : 0;
	size_t text_len = strlen(top) + 4;
	if(bot) text_len += strlen(bot);

	char* full_text = malloc(text_len);
	strcpy(full_text, top);
	strcat(full_text, " / ");
	if(bot) strcat(full_text, bot);

	for(char* c = full_text; *c; ++c) *c = toupper(*c);

	IMEntry e = {
		.id = next_id,
		.text = full_text,
	};

	char* png_data = NULL;
	cairo_surface_write_to_png_stream(img, &im_png_write, &png_data);

	bool ok = im_upload(png_data, sb_count(png_data), &e);

	cairo_destroy(cairo);
	cairo_surface_destroy(img);

	if(ok){
		ctx->send_ipc(0, "update", 7);
		sb_push(im_entries, e);
	} else {
		free(full_text);
	}

	sb_free(png_data);

	return ok ? &sb_last(im_entries) : NULL;
}

// TODO
#if 0
static void im_update(void){

	for(IMEntry* i = im_entries; i < sb_end(im_entries); ++i){
		free(i->text);
		free(i->url);
	}
	sb_free(im_entries);

	char url[256];
	snprintf(url, sizeof(url), "https://api.imgur.com/3/album/%s/images", imgur_album_id);

	char* data = NULL;
	CURL* curl = inso_curl_init(url, &data);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, imgur_curl_headers);
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	sb_push(data, 0);

	static const char* data_path[]  = { "data", NULL };
	static const char* id_path[]    = { "id", NULL };
	static const char* title_path[] = { "title", NULL };
	static const char* desc_path[]  = { "description", NULL };

	yajl_val root = yajl_tree_parse(data, NULL, 0);
	yajl_val imgs = yajl_tree_get(root, data_path, yajl_t_array);

	if(root && imgs){
		for(int i = 0; i < imgs->u.array.len; ++i){
			yajl_val key = imgs->u.array.values[i];

			yajl_val img_id    = yajl_tree_get(key, id_path   , yajl_t_string);
			yajl_val img_title = yajl_tree_get(key, title_path, yajl_t_string);
			yajl_val img_desc  = yajl_tree_get(key, desc_path , yajl_t_string);

			if(!img_id || !img_title || !img_desc) continue;

			IMEntry e = {
				.id = atoi(img_title->u.string),
				.text = strdup(img_desc->u.string)
			};
			asprintf_check(&e.url, "https://i.imgur.com/%s.png", img_id->u.string);

			sb_push(im_entries, e);
		}
	}

	yajl_tree_free(root);
	sb_free(data);
}
#endif

static bool im_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	
	imgur_client_id  = getenv("INSOBOT_IMGUR_CLIENT_ID");
	imgur_album_id   = getenv("INSOBOT_IMGMACRO_ALBUM_ID");
	imgur_album_hash = getenv("INSOBOT_IMGMACRO_ALBUM_HASH");

	if(!imgur_client_id || !imgur_album_id || !imgur_album_hash){
		return false;
	}

	char header_buf[256];
	snprintf(header_buf, sizeof(header_buf), "Authorization: Client-ID %s", imgur_client_id);
	imgur_curl_headers = curl_slist_append(NULL, header_buf);

	const char* data_dir = getenv("XDG_DATA_HOME");

	struct stat st;
	if(!data_dir|| stat(data_dir, &st) != 0 || !S_ISDIR(st.st_mode)){
		data_dir = getenv("HOME");
		assert(data_dir);

		asprintf_check(&im_base_dir, "%s/.local/share/insobot/imgmacro/", data_dir);
	} else {
		asprintf_check(&im_base_dir, "%s/insobot/imgmacro/", data_dir);
	}

	IMEntry e;
	FILE* f = fopen(ctx->get_datafile(), "r");
	while(fscanf(f, "%d %ms %ms %m[^\n]", &e.id, &e.url, &e.del, &e.text) == 4){
		sb_push(im_entries, e);
	}
	fclose(f);

//	inso_mkdir_p(im_base_dir);

	return true;
}

static void imgmacro_markov_cb(intptr_t result, intptr_t arg){
	if(result && !*(char*)arg){
		*(char**)arg = (char*)result;
	} else if(result){
		free((char*)result);
	}
}

static void im_cmd(const char* chan, const char* name, const char* arg, int cmd){
	if(!inso_is_wlist(ctx, name)) return;

	switch(cmd){
		case IM_CREATE: {
			char template[64], txt_top[128], txt_bot[128];
			
			int i = sscanf(arg, " %63s \"%127[^\"]\" \"%127[^\"]\"", template, txt_top, txt_bot);

			if(i < 2){
				ctx->send_msg(chan, "%s: Usage: mkmeme <img> <\"top text\"> [\"bottom text\"]", name);
				break;
			}

			for(char* c = template; *c; ++c) *c = tolower(*c);
			for(char* c = txt_top; *c; ++c) *c = toupper(*c);
			for(char* c = txt_bot; *c; ++c) *c = toupper(*c);

			char* img_name = im_get_template(template);
			if(!img_name){
				ctx->send_msg(chan, "%s: Unknown template image", name);
				break;
			}

			char* maybe_bot = i == 3 ? txt_bot : NULL;

			IMEntry* e = im_create(img_name, txt_top, maybe_bot);
			if(e){
				ctx->send_msg(chan, "%s Meme %d: %s", name, e->id, e->url);
			} else {
				ctx->send_msg(chan, "Error creating image");
			}

			free(img_name);
		} break;

		case IM_SHOW: {
			int id;
			char* link = NULL;

			if(sscanf(arg, " %d", &id) != 1){
				int total = sb_count(im_entries);
				if(total == 0){
					ctx->send_msg(chan, "%s: None here :(", name);
				} else {
					link = im_entries[rand() % total].url;
				}
			} else {
				link = im_lookup(id);
			}

			if(link){
				ctx->send_msg(chan, "%s: %s", name, link);
			} else {
				ctx->send_msg(chan, "%s: Unknown id.", name);
			}
		} break;

		case IM_LIST: {

		} break;

		case IM_AUTO: {
			char* markov_text = NULL;
			MOD_MSG(ctx, "markov_gen", 0, &imgmacro_markov_cb, &markov_text);
			if(!markov_text) break;

			size_t word_count = 1;
			for(char* c = markov_text; *c; ++c){
				if(*c == ' ') word_count++;
				*c = toupper(*c);
			}
			word_count = INSO_MIN(word_count, 12);

			size_t half_count = word_count / 2;
			char *txt_top = markov_text, *txt_bot = NULL;

			for(char* c = markov_text; *c; ++c){
				if(*c == ' ' && --half_count <= 0){
					*c = '\0';
					txt_bot = c+1;
					break;
				}
			}

			// give the bottom text a bit more than half to maybe finish sentences.
			half_count = (word_count*3) / 2;
			if(txt_bot){
				for(char* c = txt_bot; *c; ++c){
					if(*c == ' ' && --half_count <= 0){
						*c = '\0';
						break;
					}
				}
			}

			char* img_name = im_get_template(NULL);
			IMEntry* e = im_create(img_name, txt_top, txt_bot);
			if(e){
				ctx->send_msg(chan, "%s Meme %d: %s", name, e->id, e->url);
			} else {
				ctx->send_msg(chan, "Error creating image");
			}

			free(img_name);
			free(markov_text);
		} break;
	}
}

static void im_pm(const char* name, const char* msg){
	if(*msg == '\\' || *msg == '!') ++msg;

	// FIXME: avoid repetition of the stuff in the DEFINE_CMDS here somehow
	if(strncasecmp(msg, "newimg", 6) == 0 || strncasecmp(msg, "mkmeme", 6) == 0){
		im_cmd(name, name, msg + 6, IM_CREATE);
	}
}

static bool im_save(FILE* f){
	for(IMEntry* i = im_entries; i < sb_end(im_entries); ++i){
		fprintf(f, "%d %s %s %s\n", i->id, i->url, i->del, i->text);
	}
	return true;
}

static void im_quit(void){
	for(IMEntry* i = im_entries; i < sb_end(im_entries); ++i){
		free(i->url);
		free(i->text);
		free(i->del);
	}
	sb_free(im_entries);

	free(im_base_dir);
	curl_slist_free_all(imgur_curl_headers);
	cairo_debug_reset_static_data();
}

static void im_ipc(int sender_id, const uint8_t* data, size_t data_len){
//	im_update(); TODO
}
