#include "platform/kv.h"
#include "platform/log.h"
#include "platform/os.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static const char *TAG = "kv";

static mimi_mutex_t *s_mu;
static cJSON *s_root;
static char s_path[256];

static cJSON *get_ns_obj(const char *ns, bool create)
{
    if (!s_root) s_root = cJSON_CreateObject();
    if (!s_root) return NULL;

    cJSON *obj = cJSON_GetObjectItem(s_root, ns);
    if (!obj && create) {
        obj = cJSON_CreateObject();
        if (!obj) return NULL;
        cJSON_AddItemToObject(s_root, ns, obj);
    }
    return obj;
}

static mimi_err_t load_file(void)
{
    FILE *f = fopen(s_path, "rb");
    if (!f) {
        if (errno == ENOENT) {
            s_root = cJSON_CreateObject();
            return s_root ? MIMI_OK : MIMI_ERR_NO_MEM;
        }
        return MIMI_ERR_IO;
    }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (10 * 1024 * 1024)) {  // sanity
        fclose(f);
        return MIMI_ERR_IO;
    }

    char *buf = (char *)calloc(1, (size_t)n + 1);
    if (!buf) {
        fclose(f);
        return MIMI_ERR_NO_MEM;
    }

    size_t r = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[r] = '\0';

    cJSON *parsed = cJSON_Parse(buf);
    free(buf);
    if (!parsed || !cJSON_IsObject(parsed)) {
        cJSON_Delete(parsed);
        s_root = cJSON_CreateObject();
        return s_root ? MIMI_OK : MIMI_ERR_NO_MEM;
    }

    s_root = parsed;
    return MIMI_OK;
}

static mimi_err_t save_file_locked(void)
{
    if (!s_root) s_root = cJSON_CreateObject();
    if (!s_root) return MIMI_ERR_NO_MEM;

    char *json = cJSON_PrintUnformatted(s_root);
    if (!json) return MIMI_ERR_NO_MEM;

    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_path);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(json);
        return MIMI_ERR_IO;
    }
    size_t len = strlen(json);
    if (fwrite(json, 1, len, f) != len) {
        fclose(f);
        free(json);
        return MIMI_ERR_IO;
    }
    fclose(f);
    free(json);

    if (rename(tmp, s_path) != 0) {
        return MIMI_ERR_IO;
    }
    return MIMI_OK;
}

mimi_err_t mimi_kv_init(const char *persist_path)
{
    if (!persist_path || persist_path[0] == '\0') return MIMI_ERR_INVALID_ARG;
    snprintf(s_path, sizeof(s_path), "%s", persist_path);

    if (!s_mu) {
        mimi_err_t e = mimi_mutex_create(&s_mu);
        if (e != MIMI_OK) return e;
    }

    mimi_mutex_lock(s_mu);
    cJSON_Delete(s_root);
    s_root = NULL;
    mimi_err_t e = load_file();
    mimi_mutex_unlock(s_mu);

    if (e == MIMI_OK) MIMI_LOGI(TAG, "KV initialized: %s", s_path);
    return e;
}

mimi_err_t mimi_kv_get_str(const char *ns, const char *key, char *out, size_t out_len, bool *found)
{
    if (found) *found = false;
    if (!ns || !key || !out || out_len == 0) return MIMI_ERR_INVALID_ARG;

    mimi_mutex_lock(s_mu);
    cJSON *obj = get_ns_obj(ns, false);
    cJSON *item = obj ? cJSON_GetObjectItem(obj, key) : NULL;
    if (item && cJSON_IsString(item) && item->valuestring) {
        snprintf(out, out_len, "%s", item->valuestring);
        if (found) *found = true;
        mimi_mutex_unlock(s_mu);
        return MIMI_OK;
    }
    mimi_mutex_unlock(s_mu);
    out[0] = '\0';
    return MIMI_OK;
}

mimi_err_t mimi_kv_set_str(const char *ns, const char *key, const char *value)
{
    if (!ns || !key || !value) return MIMI_ERR_INVALID_ARG;

    mimi_mutex_lock(s_mu);
    cJSON *obj = get_ns_obj(ns, true);
    if (!obj) {
        mimi_mutex_unlock(s_mu);
        return MIMI_ERR_NO_MEM;
    }
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, value);
    mimi_err_t e = save_file_locked();
    mimi_mutex_unlock(s_mu);
    return e;
}

mimi_err_t mimi_kv_get_u32(const char *ns, const char *key, uint32_t *out, bool *found)
{
    if (found) *found = false;
    if (!ns || !key || !out) return MIMI_ERR_INVALID_ARG;

    mimi_mutex_lock(s_mu);
    cJSON *obj = get_ns_obj(ns, false);
    cJSON *item = obj ? cJSON_GetObjectItem(obj, key) : NULL;
    if (item && cJSON_IsNumber(item)) {
        double d = item->valuedouble;
        if (d >= 0 && d <= 4294967295.0) {
            *out = (uint32_t)d;
            if (found) *found = true;
        }
    }
    mimi_mutex_unlock(s_mu);
    return MIMI_OK;
}

mimi_err_t mimi_kv_set_u32(const char *ns, const char *key, uint32_t value)
{
    if (!ns || !key) return MIMI_ERR_INVALID_ARG;

    mimi_mutex_lock(s_mu);
    cJSON *obj = get_ns_obj(ns, true);
    if (!obj) {
        mimi_mutex_unlock(s_mu);
        return MIMI_ERR_NO_MEM;
    }
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddNumberToObject(obj, key, (double)value);
    mimi_err_t e = save_file_locked();
    mimi_mutex_unlock(s_mu);
    return e;
}

