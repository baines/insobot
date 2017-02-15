#ifndef INSO_XML_H
#define INSO_XML_H
#include <stddef.h>
#include <stdint.h>

// Tokens.
//   "next = " signifies that the next token is a char* cast to uintptr_t with additional data.
enum {
	IXT_EOF = 0,
	IXT_TAG_OPEN,  // next = tag name
	IXT_TAG_CLOSE,
	IXT_PI_OPEN,   // next = processing instruction name
	IXT_PI_CLOSE,
	IXT_ATTR_KEY,  // next = attribute key name
	IXT_ATTR_VAL,  // next = attribute value
	IXT_CONTENT,   // next = tag content
	IXT_COMMENT,   // next = comment
	IXT_CDATA,     // next = raw CDATA stuff
	IXT_DOCTYPE,   // next = doctype (unparsed)

	IXT_COUNT,
};

// Returned by ixt_tokenize
enum {
	IXTR_OK,
	IXTR_TRUNCATED,
	IXTR_INVALID,
};

int ixt_tokenize(char* data_in, uintptr_t* tokens_out, size_t token_count);

#endif

#ifdef INSO_IMPL
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <stdlib.h>

char* ixt_skip_ws(char* p){
	while(*p && (*p <= ' ')) ++p;
	return p;
}

void ixt_unescape(char* msg, size_t len){

	typedef struct {
		char *from, *to;
		size_t from_len, to_len;
	} Replacement;

	#define RTAG(x, y) { .from = (x), .to = (y), .from_len = sizeof(x) - 1, .to_len = sizeof(y) - 1 }
	Replacement tags[] = {
		// there are loads more, meh
		RTAG("&amp;", "&"),
		RTAG("&gt;", ">"),
		RTAG("&lt;", "<"),
		RTAG("&quot;", "\""),
		RTAG("&apos;", "'"),
		RTAG("&nbsp;", " "),
		RTAG("&mdash;", "—"),
		RTAG("&raquo;", "»"),

		// not really html tags, but useful replacements
		RTAG("\n", " "),
		RTAG("\r", " "),
		RTAG("\t", " "),
	};
	#undef RTAG

	char c[MB_LEN_MAX];

	for(char* p = msg; *p; ++p){
		for(size_t i = 0; i < sizeof(tags) / sizeof(*tags); ++i){
			if(strncmp(p, tags[i].from, tags[i].from_len) == 0){
				const int sz = tags[i].from_len - tags[i].to_len;
				assert(sz >= 0);

				memmove(p, p + sz, len - (p - msg));
				memcpy(p, tags[i].to, tags[i].to_len);
				break;
			}
		}

		wchar_t wc;
		int old_len, new_len;
		if(sscanf(p, "&#%u;%n", &wc, &old_len) == 1){
			if((new_len = wctomb(c, wc)) > 0 && old_len > new_len){
				memmove(p, p + (old_len - new_len), len - (p - msg));
				memcpy(p, c, new_len);
			}
		}
	}
}

