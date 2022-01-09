#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum uj_type {
	UJ_NULL,
	UJ_BOOL_FALSE,
	UJ_BOOL_TRUE,
	UJ_INT,
	UJ_DOUBLE,
	UJ_STR,
	UJ_OBJ,
	UJ_OBJ_CLOSE,
	UJ_ARR,
	UJ_ARR_CLOSE,
};

enum uj_status {
	/* lexer & parser */
	UJ_OK                       = 0,
	UJ_PARTIAL                  = 1,

	UJ_ERR_OOM                  = -2,
	UJ_ERR_MALFORMED            = -3,
	/* parser only */
	UJ_ERR_EXPECTED_KEY         = -4,
	UJ_ERR_UNEXPECTED_OBJ_CLOSE = -5,
	UJ_ERR_UNEXPECTED_ARR_CLOSE = -6,
	UJ_ERR_TOO_DEEP             = -41,
};

struct uj_lexer {
	uint_fast32_t state;
	uint_fast32_t num_state;
	uint_fast32_t udigits;
	uint_fast32_t ucode;

	const char* literal;

	char* work_mem;
	char* work_cur;
	char* work_end;
};

typedef _Bool (*uj_lex_callback)(struct uj_lexer*, enum uj_type, const char* data, size_t len);

///////////////////////

static void    uj_lex_init (struct uj_lexer*, char* work_mem, size_t work_mem_size);
enum uj_status uj_lex      (struct uj_lexer*, const char* json, size_t len, uj_lex_callback cb);

///////////////////////

static void uj_lex_init(struct uj_lexer* uj, char* work_mem, size_t work_mem_size){
	memset(uj, 0, sizeof(*uj));
	uj->work_mem = work_mem;
	uj->work_cur = work_mem;
	uj->work_end = work_mem + work_mem_size;
}

static inline int uj_lex_num(struct uj_lexer* uj, const char** json, size_t uj_len, uj_lex_callback callback, _Bool eof){
	const char* end = *json + uj_len;

	if(eof)
		goto eof_check;

	enum {
		UJ_NS_INITIAL,
		UJ_NS_ZERO,
		UJ_NS_DIGIT,
		UJ_NS_MANTISSA,
		UJ_NS_EXP_SIGN,
		UJ_NS_EXPONENT,
	};

	while(*json != end){
		char c = **json;
		int new_state = -1;

		if(c == ',' || c == '}' || c == ']' || c == ':' || c == ' ' || c == '\t' || c == '\r' || c == '\n'){
			int rv;
eof_check:
			switch(uj->num_state){
				case UJ_NS_ZERO:
				case UJ_NS_DIGIT: {
					rv = callback(uj, UJ_INT, uj->work_mem, uj->work_cur - uj->work_mem);
				} break;
				case UJ_NS_MANTISSA:
				case UJ_NS_EXPONENT: {
					rv = callback(uj, UJ_DOUBLE, uj->work_mem, uj->work_cur - uj->work_mem);
				} break;
				default:
					rv = UJ_ERR_MALFORMED;
			}

			--*json;
			uj->work_cur = uj->work_mem;
			uj->num_state = UJ_NS_INITIAL;
			return rv;
		}

		switch(uj->num_state){
			default:/* case UJ_NS_INITIAL:*/ {
				if(c == '0')
					new_state = UJ_NS_ZERO;
				else if(c >= '1' && c <= '9')
					new_state = UJ_NS_DIGIT;
			} break;
			case UJ_NS_ZERO: {
				if(c == '.')
					new_state = UJ_NS_MANTISSA;
				else if(c == 'e' || c == 'E')
					new_state = UJ_NS_EXP_SIGN;
			} break;
			case UJ_NS_DIGIT: {
				if(c >= '0' && c <= '9')
					new_state = UJ_NS_DIGIT;
				else if(c == '.')
					new_state = UJ_NS_MANTISSA;
				else if(c == 'e' || c == 'E')
					new_state = UJ_NS_EXP_SIGN;
			} break;
			case UJ_NS_MANTISSA: {
				if(c >= '0' && c <= '9')
					new_state = UJ_NS_MANTISSA;
				else if(c == 'e' || c == 'E')
					new_state = UJ_NS_EXP_SIGN;
			} break;
			case UJ_NS_EXP_SIGN: {
				if(c == '+' || c == '-' || (c >= '0' && c <= '9'))
					new_state = UJ_NS_EXPONENT;
			} break;
			case UJ_NS_EXPONENT: {
				if(c >= '0' && c <= '9')
					new_state = UJ_NS_EXPONENT;
			} break;
		}

		if(new_state == -1){
			uj->num_state = UJ_NS_INITIAL;
			return UJ_ERR_MALFORMED;
		}

		if(uj->work_cur == uj->work_end){
			uj->num_state = UJ_NS_INITIAL;
			return UJ_ERR_OOM;
		}

		uj->num_state = new_state;
		*uj->work_cur++ = *(*json)++;
	}

	return UJ_PARTIAL;
}

