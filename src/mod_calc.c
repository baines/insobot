#include "module.h"
#include "inso_utils.h"
#include "stb_sb.h"
#include <ctype.h>
#include <setjmp.h>
#include <math.h>
#include <float.h>
#include <limits.h>

#if __GNUC__ < 5
#define MSB(l) (l ? LONG_BIT - __builtin_clzl(l) : 0)
static inline bool mul_overflow(long _a, long _b, long* c){
	unsigned long a = _a, b = _b;
	return (MSB(a) + MSB(b) >= LONG_BIT) ? true : ((*c = a*b), false);
}
static inline bool add_overflow(long a, long b, long* c){
	return b <= 0
		? ((LONG_MIN - b) <= a ? ((*c=a+b),false) : true)
		: ((LONG_MAX - b) >= a ? ((*c=a+b),false) : true);
}
static inline bool sub_overflow(long a, long b, long* c){
	return b <= 0
		? (LONG_MAX + b >= a) ? ((*c=a-b),false) : true
		: (LONG_MIN + b <= a) ? ((*c=a-b),false) : true;
}
#undef MSB
#else /* GNUC >= 5 */
#define mul_overflow __builtin_mul_overflow
#define add_overflow __builtin_add_overflow
#define sub_overflow __builtin_sub_overflow
#endif

static void calc_cmd  (const char*, const char*, const char*, int);
static bool calc_init (const IRCCoreCtx*);

enum { CALC_EXEC };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "calc",
	.desc     = "Perform calculations",
	.flags    = IRC_MOD_GLOBAL,
	.on_cmd   = &calc_cmd,
	.on_init  = &calc_init,
	.commands = DEFINE_CMDS(
		[CALC_EXEC] = CMD("calc")
	),
	.cmd_help = DEFINE_CMDS(
		[CALC_EXEC] = "<expr> | Calculate the result of the mathematical expression <expr>"
	),
};

static const IRCCoreCtx* ctx;

enum {
	OP_ADD,    // +
	OP_SUB,    // -
	OP_DIV,    // /
	OP_MUL,    // *
	OP_MOD,    // %
	OP_EXP,    // **
	OP_AND,    // &
	OP_OR,     // |
	OP_XOR,    // ^
	OP_SHR,    // >>
	OP_SHL,    // <<

	OP_UNARY = 0x10,
	OP_NOP   = OP_UNARY, // also +
	OP_NEG,              // also -

	OP_PAREN_OPEN,
	OP_PAREN_CLOSE,

	OP_COUNT,
};

static int precedence[OP_COUNT] = {
	[OP_NOP] = 8,
	[OP_NEG] = 8,
	[OP_EXP] = 7,
	[OP_DIV] = 6,
	[OP_MUL] = 6,
	[OP_MOD] = 6,
	[OP_ADD] = 5,
	[OP_SUB] = 5,
	[OP_SHR] = 4,
	[OP_SHL] = 4,
	[OP_AND] = 3,
	[OP_XOR] = 2,
	[OP_OR]  = 1,
	[OP_PAREN_OPEN] = -1,
	[OP_PAREN_CLOSE] = -1,
};

static bool right_assoc[OP_COUNT] = {
	[OP_EXP] = true,
};

enum {
	T_NONE,

	T_OP,
	T_PAREN_OPEN,
	T_PAREN_CLOSE,

	T_CONSTANT_ADD,
	T_CONSTANT_EXP,

	T_NUM = 0x10,
	T_LONG = T_NUM,
	T_FLOAT,
};

union num {
	float f;
	long l;
};

typedef struct {
	long type;
	union num data;
} token;

typedef struct {
	token prev;
	long base;
	jmp_buf errbuf;
	bool eof;
} calc_state;

enum {
	ERR_NONE,
	ERR_DBZ,
	ERR_TKN,
	ERR_PARENS,
	ERR_NON_FLOAT,
	ERR_UNKNOWN,
};

static const char* errmsg[] = {
	[ERR_DBZ] = "\001ACTION bursts into flames after dividing by zero.\001",
	[ERR_TKN] = "%s: Error: Unknown token '%.*s'.",
	[ERR_PARENS] = "%s: Error: Unbalanced parentheses.",
	[ERR_NON_FLOAT] = "%s: Cannot apply bitwise op to float.",
	[ERR_UNKNOWN] = "%s: wat?",
};

