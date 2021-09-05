#include "stb_sb.h"
#include "ibstat.h"

#define S(s) s, sizeof(s)-1

void escape_html(sb(char)* out, const char* in){
	const char* p = in;

	static const struct {
		char from;
		const char* to;
		size_t to_len;
	} tags[] = {
		{ '<' , S("&lt;")   },
		{ '>' , S("&gt;")   },
		{ '&' , S("&amp;")  },
		{ '"' , S("&quot;") },
		{ '\'', S("&#39;")  },
	};

	for(;; ++p){
		size_t n = strcspn(p, "<>&'\"");
		memcpy(sb_add(*out, n), p, n);
		p += n;
		if(!*p) break;

		for(size_t i = 0; i < countof(tags); ++i){
			if(*p == tags[i].from){
				memcpy(sb_add(*out, tags[i].to_len), tags[i].to, tags[i].to_len);
				break;
			}
		}
	}
}

void escape_json(sb(char)* out, const char* in){
	const uint8_t* p = (const uint8_t*)in;

	static const char tab[] = {
		[0x08] = 'b',
		[0x09] = 't',
		[0x0a] = 'n',
		[0x0c] = 'f',
		[0x0d] = 'r',
		[0x22] = '"',
		[0x2f] = '/',
	};

	static const char hex[] = "0123456789abcdef";

	for(; *p; ++p){
		if(*p < 0x30 && tab[*p]){
			sb_push(*out, '\\');
			sb_push(*out, tab[*p]);
		} else if(*p < 0x20){
			sb_push(*out, '\\');
			sb_push(*out, 'u');
			sb_push(*out, '0');
			sb_push(*out, '0');
			sb_push(*out, hex[*p >> 4]);
			sb_push(*out, hex[*p & 15]);
		} else if(*p == '\\'){
			sb_push(*out, '\\');
			sb_push(*out, '\\');
		} else {
			sb_push(*out, *p);
		}
	}
}

void escape_csv(sb(char)* out, const char* in){
	for(const char* p = in; *p; ++p){
		if(*p == '"') sb_push(*out, '"');
		sb_push(*out, *p);
	}
}