static inline char uj_lower(char c){
	if(c >= 'A' && c <= 'Z')
		return c + ('a' - 'A');
	return c;
}

int uj_lex(struct uj_lexer* uj, const char* json, size_t len, uj_lex_callback callback){
	const char* p   = json;
	const char* end = json + len;
	const _Bool eof = len == 0;

	goto *&&resume_start + uj->state;

	for(;; ++p){
		uj->state = 0;

resume_start:
		if(p == end)
			break;

		if(*p == '\r' || *p == '\n' || *p == '\t' || *p == ' ' || *p == ':' || *p == ',')
			continue;

		static const char objarr[] = "{}[]";
		const char* c;
		int rv;

		uj->literal =
			*p == 't' ? "true" :
			*p == 'f' ? "false" :
			*p == 'n' ? "null" :
			NULL;

		if(uj->literal){
			uj->state = &&resume_literal - &&resume_start;
resume_literal:
			for(;;){
				if(eof || *p != *uj->literal)
					return UJ_ERR_MALFORMED;

				if(!*++uj->literal){
					break;
				}

				if(++p == end)
					return UJ_PARTIAL;
			}

			char k = uj->literal[-2];
			int type = UJ_NULL + (k == 's') + ((k == 'u') << 1);
			callback(uj, type, NULL, 0);
			uj->literal = NULL;
		}

		else if(*p == '-' || (*p >= '0' && *p <= '9')){
			if(*p == '-'){
				*uj->work_cur++ = '-';
				++p;
			}
			uj->state = &&resume_number - &&resume_start;
resume_number:
			rv = uj_lex_num(uj, &p, end - p, callback, eof);
			if(rv <= 0)
				return rv;
		}

		else if(*p == '"'){
			uj->state = &&resume_str - &&resume_start;
			++p;
resume_str:
			for(;;){
				if(p == end)
					return UJ_PARTIAL;

				if(*p == '"'){
					rv = callback(uj, UJ_STR, uj->work_mem, uj->work_cur - uj->work_mem);
					uj->work_cur = uj->work_mem;
					break;
				} else if(*p == '\\'){
					static const char from[] = "\"\\/bfnrt";
					static const char to[]   = "\"\\/\b\f\n\r\t";
					uj->state = &&resume_str_unescape_1 - &&resume_start;
					++p;
resume_str_unescape_1:
					if(p == end)
						return UJ_PARTIAL;

					if(uj->work_cur == uj->work_end)
						return UJ_ERR_OOM;

					if((c = memchr(from, *p, sizeof(from)))){
						++p;
						*uj->work_cur++ = to[c - from];
						uj->state = &&resume_str - &&resume_start;
					} else if(*p == 'u') {
						++p;
						uj->state = &&resume_str_unescape_2 - &&resume_start;
resume_str_unescape_2:
						for(; uj->udigits < 4; ++uj->udigits){
							if(p == end)
								return UJ_PARTIAL;

							static const char hex[] = "0123456789abcdef";
							c = memchr(hex, uj_lower(*p), sizeof(hex));
							if(!c)
								return UJ_ERR_MALFORMED;

							uj->ucode = (uj->ucode << 4) | (c - hex);
							++p;
						}

						// convert to utf-8
						static const size_t counts[] = { 0x7f, 0x7ff };
						int i;
						for(i = 0; i < 2; ++i){
							if(uj->ucode <= counts[i])
								break;
						}

						if(uj->work_end - uj->work_cur < (i+1))
							return UJ_ERR_OOM;

						if(i == 0){
							*uj->work_cur++ = uj->ucode;
						} else if(i == 1){
							*uj->work_cur++ = (uj->ucode >> 6) | 0xc0;
							*uj->work_cur++ = (uj->ucode & 0x3f) | 0x80;
						} else {
							*uj->work_cur++ = (uj->ucode >> 12) | 0xe0;
							*uj->work_cur++ = ((uj->ucode >> 6) & 0x3f) | 0x80;
							*uj->work_cur++ = (uj->ucode & 0x3f) | 0x80;
						}

						uj->udigits = uj->ucode = 0;
						uj->state = &&resume_str - &&resume_start;
					} else {
						return UJ_ERR_MALFORMED;
					}
				} else {
					if(uj->work_cur == uj->work_end)
						return UJ_ERR_OOM;

					*uj->work_cur++ = *p++;
				}
			}
		} else if((c = memchr(objarr, *p, sizeof(objarr)))){
			rv = callback(uj, UJ_OBJ + (c - objarr), NULL, 0);
			if(rv <= 0)
				return rv;
		} else {
			return UJ_ERR_MALFORMED;
		}
	}

	return UJ_OK;
}
#include <stdlib.h>
#include <inttypes.h>