int ixt_tokenize(char* in, uintptr_t* tokens, size_t count){

#define IXT_ASSERT(x) if(!(x)){ fputs("ixt assertion failure: " #x "\n", stderr); goto invalid; }
#define IXT_EMIT(x) if(t - tokens < count-1){ *t++ = (uintptr_t)(x); } else { goto truncated; }

	IXT_ASSERT(count >= 1);

	enum {
		IXTS_DEFAULT,
		IXTS_INTAG,
		IXTS_INPI,
	} state = IXTS_DEFAULT;

	char* p = in;
	uintptr_t* t = tokens;

	while((p = ixt_skip_ws(p)), *p){
		if(state == IXTS_DEFAULT){
			if(*p == '<'){
				size_t n;
checktag:
				n = 0;
				if(p[1] == '/'){ // end tag
					n = strcspn(++p, ">");
					IXT_EMIT(IXT_TAG_CLOSE);
				} else if(p[1] == '?'){ // processing instruction
					n = strcspn(p+=2, " ?");

					IXT_EMIT(IXT_PI_OPEN);
					IXT_EMIT(p);

					if(p[n] == '?'){
						IXT_EMIT(IXT_PI_CLOSE);
					} else {
						state = IXTS_INPI;
					}
				} else if(p[1] == '!' && p[2] == '-' && p[3] == '-'){ // comment
					char* end = strstr(p+3, "-->");
					if(!end) break;
					*end = 0;

					IXT_EMIT(IXT_COMMENT);
					IXT_EMIT(p+5);

					p = end + 2;
				} else if(strncmp(p+1, "![CDATA[", 8) == 0){ // cdata
					char* end = strstr(p+9, "]]>");
					if(!end) break;
					*end = 0;

					IXT_EMIT(IXT_CDATA);
					IXT_EMIT(p+10);

					p = end + 2;
				} else if(strncmp(p+1, "!DOCTYPE", 8) == 0){ // doctype
					// XXX: doesn't parse the element / attrlist stuff because i don't care
					char* start = p + 10;

					n = strcspn(p+=9, ">[");
					if(p[n] == '['){
						p = strchr(p + n + 1, ']');
						if(!p) p = p + n;
						n = strcspn(p, ">");
					}

					IXT_EMIT(IXT_DOCTYPE);
					IXT_EMIT(start);
				} else { // start tag
					n = strcspn(++p, "/> \r\n\t");

					IXT_EMIT(IXT_TAG_OPEN);
					IXT_EMIT(p);

					if(p[n] == '/'){
						IXT_EMIT(IXT_TAG_CLOSE);
						p[n] = 0;
						++p;
					} else if(p[n] != '>'){
						state = IXTS_INTAG;
					}
				}

				p[n] = 0;
				p += n + 1;
			} else {
				size_t n = strcspn(p, "<");
				IXT_EMIT(IXT_CONTENT);
				IXT_EMIT(p);

				p[n] = 0;
				ixt_unescape(p, n);
				p += n;

				goto checktag;
			}
		} else { // intag / inpi
			if((state == IXTS_INTAG && *p == '/') || (state == IXTS_INPI && *p == '?')){
				IXT_ASSERT(p[1] == '>');

				IXT_EMIT(state == IXTS_INTAG ? IXT_TAG_CLOSE : IXT_PI_CLOSE);
				state = IXTS_DEFAULT;
				p += 2;
			} else if(*p == '>'){
				state = IXTS_DEFAULT;
				++p;
			} else { // attr
				size_t n = strcspn(p, "= \r\n\t");

				IXT_EMIT(IXT_ATTR_KEY);
				IXT_EMIT(p);

				int skip_ws = p[n] != '=';
				p[n] = 0;
				p += n + 1;

				if(skip_ws){
					p = ixt_skip_ws(p);
					IXT_ASSERT(*p == '=');
					++p;
				}

				p = ixt_skip_ws(p);
				IXT_ASSERT(*p == '"' || *p == '\'');
				char* q = p + 1;
				while(*q && *q != *p) ++q;
				*q = 0;

				IXT_EMIT(IXT_ATTR_VAL);
				IXT_EMIT(p+1);
				ixt_unescape(p+1, q-(p+1));

				p = q+1;
			}
		}
	}

#undef IXT_EMIT
#undef IXT_ASSERT

	*t++ = IXT_EOF;
	return IXTR_OK;

invalid:
	*t++ = IXT_EOF;
	return IXTR_INVALID;

truncated:
	*t++ = IXT_EOF;
	return IXTR_TRUNCATED;
}

#endif

#if 0
int main(int argc, char** argv){
	const char* fname = argc >= 2 ? argv[1] : "feed.xml";
	
	static const char* tnames[] = {
		"IXT_EOF",
		"IXT_TAG_OPEN",
		"IXT_TAG_CLOSE",
		"IXT_PI_OPEN",
		"IXT_PI_CLOSE",
		"IXT_ATTR_KEY",
		"IXT_ATTR_VAL",
		"IXT_CONTENT",
		"IXT_COMMENT",
		"IXT_CDATA",
		"IXT_DOCTYPE",
	};

	FILE* f = fopen(fname, "r");
	assert(f);

	fseek(f, 0, SEEK_END);
	int sz = ftell(f);
	rewind(f);

	char* buf = malloc(sz+1);
	fread(buf, sz, 1, f);
	buf[sz] = 0;

	uintptr_t tokens[0x1000];
	ixt_tokenize(buf, tokens, 0x1000);

	int nesting = 5;
	for(uintptr_t* t = tokens; *t; ++t){
		if(*t < IXT_COUNT){
			if(*t == IXT_TAG_CLOSE || *t == IXT_PI_CLOSE) nesting -= 2;
			printf("%*s: %s\n", nesting, "TOKEN", tnames[(int)*t]);
			if(*t == IXT_TAG_OPEN || *t == IXT_PI_OPEN) nesting += 2;
		} else {
			printf("%*s: %s\n", nesting, "  VAL", (char*)(*t));
		}
	}

	fclose(f);

	return 0;
}
#endif
