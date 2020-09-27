#include "ibadmin.h"
#include <string.h>
#include <time.h>

struct esc {
	const char* key;
	sb(char)    val;
	int         mode;
};

enum {
	ESCAPE_NONE,
	ESCAPE_HTML,
	ESCAPE_JSON,
	ESCAPE_CSV,
};

static int escape_mode_get(char c){
	switch(c){
		case 'h':
			return ESCAPE_HTML;
		case 'j':
			return ESCAPE_JSON;
		case 'c':
			return ESCAPE_CSV;
		default:
			return ESCAPE_NONE;
	}
}

static void template_write(sb(char)* out, sb(struct esc)* esc, int mode, const char* key, const char* val){

	if(mode == ESCAPE_NONE){
		size_t sz = strlen(val);
		memcpy(sb_add(*out, sz), val, sz);
		return;
	}

	sb_each(e, *esc){
		if(e->mode == mode && strcmp(e->key, key) == 0){
			const size_t n = sb_count(e->val);
			memcpy(sb_add(*out, n), e->val, n);
			return;
		}
	}

	struct esc e = {
		.key  = key,
		.mode = mode,
	};

	switch(mode){
		case ESCAPE_HTML:
			escape_html(&e.val, val);
			break;
		case ESCAPE_JSON:
			escape_json(&e.val, val);
			break;
		case ESCAPE_CSV:
			escape_csv(&e.val, val);
			break;
		default:
			exit_error(501);
	}

	sb_push(*esc, e);

	const size_t n = sb_count(e.val);
	memcpy(sb_add(*out, n), e.val, n);
}

void template_append(sb(char)* out, const char* data, size_t len, const char** subst){
	const char* prev_p = data;
	const char* p;

	sb(struct esc) escaped = NULL;

	while((p = memchr(prev_p, '`', (data + len) - prev_p))){
		int n = p - prev_p;
		memcpy(sb_add(*out, n), prev_p, n);

		++p;

		const char* end = rawmemchr(p, '`');
		const size_t key_len = end-p;
		char* key = strndupa(p, key_len);

		int escape_mode = ESCAPE_NONE;
		if(key_len > 2 && key[key_len-2] == '|'){
			escape_mode = escape_mode_get(key[key_len-1]);
			key[key_len-2] = '\0';
		}

		for(const char** s = subst; *s; s+=2){
			if(strcmp(*s, key) == 0){
				template_write(out, &escaped, escape_mode, key, s[1]);
				break;
			}
		}

		prev_p = end+1;
	}

	sb_each(e, escaped){
		sb_free(e->val);
	}
	sb_free(escaped);

	int n = (data + len) - prev_p;
	memcpy(sb_add(*out, n), prev_p, n);

	sb_push(*out, 0);
	--stb__sbn(*out);
}

sb(char) template_bake(const char* data, size_t len, const char** subst){
	sb(char) result = NULL;
	template_append(&result, data, len, subst);
	return result;
}

void template_puts(const char* data, size_t len, const char** subst, int type, time_t mod){
	sb(char) result = template_bake(data, len, subst);
	util_output(result, sb_count(result), type, mod);
	sb_free(result);
}