static const char ops[] = "+-/*%*&|^><()";

static struct {
	const char* str;
	size_t str_len;
	float value;
} constants[] = {
	{ "pi" , 2, M_PI     },
	{ "tau", 3, M_PI*2.f },
};

static struct {
	uint32_t codepoint;
	float value;
	int type;
} unicode_constants[] = {
	{ 'e'   , M_E     , OP_MUL },
	{ 0x03C0, M_PI    , OP_MUL },
	{ 0x03C4, M_PI*2.f, OP_MUL },
	{ 0x00B2, 2       , T_CONSTANT_EXP },
	{ 0x00B3, 3       , T_CONSTANT_EXP },
	{ 0x00BC, 1.f/4.f , T_CONSTANT_ADD },
	{ 0x00BD, 1.f/2.f , T_CONSTANT_ADD },
	{ 0x00BE, 3.f/4.f , T_CONSTANT_ADD },
	{ 0x2150, 1.f/7.f , T_CONSTANT_ADD },
	{ 0x2151, 1.f/9.f , T_CONSTANT_ADD },
	{ 0x2152, 0.1f    , T_CONSTANT_ADD },
	{ 0x2153, 1.f/3.f , T_CONSTANT_ADD },
	{ 0x2154, 2.f/3.f , T_CONSTANT_ADD },
	{ 0x2155, 1.f/5.f , T_CONSTANT_ADD },
	{ 0x2156, 2.f/5.f , T_CONSTANT_ADD },
	{ 0x2157, 3.f/5.f , T_CONSTANT_ADD },
	{ 0x2158, 4.f/5.f , T_CONSTANT_ADD },
	{ 0x2159, 1.f/6.f , T_CONSTANT_ADD },
	{ 0x215A, 5.f/6.f , T_CONSTANT_ADD },
	{ 0x215B, 1.f/8.f , T_CONSTANT_ADD },
	{ 0x215C, 3.f/8.f , T_CONSTANT_ADD },
	{ 0x215D, 5.f/8.f , T_CONSTANT_ADD },
	{ 0x215E, 7.f/8.f , T_CONSTANT_ADD },
};

static inline token token_op(long op){
	return (token){ T_OP, { .l = op }};
}

static token check_constants(const char** str, calc_state* state){

	size_t max = strlen(*str);
	for(size_t i = 0; i < ARRAY_SIZE(constants); ++i){
		if(max >= constants[i].str_len && strncmp(*str, constants[i].str, constants[i].str_len) == 0){
			*str += constants[i].str_len;
			return (token){ T_FLOAT, { .f = constants[i].value }};
		}
	}

	wchar_t wc;
	int n = mbtowc(&wc, *str, MB_CUR_MAX);

	for(size_t i = 0; i < ARRAY_SIZE(unicode_constants); ++i){
		if(unicode_constants[i].codepoint == (uint32_t)wc){
			int type = T_FLOAT;

			// FIXME: silly logic...
			if(unicode_constants[i].type == T_CONSTANT_EXP){
				type = T_CONSTANT_EXP;
			} else if((state->prev.type & T_NUM) && unicode_constants[i].type != OP_MUL){
				type = unicode_constants[i].type;
			}

			*str += n;
			return (token){ type, { .f = unicode_constants[i].value }};
		}
	}

	return (token){ T_NONE };
}

static bool is_unary(calc_state* state){
	return state->prev.type == T_OP || state->prev.type == T_PAREN_OPEN;
}

