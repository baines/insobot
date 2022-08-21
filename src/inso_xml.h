#ifndef INSO_XML_H
#define INSO_XML_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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

// Flags for ixt_tokenize
enum {
	IXTF_NONE = 0,
	IXTF_SKIP_BLANK = (1 << 0), // don't create content tags that are only whitespace
	IXTF_TRIM       = (1 << 1), // remove whitespace at start/end of content
};

int  ixt_tokenize (char* data_in, uintptr_t* tokens_out, size_t token_count, int flags);
bool ixt_match    (uintptr_t* tokens, ...) __attribute__((sentinel));

#endif

#ifdef INSO_IMPL
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>

static char* ixt_skip_ws(char* p){
	while(*p && ((uint8_t)*p <= ' ')) ++p;
	return p;
}

static void ixt_unescape(char* msg, size_t len){

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

		// named replacements
		for(size_t i = 0; i < sizeof(tags) / sizeof(*tags); ++i){
			if(strncmp(p, tags[i].from, tags[i].from_len) == 0){
				const int sz = tags[i].from_len - tags[i].to_len;
				assert(sz >= 0);

				memmove(p, p + sz, len - (p - msg));
				memcpy(p, tags[i].to, tags[i].to_len);
				break;
			}
		}

		// numeric replacements
		wint_t wc;
		int old_len, new_len;
		if(*p == '&' &&
			(sscanf(p, "&#%u;%n" , &wc, &old_len) == 1 || sscanf(p, "&#x%x;%n", &wc, &old_len) == 1) &&
			(new_len = wctomb(c, wc)) > 0 &&
			old_len > new_len
		){
			memmove(p, p + (old_len - new_len), len - (p - msg));
			memcpy(p, c, new_len);
		}
	}
}

int ixt_tokenize(char* in, uintptr_t* tokens, size_t count, int flags){

#define IXT_ASSERT(x) if(!(x)){ fputs("ixt assertion failure: " #x "\n", stderr); goto invalid; }
#define IXT_EMIT(x) if((size_t)(t - tokens) < count-1){ *t++ = (uintptr_t)(x); } else { goto truncated; }

	enum {
		IXTS_DEFAULT,
		IXTS_INTAG,
		IXTS_INPI,
	} state = IXTS_DEFAULT;

	char* p = ixt_skip_ws(in);
	uintptr_t* t = tokens;

	const bool skip_blank_content = flags & IXTF_SKIP_BLANK;
	const bool trim_content       = flags & IXTF_TRIM;

	IXT_ASSERT(count >= 1);

	while(*p){
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
					IXT_ASSERT(end && "Missing comment end marker.");
					*end = 0;

					IXT_EMIT(IXT_COMMENT);
					IXT_EMIT(p+5);

					p = end + 2;
				} else if(strncmp(p+1, "![CDATA[", 8) == 0){ // cdata
					char* end = strstr(p+9, "]]>");
					IXT_ASSERT(end && "Missing CDATA end marker.");
					*end = 0;

					IXT_EMIT(IXT_CDATA);
					IXT_EMIT(p+10);

					p = end + 2;
				} else if(strncasecmp(p+1, "!DOCTYPE", 8) == 0){ // doctype
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
			} else { // content between tags
				size_t n = strcspn(p, "<");

				if(skip_blank_content && p + n == ixt_skip_ws(p)){
					p += n;
					continue;
				}

				IXT_EMIT(IXT_CONTENT);

				const bool eof = !p[n];
				p[n] = 0;
				ixt_unescape(p, n);

				char* q = p;
				if(trim_content){
					q = ixt_skip_ws(p);
				}

				IXT_EMIT(q);

				if(trim_content){
					q += strlen(q) - 1;
					while(q >= p && (uint8_t)(*q) <= ' '){
						*q-- = 0;
					}
				}

				p += n;

				if(eof){
					break;
				} else {
					goto checktag;
				}
			}
		} else { // intag / inpi
			p = ixt_skip_ws(p);
			if((state == IXTS_INTAG && *p == '/') || (state == IXTS_INPI && *p == '?')){ // end of self-closing tag
				IXT_ASSERT(p[1] == '>');

				IXT_EMIT(state == IXTS_INTAG ? IXT_TAG_CLOSE : IXT_PI_CLOSE);
				state = IXTS_DEFAULT;
				p += 2;
			} else if(*p == '>'){ // end of opening tag
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

					if(*p != '=') {
						// attribute without value
						continue;
					}

					++p;
				}

				p = ixt_skip_ws(p);
				char* q = p + 1;

				if(*p == '"' || *p == '\'') {
					while(*q && *q != *p) ++q;
					p++;
				} else {
					q = ixt_skip_ws(p);
				}

				*q = 0;

				IXT_EMIT(IXT_ATTR_VAL);
				IXT_EMIT(p);
				ixt_unescape(p, q-p);

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

bool ixt_match(uintptr_t* tokens, ...){
	va_list v;
	va_start(v, tokens);

	bool result = true;
	int index = 0;
	uintptr_t t;

	while((t = va_arg(v, uintptr_t))){
		if(t < IXT_COUNT){
			if(t != tokens[index]){
				result = false;
				break;
			}
		} else {
			if(strcmp((char*)tokens[index], (char*)t) != 0){
				result = false;
				break;
			}
		}
		++index;
	}

	va_end(v);
	return result;
}

#endif

#if INSO_TEST

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

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

	uintptr_t tokens[0x2000];
	ixt_tokenize(buf, tokens, 0x2000, IXTF_SKIP_BLANK | IXTF_TRIM);

	int nesting = 5;
	for(uintptr_t* t = tokens; *t; ++t){
		if(*t < IXT_COUNT){
			if(*t == IXT_TAG_CLOSE || *t == IXT_PI_CLOSE) nesting -= 2;
			printf("%*s: %s\n", nesting, "TOKEN", tnames[*t]);
			if(*t == IXT_TAG_OPEN || *t == IXT_PI_OPEN) nesting += 2;
		} else {
			printf("%*s: %s\n", nesting, "  VAL", (char*)(*t));
		}
	}

	fclose(f);

	return 0;
}
#endif
