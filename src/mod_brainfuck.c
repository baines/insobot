#include "module.h"
#include "config.h"
#include "inso_utils.h"

static bool brainfuck_init (const IRCCoreCtx*);
static void brainfuck_cmd  (const char*, const char*, const char*, int);

enum { BRAINFUCK_EXEC };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "brainfuck",
	.desc     = "Brainfuck interpreter",
	.on_init  = brainfuck_init,
	.on_cmd   = &brainfuck_cmd,
	.commands = DEFINE_CMDS (
		[BRAINFUCK_EXEC] = "!bf"
	)
};

static const IRCCoreCtx* ctx;

static char bf_mem[30000];
static const char* bf_end = bf_mem + sizeof(bf_mem) - 1;

#define MAX_CYCLES 500000

#if 0
	#define BF_DBG(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
	#define BF_DBG(fmt, ...)
#endif

static bool brainfuck_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void brainfuck_cmd(const char* chan, const char* name, const char* arg, int cmd){
	if(cmd != BRAINFUCK_EXEC) return;
	if(!inso_is_wlist(ctx, name)) return;
	if(*arg++ != ' ') return;

	const char* ip    = arg;

	const char* input = strchrnul(arg, ' ');
	const char* in_p  = *input ? input+1 : input;

	char  output[512] = {};
	char* out_p       = output;

	char* p           = bf_mem + sizeof(bf_mem)/2;
	
	memset(bf_mem, 0, sizeof(bf_mem));

	int cycles = 0;
	int nesting = 0;

	while(++cycles < MAX_CYCLES){
		switch(*ip){
			case '>': { BF_DBG(">\n"); if(p < bf_end) ++p; } break;
			case '<': { BF_DBG("<\n"); if(p > bf_mem) --p; } break;

			case '+': { BF_DBG("+ [%d]\n", *p+1); ++*p; } break;
			case '-': { BF_DBG("- [%d]\n", *p-1); --*p; } break;

			case '.': {
				BF_DBG("out [%d]\n", *p);
				if(out_p - output < sizeof(output) - 1){
					*out_p++ = *p;
				}
			} break;

			case ',': {
				BF_DBG("in [%d]\n", *in_p);
				// EOF = no change
				if(*in_p){
					*p = *in_p++;
				}
			} break;

			case '[': {
				BF_DBG("[ jump [%d]\n", *p);
				int n = nesting;
				if(!*p){
					while(ip++ != input){
						if(*ip == '[') ++n;
						if(*ip == ']'){
							if(n == nesting) break;
							else --n;
						}
					}

					if(ip == input) goto invalid;
				} else {
					++nesting;
				}
			} break;

			case ']': {
				BF_DBG("] jump [%d]\n", *p);
				int n = nesting;
				if(n <= 0) goto invalid;

				if(*p){
					while(--ip != arg){
						if(*ip == '['){
							if(n == nesting) break;
							else --n;
						}
						if(*ip == ']') ++n;
					}

					if(ip == arg) goto invalid;
				} else {
					--nesting;
				}
			} break;

			case ' ':
			case 0: {
				goto done;
			} break;

			default: {
				BF_DBG("??? [%d]\n", *ip);
				goto invalid;
			} break;
		}

		++ip;
	}

done:
	if(cycles == MAX_CYCLES){
		ctx->send_msg(chan, "%s: max cycle count (%d) reached.", name, MAX_CYCLES);
	} else {
		ctx->send_msg(chan, "%s: Output: %s", name, output);
	}

	return;

invalid:
	ctx->send_msg(chan, "%s: malformed program.", name);
}