static token calc_next_token(const char** str, calc_state* state){
	char* p;

	if(!**str){
		state->eof = true;
		return (token){ T_PAREN_CLOSE };
	}

	while(**((uint8_t**)str) <= ' '){
		++*str;
	}

	{
		token t = check_constants(str, state);
		if(t.type != T_NONE){
			return t;
		}
	}

	if((p = strchr(ops, **str))){
		++*str;

		if(is_unary(state) && (*p == '+' || *p == '-')){ // unary +, -
			return token_op(OP_UNARY + (p - ops));
		} else if(p - ops > 10){
			return (token){ T_PAREN_OPEN + (*p == ')') }; // parens
		} else if(*p == '*' && **str == '*'){ // exp vs mul
			++*str;
			return token_op(OP_EXP);
		} else if((*p == '>' || *p == '<') && *(*str)++ != *p){ // bitshifts
			*str -= 2;
			longjmp(state->errbuf, ERR_TKN);
		} else { // everything else
			return token_op(p - ops);
		}
	}

	// must be a number if we get here

	const char* digits = "0123456789";
	long base = 10;
	long num = 0;
	float fnum = 0.0f;
	bool is_float = false;

	if(**str == '0'){
		if((*str)[1] == 'x' || (*str)[1] == 'X'){
			digits = "0123456789abcdef";
			base = 16;
			*str += 2;
		} else if((*str)[1] == 'b' || (*str)[1] == 'B'){
			digits = "01";
			base = 2;
			*str += 2;
		}
	}

	if(**str != '.' && !strchr(digits, tolower(**str))){
		longjmp(state->errbuf, ERR_TKN);
	}

	// implicit multiply for consecutive number tokens
	if((state->prev.type & T_NUM) || state->prev.type == T_PAREN_CLOSE){
		return token_op(OP_MUL);
	}

	while(**str && (p = strchr(digits, tolower(**str)))){
		num = (num * base) + (p - digits);
		++*str;
	}

	// real numbers
	if(**str == '.'){
		is_float = true;
		++*str;
		int i = 10;
		while(**str && (p = strchr(digits, tolower(**str)))){
			fnum += (1.0f / (float)i) * (p - digits);
			++*str;
			i *= 10;
		}
	}
	fnum += num;

	// scientific notation
	if(**str == 'e'){
		is_float = true;
		float exp = 0.0f;
		int sign = 1;
		++*str;

		if(**str == '+') ++*str;
		else if(**str == '-'){
			sign = -1;
			++*str;
		}

		while(**str && (p = strchr(digits, tolower(**str)))){
			exp = (exp * 10.0f) + (p - digits);
			++*str;
		}

		fnum *= powf(10.0f, sign * exp);
	}

	state->base = INSO_MAX(state->base, base);

	if(is_float){
		return (token){ T_FLOAT, { .f = fnum } };
	} else {
		return (token){ T_LONG, { .l = num } };
	}
}

static long lpow(long num, unsigned long exp, bool* oflow){
	long result = 1;

	while(exp){
		if(exp & 1){
			if(mul_overflow(result, num, &result)) goto overflow;
		}
		exp >>= 1;
		if(mul_overflow(num, num, &num)) goto overflow;
	}

	return result;

overflow:
	if(oflow) *oflow = true;
	return 0;
}

static token enfloaten(token t){
	if(t.type == T_LONG){
		t.data.f = (float)t.data.l;
		t.type = T_FLOAT;
	}
	return t;
}

