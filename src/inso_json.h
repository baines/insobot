#ifndef INSO_JSON_H_
#define INSO_JSON_H_
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>
#include <yajl/yajl_tree.h>
#include <yajl/yajl_gen.h>
#include "uj.h"

// TODO: replace use of yajl with uj.h
// we use some of yajl's json-generating code though, which uj.h doesn't have - so figure that one out...

#define UJ_P(...) (const char*[]){ __VA_ARGS__, NULL }
#define UJ_GET(root, path, type) uj_get_path((root), UJ_P path, (type));

struct uj_node* uj_get(struct uj_node* root, const char* key, enum uj_type type);
struct uj_node* uj_get_path(struct uj_node* root, const char** path, enum uj_type type);

///

bool yajl_multi_get(yajl_val root, ...) __attribute__((sentinel));

#define YAJL_P(...) (const char*[]){ __VA_ARGS__, NULL }
#define YAJL_GET(root, type, path) yajl_tree_get((root), YAJL_P path, (type));

#endif

#ifdef INSO_IMPL

bool yajl_multi_get(yajl_val root, ...){
	va_list va;
	va_start(va, root);
	bool result = false;

	const char* id;
	while((id = va_arg(va, char*))){
		int type      = va_arg(va, int);
		yajl_val* dst = va_arg(va, yajl_val*);

		assert(type);
		assert(dst);

		const char* yajl_path[] = { id, NULL };

		yajl_val ret = yajl_tree_get(root, yajl_path, type);
		if(!ret) goto out;
		*dst = ret;
	}

	result = true;

out:
	va_end(va);
	return result;
}

struct uj_node* uj_get(struct uj_node* root, const char* key, enum uj_type type) {
    if(!root || root->type != UJ_OBJ) {
        return NULL;
    }

    for(struct uj_kv* kv = root->obj; kv; kv = kv->next) {
        if(strcmp(key, kv->key) == 0) {
            return (kv->val && kv->val->type == type)
                ? kv->val
                : NULL
                ;
        }
    }

    return NULL;
}

struct uj_node* uj_get_path(struct uj_node* root, const char** path, enum uj_type type) {
    struct uj_node* n = root;

    for(const char** p = path; *p; ++p) {
        const char* comp = *p;

        enum uj_type subtype = UJ_OBJ;
        if(p[1] == NULL) {
            subtype = type;
        }

        n = uj_get(n, comp, subtype);
        if(!n) {
            break;
        }
    }

    return n;
}

#endif
