#include <string.h>
#include "../../src/stb_sb.h"

#define S(s) s, sizeof(s)-1
#define countof(x) (sizeof(x)/sizeof(*x))

sb(char) escape_html(const char* in){
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

	sb(char) out = NULL;

	for(;; ++p){
		size_t n = strcspn(p, "<>&'\"");
		memcpy(sb_add(out, n), p, n);
		p += n;
		if(!*p) break;

		for(size_t i = 0; i < countof(tags); ++i){
			if(*p == tags[i].from){
				memcpy(sb_add(out, tags[i].to_len), tags[i].to, tags[i].to_len);
				break;
			}
		}
	}

	sb_push(out, 0);
	return out;
}
