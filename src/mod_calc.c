#include "module.h"
#include "inso_utils.h"
#include "stb_sb.h"

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

	OP_PAREN_OPEN,
	OP_PAREN_CLOSE,
};

static int precedence[] = {
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

enum {
	T_NUM,
	T_OP,
	T_PAREN_OPEN,
	T_PAREN_CLOSE,
	T_BAD   = 0x10,
	T_ERROR = T_BAD,
	T_EOF   = T_BAD | 1,
};

typedef struct {
	long type;
	long data;
} token;

typedef struct {
	token prev;
	long base;
} calc_state;

static const char ops[] = "+-/*%*&|^><()";

static token calc_next_token(const char** str, calc_state* state){
	bool negative = false;
	char* p;

	if(!**str) return (token){ T_EOF };

	while(**((uint8_t**)str) <= ' '){
		++*str;
	}

	if((p = strchr(ops, **str))){
		++*str;
		if(p - ops > 10){
			return (token){ T_PAREN_OPEN + (*p == ')') };
		} else if(*p == '*'){ // exp vs mul
			if(**str == '*'){
				++*str;
				return (token){ T_OP, OP_EXP };
			} else {
				return (token){ T_OP, OP_MUL };
			}
		} else if(*p == '-'){ // sub vs negative number
			if(state->prev.type == T_OP){
				negative = true;
			} else {
				return (token){ T_OP, OP_SUB };
			}
		} else if(*p == '>' || *p == '<'){ // bitshifts
			if(**str != *p){
				return (token){ T_ERROR };
			} else {
				++*str;
				return (token){ T_OP, p - ops };
			}
		} else {
			return (token){ T_OP, p - ops };
		}
	}

	const char* digits = "0123456789";
	long base = 10;
	long num = 0;

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

	if(!strchr(digits, **str)){
		return (token){ T_ERROR };
	}

	while(**str && (p = strchr(digits, **str))){
		num = (num * base) + (p - digits);
		++*str;
	}

	if(negative){
		num *= -1;
	}

	state->base = INSO_MAX(state->base, base);

	return (token){ T_NUM, num };
}

static long lpow(long num, unsigned long exp){
	long result = 1;

	while(exp){
		if(exp & 1){
			result *= num;
		}
		exp >>= 1;
		num *= num;
	}

	return result;
}

static bool calc_apply_op(long op, long** nums){
	if(sb_count(*nums) < 2) return true;

	long b = sb_last(*nums); sb_pop(*nums);
	long a = sb_last(*nums); sb_pop(*nums);

	switch(op){
		case OP_ADD: sb_push(*nums, a + b); break;
		case OP_SUB: sb_push(*nums, a - b); break;
		case OP_DIV: if(!b) return false; else sb_push(*nums, a / b); break;
		case OP_MUL: sb_push(*nums, a * b); break;
		case OP_MOD: if(!b) return false; else sb_push(*nums, a % b); break;
		case OP_EXP: sb_push(*nums, lpow(a, labs(b))); break;
		case OP_AND: sb_push(*nums, a & b); break;
		case OP_OR : sb_push(*nums, a | b); break;
		case OP_XOR: sb_push(*nums, a ^ b); break;
		case OP_SHR: sb_push(*nums, a >> b); break;
		case OP_SHL: {
			unsigned long _a = a, _b = b;
			long r = _b >= sizeof(long)*8 ? 0 : _a << _b;
			sb_push(*nums, r);
		} break;
	}

	return true;
}

static void calc_cmd(const char* chan, const char* name, const char* arg, int cmd){
	if(!*arg++) return;

	long* op_stack = NULL;
	long* num_stack = NULL;

	token t;
	calc_state state = {};

	while((t = calc_next_token(&arg, &state)), !(t.type & T_BAD)){

		if(t.type == T_NUM){
			sb_push(num_stack, t.data);
		} else if(t.type == T_OP){
			while(sb_count(op_stack) && precedence[sb_last(op_stack)] >= precedence[t.data]){
				if(!calc_apply_op(sb_last(op_stack), &num_stack)) goto dbz;
				sb_pop(op_stack);
			}
			sb_push(op_stack, t.data);
		} else if(t.type == T_PAREN_OPEN){
			sb_push(op_stack, OP_PAREN_OPEN);
		} else {
			while(sb_count(op_stack) && sb_last(op_stack) != OP_PAREN_OPEN){
				if(!calc_apply_op(sb_last(op_stack), &num_stack)) goto dbz;
				sb_pop(op_stack);
			}

			if(sb_count(op_stack)){
				sb_pop(op_stack);
			} else {
				ctx->send_msg(chan, "%s: Error, unbalanced parentheses.", name);
				goto out;
			}
		}

		state.prev = t;
	}

	if(t.type == T_ERROR){
		int tlen = mblen(arg, MB_CUR_MAX);
		ctx->send_msg(chan, "%s: Error: unknown token '%.*s'", name, tlen, arg);
		goto out;
	}

	while(sb_count(op_stack)){
		if(sb_last(op_stack) == OP_PAREN_OPEN){
			ctx->send_msg(chan, "%s: Error, unbalanced parentheses.", name);
			goto out;
		}
		if(!calc_apply_op(sb_last(op_stack), &num_stack)) goto dbz;
		sb_pop(op_stack);
	}

	const char* fmt = state.base == 16 ? "%s: %#lx." : "%s: %ld.";
	ctx->send_msg(chan, fmt, name, sb_last(num_stack));

out:
	sb_free(op_stack);
	sb_free(num_stack);
	return;

dbz:
	ctx->send_msg(chan, "\001ACTION bursts into flames after dividing by zero.\001");
}

static bool calc_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}