static void calc_apply_op(long op, token** nums, calc_state* state){
	bool is_unary = op & OP_UNARY;

	if(sb_count(*nums) < (2u - is_unary)){
		longjmp(state->errbuf, ERR_UNKNOWN);
	}

	token ta = {};
	token tb = sb_last(*nums); sb_pop(*nums);

	if(!is_unary){
		ta = sb_last(*nums); sb_pop(*nums);

		if(ta.type != tb.type || op == OP_DIV || (op == OP_EXP && tb.type == T_LONG && tb.data.l < 0)){
			ta = enfloaten(ta);
			tb = enfloaten(tb);
		}

		if((op == OP_DIV || op == OP_MOD)
			&& ((tb.type == T_FLOAT && fabs(tb.data.f) < FLT_EPSILON) ||
			    (tb.type == T_LONG && !tb.data.l))){
			longjmp(state->errbuf, ERR_DBZ);
		}
	}

	if(tb.type == T_LONG){
		long result, a = ta.data.l, b = tb.data.l;
		bool overflow = false;

		switch(op){
			case OP_ADD: overflow = add_overflow(a, b, &result); break;
			case OP_SUB: overflow = sub_overflow(a, b, &result); break;
			case OP_DIV: result = a / b; break;
			case OP_MUL: overflow = mul_overflow(a, b, &result); break;
			case OP_MOD: result = a % b; break;
			case OP_EXP: result = lpow(a, b, &overflow); break;
			case OP_AND: result = a & b; break;
			case OP_OR : result = a | b; break;
			case OP_XOR: result = a ^ b; break;
			case OP_SHR: result = a >> b; break;
			case OP_SHL: {
				unsigned long _a = a, _b = b;
				result = _b >= LONG_BIT ? 0 : _a << _b;
			} break;
			case OP_NEG: result = -b; break;
			case OP_NOP: result = b; break;
		}

		if(overflow){
			ta = enfloaten(ta);
			tb = enfloaten(tb);
		} else {
			token t = { T_LONG, { .l = result }};
			sb_push(*nums, t);
		}
	}

	if(tb.type == T_FLOAT){
		float result, a = ta.data.f, b = tb.data.f;

		switch(op){
			case OP_ADD: result = a + b; break;
			case OP_SUB: result = a - b; break;
			case OP_DIV: result = a / b; break;
			case OP_MUL: result = a * b; break;
			case OP_MOD: result = fmodf(a, b); break;
			case OP_EXP: result = powf(a, b); break;
			case OP_NEG: result = -b; break;
			case OP_NOP: result = b; break;
			default: {
				longjmp(state->errbuf, ERR_NON_FLOAT);
			};
		}

		token t = { T_FLOAT, { .f = result }};
		sb_push(*nums, t);
	}
}

static void calc_cmd(const char* chan, const char* name, const char* arg, int cmd){
	if(!*arg++){
		ctx->send_msg(chan, "%s: Give me an expression to calculate.", name);
		return;
	}

	long* op_stack = NULL;
	token* num_stack = NULL;

	token t;
	calc_state state = { .prev = { T_PAREN_OPEN }};

	int err;
	if((err = setjmp(state.errbuf))){
		int tlen = mblen(arg, MB_CUR_MAX);
		ctx->send_msg(chan, errmsg[err], name, tlen, arg);
		goto out;
	}

	sb_push(op_stack, OP_PAREN_OPEN);

	do {
		t = calc_next_token(&arg, &state);

		if(t.type == T_CONSTANT_ADD){

			t.type = T_FLOAT;
			sb_push(num_stack, t);
			calc_apply_op(OP_ADD, &num_stack, &state);

		} else if(t.type == T_CONSTANT_EXP){

			t.type = T_LONG;
			t.data.l = (long)t.data.f;
			sb_push(num_stack, t);
			calc_apply_op(OP_EXP, &num_stack, &state);

		} else if(t.type & T_NUM){

			sb_push(num_stack, t);

		} else if(t.type == T_OP){

			while(sb_count(op_stack) && (precedence[sb_last(op_stack)] - precedence[t.data.l]) >= right_assoc[t.data.l]){
				calc_apply_op(sb_last(op_stack), &num_stack, &state);
				sb_pop(op_stack);
			}
			sb_push(op_stack, t.data.l);

		} else if(t.type == T_PAREN_OPEN){

			sb_push(op_stack, OP_PAREN_OPEN);

		} else { // close paren

			while(sb_count(op_stack) && sb_last(op_stack) != OP_PAREN_OPEN){
				calc_apply_op(sb_last(op_stack), &num_stack, &state);
				sb_pop(op_stack);
			}

			if(!sb_count(op_stack)){
				longjmp(state.errbuf, ERR_PARENS);
			}

			sb_pop(op_stack);
		}

		state.prev = t;
	} while(!state.eof);

	if(sb_count(op_stack)>0) longjmp(state.errbuf, ERR_PARENS);
	if(!sb_count(num_stack)) longjmp(state.errbuf, ERR_UNKNOWN);

	while(sb_count(num_stack) > 1){
		calc_apply_op(OP_MUL, &num_stack, &state);
	}

	t = sb_last(num_stack);
	if(t.type == T_FLOAT){
		ctx->send_msg(chan, "%s: %g.", name, t.data.f);
	} else {
		const char* fmt = state.base == 16 ? "%s: %#lx." : "%s: %ld.";
		ctx->send_msg(chan, fmt, name, t.data.l);
	}

out:
	sb_free(op_stack);
	sb_free(num_stack);
}

static bool calc_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}