#ifndef UJ_PARSE_BUF_SIZE
#define UJ_PARSE_BUF_SIZE 4096
#endif

#ifndef UJ_PARSE_MAX_DEPTH
#define UJ_PARSE_MAX_DEPTH 256
#endif

struct uj_kv {
	char* key;
	struct uj_node* val;
	struct uj_kv* next;
};

struct uj_node {
	enum uj_type            type;
	union {
		intmax_t            num;
		char*               str;
		double              dbl;
		struct uj_kv*       obj;
		struct {
			struct uj_node* arr;
			size_t          len;
		};
	};
};

struct uj_level {
	enum uj_type       type;
	struct uj_node** next;
	union {
		struct uj_kv** next_kv;
		size_t* len;
	};
};

struct uj_parser {
	struct uj_lexer uj;
	struct uj_level* lvl;
	struct uj_level level_storage[UJ_PARSE_MAX_DEPTH];
	struct uj_node* root_node;
	enum uj_status err;
	_Bool initialized;
	char mem[UJ_PARSE_BUF_SIZE];
};

///////////////////////

struct uj_node*  uj_parse           (const char* json , size_t len, enum uj_status* err);
enum   uj_status uj_parse_chunk     (struct uj_parser*, const char* chunk, size_t len);
struct uj_node*  uj_parse_chunk_end (struct uj_parser*);
void             uj_node_free       (struct uj_node*, _Bool free_self);

///////////////////////

