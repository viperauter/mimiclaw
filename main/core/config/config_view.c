#include "config_view.h"

#include "config.h"
#include "cJSON.h"

static mimi_cfg_obj_t wrap(const cJSON *n)
{
    mimi_cfg_obj_t out;
    out.node = (const void *)n;
    return out;
}

static const cJSON *root_json(void)
{
    /* Internal raw config root getter (not exposed in config.h). */
    extern const cJSON *mimi_config_json_root_internal(void);
    return mimi_config_json_root_internal();
}

mimi_cfg_obj_t mimi_cfg_root(void)
{
    const cJSON *r = root_json();
    if (!r || !cJSON_IsObject(r)) return wrap(NULL);
    return wrap(r);
}

mimi_cfg_obj_t mimi_cfg_section(const char *section)
{
    const cJSON *r = root_json();
    if (!r || !cJSON_IsObject(r) || !section || !section[0]) return wrap(NULL);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)r, section);
    if (!v || !cJSON_IsObject(v)) return wrap(NULL);
    return wrap(v);
}

mimi_cfg_obj_t mimi_cfg_named(const char *section, const char *name)
{
    if (!name || !name[0]) return wrap(NULL);
    mimi_cfg_obj_t sec = mimi_cfg_section(section);
    const cJSON *sec_node = (const cJSON *)sec.node;
    if (!sec_node) return wrap(NULL);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)sec_node, name);
    if (!v || !cJSON_IsObject(v)) return wrap(NULL);
    return wrap(v);
}

mimi_cfg_obj_t mimi_cfg_get_obj(mimi_cfg_obj_t parent, const char *key)
{
    const cJSON *p = (const cJSON *)parent.node;
    if (!p || !cJSON_IsObject(p) || !key || !key[0]) return wrap(NULL);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)p, key);
    if (!v || !cJSON_IsObject(v)) return wrap(NULL);
    return wrap(v);
}

mimi_cfg_obj_t mimi_cfg_get_arr(mimi_cfg_obj_t parent, const char *key)
{
    const cJSON *p = (const cJSON *)parent.node;
    if (!p || !cJSON_IsObject(p) || !key || !key[0]) return wrap(NULL);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)p, key);
    if (!v || !cJSON_IsArray(v)) return wrap(NULL);
    return wrap(v);
}

int mimi_cfg_arr_size(mimi_cfg_obj_t arr)
{
    const cJSON *a = (const cJSON *)arr.node;
    if (!a || !cJSON_IsArray(a)) return 0;
    return cJSON_GetArraySize((cJSON *)a);
}

mimi_cfg_obj_t mimi_cfg_arr_get(mimi_cfg_obj_t arr, int idx)
{
    const cJSON *a = (const cJSON *)arr.node;
    if (!a || !cJSON_IsArray(a) || idx < 0) return wrap(NULL);
    const cJSON *v = cJSON_GetArrayItem((cJSON *)a, idx);
    return wrap(v);
}

const char *mimi_cfg_get_str(mimi_cfg_obj_t obj, const char *key, const char *fallback)
{
    const cJSON *o = (const cJSON *)obj.node;
    if (!o || !cJSON_IsObject(o) || !key || !key[0]) return fallback;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)o, key);
    const char *s = cJSON_GetStringValue((cJSON *)v);
    return s ? s : fallback;
}

bool mimi_cfg_get_bool(mimi_cfg_obj_t obj, const char *key, bool fallback)
{
    const cJSON *o = (const cJSON *)obj.node;
    if (!o || !cJSON_IsObject(o) || !key || !key[0]) return fallback;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)o, key);
    if (v && (cJSON_IsBool(v) || cJSON_IsNumber(v))) return cJSON_IsTrue((cJSON *)v) || (cJSON_IsNumber(v) && v->valueint != 0);
    return fallback;
}

int mimi_cfg_get_int(mimi_cfg_obj_t obj, const char *key, int fallback)
{
    const cJSON *o = (const cJSON *)obj.node;
    if (!o || !cJSON_IsObject(o) || !key || !key[0]) return fallback;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)o, key);
    if (v && cJSON_IsNumber(v)) return (int)v->valuedouble;
    return fallback;
}

double mimi_cfg_get_double(mimi_cfg_obj_t obj, const char *key, double fallback)
{
    const cJSON *o = (const cJSON *)obj.node;
    if (!o || !cJSON_IsObject(o) || !key || !key[0]) return fallback;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)o, key);
    if (v && cJSON_IsNumber(v)) return v->valuedouble;
    return fallback;
}

const char *mimi_cfg_as_str(mimi_cfg_obj_t node, const char *fallback)
{
    const cJSON *n = (const cJSON *)node.node;
    if (!n) return fallback;
    const char *s = cJSON_GetStringValue((cJSON *)n);
    return s ? s : fallback;
}

bool mimi_cfg_as_bool(mimi_cfg_obj_t node, bool fallback)
{
    const cJSON *v = (const cJSON *)node.node;
    if (!v) return fallback;
    if (v && (cJSON_IsBool(v) || cJSON_IsNumber(v))) return cJSON_IsTrue((cJSON *)v) || (cJSON_IsNumber(v) && v->valueint != 0);
    return fallback;
}

int mimi_cfg_as_int(mimi_cfg_obj_t node, int fallback)
{
    const cJSON *v = (const cJSON *)node.node;
    if (!v) return fallback;
    if (v && cJSON_IsNumber(v)) return (int)v->valuedouble;
    return fallback;
}

double mimi_cfg_as_double(mimi_cfg_obj_t node, double fallback)
{
    const cJSON *v = (const cJSON *)node.node;
    if (!v) return fallback;
    if (v && cJSON_IsNumber(v)) return v->valuedouble;
    return fallback;
}

bool mimi_cfg_is_object(mimi_cfg_obj_t node)
{
    const cJSON *n = (const cJSON *)node.node;
    return n && cJSON_IsObject(n);
}

bool mimi_cfg_is_array(mimi_cfg_obj_t node)
{
    const cJSON *n = (const cJSON *)node.node;
    return n && cJSON_IsArray(n);
}

void mimi_cfg_each_key(mimi_cfg_obj_t obj, mimi_cfg_each_key_cb cb, void *ctx)
{
    const cJSON *o = (const cJSON *)obj.node;
    if (!o || !cJSON_IsObject(o) || !cb) return;
    for (const cJSON *child = o->child; child; child = child->next) {
        if (!child->string) continue;
        cb(ctx, child->string, wrap(child));
    }
}

