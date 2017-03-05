#ifndef INSO_JSON_H_
#define INSO_JSON_H_
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>
#include <yajl/yajl_tree.h>
#include <yajl/yajl_gen.h>

// this is just some helper stuff around yajl, not a json parser/generator in itself.

bool yajl_multi_get(yajl_val root, ...) __attribute__((sentinel));

#define YAJL_P(...) (const char*[]){ __VA_ARGS__, NULL }
#define YAJL_GET(root, type, path) yajl_tree_get((root), YAJL_P path, (type));

#endif

#ifdef INSO_IMPL

bool yajl_multi_get(yajl_val root, ...){
	va_list va;
	va_start(va, root);

	const char* id;
	while((id = va_arg(va, char*))){
		int type      = va_arg(va, int);
		yajl_val* dst = va_arg(va, yajl_val*);

		assert(type);
		assert(dst);

		const char* yajl_path[] = { id, NULL };
		
		yajl_val ret = yajl_tree_get(root, yajl_path, type);
		if(!ret) return false;
		*dst = ret;
	}

	return true;
}

#endif