static inline _Bool uj_parser_cb(struct uj_lexer* p, enum uj_type type, const char* data, size_t len){
	struct uj_parser* j = (struct uj_parser*)p;
	struct uj_level* l = j->lvl;

#define fail(x) return j->err = (x), 0

	if(l->type == UJ_OBJ && !l->next) {
		if(type == UJ_STR) {
			*l->next_kv = calloc(1, sizeof(**l->next_kv));
			(*l->next_kv)->key = strndup(data, len);
			l->next = &(*l->next_kv)->val;
			l->next_kv = &(*l->next_kv)->next;
		} else if(type == UJ_OBJ_CLOSE) {
			j->lvl--;
		} else {
			fail(UJ_ERR_EXPECTED_KEY);
		}
	} else if(l->type == UJ_ARR && type == UJ_ARR_CLOSE) {
		j->lvl--;
	} else {
		if(!l->next)
			return 0;

		struct uj_node* node;
		if(l->type == UJ_ARR) {
			*l->next = realloc(*l->next, (*l->len + 1) * sizeof(**l->next));
			node = &(*l->next)[(*l->len)++];
			memset(node, 0, sizeof(*node));
		} else {
			node = calloc(1, sizeof(**l->next));
		}

		node->type = type;

		switch(type) {
			case UJ_STR: {
				node->str = strndup(data, len);
			} break;
			case UJ_INT: {
				node->num = strtoimax(strndupa(data, len), NULL, 10);
			} break;
			case UJ_DOUBLE: {
				node->dbl = strtod(strndupa(data, len), NULL);
			} break;
			case UJ_ARR: {
				if(++j->lvl - j->level_storage == UJ_PARSE_MAX_DEPTH)
					fail(UJ_ERR_TOO_DEEP);
				j->lvl->type = type;
				j->lvl->next = &node->arr;
				j->lvl->len = &node->len;
			} break;
			case UJ_OBJ: {
				if(++j->lvl - j->level_storage == UJ_PARSE_MAX_DEPTH)
					fail(UJ_ERR_TOO_DEEP);
				j->lvl->type = type;
				j->lvl->next = NULL;
				j->lvl->next_kv = &node->obj;
			} break;
			case UJ_ARR_CLOSE: fail(UJ_ERR_UNEXPECTED_ARR_CLOSE);
			case UJ_OBJ_CLOSE: fail(UJ_ERR_UNEXPECTED_OBJ_CLOSE);
			default: break;
		}

		if(l->type != UJ_ARR) {
			*l->next = node;
			l->next = NULL;
		}
	}

#undef fail

	return 1;
}

enum uj_status uj_parse_chunk(struct uj_parser* p, const char* chunk, size_t len) {
	if(!p->initialized) {
		p->initialized = 1;
		uj_lex_init(&p->uj, p->mem, sizeof(p->mem));
		p->lvl = p->level_storage;
		p->lvl->next = &p->root_node;
	}

	if(p->err < UJ_OK) {
		return p->err;
	}

	enum uj_status status = uj_lex(&p->uj, chunk, len, &uj_parser_cb);

	if(p->err == UJ_OK) {
		p->err = status;
	}

	return p->err;
}

struct uj_node* uj_parse_chunk_end(struct uj_parser* p) {
	if(p->err == UJ_PARTIAL) {
		p->err = uj_lex(&p->uj, NULL, 0, &uj_parser_cb);
	}

	if(p->err != UJ_OK) {
		uj_node_free(p->root_node, 1);
		return NULL;
	}

	return p->root_node;
}

struct uj_node* uj_parse(const char* json, size_t len, enum uj_status* err) {
	struct uj_parser parser = {};
	enum uj_status status = uj_parse_chunk(&parser, json, len);
	if(status != UJ_OK && err) {
		*err = status;
	}
	return uj_parse_chunk_end(&parser);
}

void uj_node_free(struct uj_node* n, _Bool free_self) {
	if(!n)
		return;

	switch(n->type) {
		case UJ_STR: {
			free(n->str);
		} break;
		case UJ_OBJ: {
			struct uj_kv* next;
			for(struct uj_kv* kv = n->obj; kv; kv = next) {
				next = kv->next;
				free(kv->key);
				uj_node_free(kv->val, 1);
				free(kv);
			}
		} break;
		case UJ_ARR: {
			for(size_t i = 0; i < n->len; ++i) {
				uj_node_free(n->arr + i, 0);
			}
			free(n->arr);
		} break;
		default: break;
	}

	if(free_self)
		free(n);
}
