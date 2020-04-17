#!/bin/bash

MODNAME="$1"

if [[ -z "$MODNAME" ]]; then
	echo "Usage: $0 <module name>"
	exit 1
fi

MODFILE="src/mod_${MODNAME}.c"

if [[ -e "$MODFILE" ]]; then
	echo "Module $MODNAME already exists, exiting."
	exit 1
fi

cat > "$MODFILE" << EOF
#include "module.h" // open me to view the available APIs

static bool ${MODNAME}_init (const IRCCoreCtx*);
static void ${MODNAME}_cmd  (const char* chan, const char* name, const char* arg, int cmd);

enum { ${MODNAME^^}_EXAMPLE_CMD };

const IRCModuleCtx irc_mod_ctx = {
	.name     = "$MODNAME",
	.desc     = "new module", // TODO
	.on_init  = &${MODNAME}_init,
	.on_cmd   = &${MODNAME}_cmd,
	.commands = DEFINE_CMDS (
		[${MODNAME^^}_EXAMPLE_CMD] = "!${MODNAME}"
	),
};

static const IRCCoreCtx* ctx;

static bool ${MODNAME}_init(const IRCCoreCtx* _ctx){
	ctx = _ctx;
	return true;
}

static void ${MODNAME}_cmd(const char* chan, const char* name, const char* arg, int cmd){
	switch(cmd){
		case ${MODNAME^^}_EXAMPLE_CMD: {

		} break;
	}
}
EOF

echo "Created module $MODNAME"
"${EDITOR:-vim}" "$MODFILE"
