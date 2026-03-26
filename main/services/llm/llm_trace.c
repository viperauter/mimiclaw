#include "llm/llm_trace.h"
#include "config.h"
#include "config_view.h"
#include "cJSON.h"
#include "core/platform/fs/fs.h"
#include "core/platform/path_utils.h"
#include "os/os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ── Trace routing (best-effort) ─────────────────────────────── */

#define TRACE_BIND_MAX 32

typedef struct {
    char trace_id[64];
    char channel[32];
    char chat_id[128];
} trace_bind_t;

static trace_bind_t s_binds[TRACE_BIND_MAX];
static uint32_t s_bind_cursor = 0;

static void sanitize_component(const char *in, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!in) return;

    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        int ok = ((c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.');
        out[j++] = ok ? (char)c : '_';
    }
    out[j] = '\0';
}

void llm_trace_bind_session(const char *trace_id, const char *channel, const char *chat_id)
{
    if (!trace_id || !trace_id[0]) return;

    uint32_t idx = (s_bind_cursor++) % TRACE_BIND_MAX;
    trace_bind_t *b = &s_binds[idx];
    memset(b, 0, sizeof(*b));
    strncpy(b->trace_id, trace_id, sizeof(b->trace_id) - 1);
    sanitize_component(channel ? channel : "", b->channel, sizeof(b->channel));
    sanitize_component(chat_id ? chat_id : "", b->chat_id, sizeof(b->chat_id));
}

static const trace_bind_t *find_bind(const char *trace_id)
{
    if (!trace_id || !trace_id[0]) return NULL;
    for (int i = 0; i < TRACE_BIND_MAX; i++) {
        if (s_binds[i].trace_id[0] && strcmp(s_binds[i].trace_id, trace_id) == 0) {
            return &s_binds[i];
        }
    }
    return NULL;
}

static size_t clamp_len(size_t n, size_t max_n)
{
    if (max_n == 0) return n;
    return (n > max_n) ? max_n : n;
}

static char *dup_truncated(const char *s, size_t max_bytes)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    size_t keep = clamp_len(n, max_bytes);
    char *out = (char *)calloc(1, keep + 1);
    if (!out) return NULL;
    memcpy(out, s, keep);
    out[keep] = '\0';
    return out;
}

static void add_ts_fields(cJSON *obj, uint64_t ts_ms)
{
    if (!obj) return;
    cJSON_AddNumberToObject(obj, "ts_ms", (double)ts_ms);

    /* Human-friendly time for quick scanning (keeps ts_ms as source of truth). */
    char buf[32];
    buf[0] = '\0';
    time_t sec = (time_t)(ts_ms / 1000ULL);
    unsigned ms = (unsigned)(ts_ms % 1000ULL);

#ifdef _WIN32
    struct tm tm_info;
    if (localtime_s(&tm_info, &sec) == 0) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03u",
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, ms);
    }
#else
    struct tm tm_info;
    if (localtime_r(&sec, &tm_info) != NULL) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03u",
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, ms);
    }
#endif

    if (buf[0]) {
        cJSON_AddStringToObject(obj, "ts", buf);
    }
}

static mimi_err_t ensure_trace_dir(char *dir_out, size_t dir_out_size)
{
    mimi_cfg_obj_t defaults = mimi_cfg_get_obj(mimi_cfg_section("agents"), "defaults");
    const char *workspace = mimi_cfg_get_str(defaults, "workspace", "./");
    const char *dir_cfg = "logs"; /* Fixed to workspace/logs (user cannot override). */

    if (mimi_path_join(workspace, dir_cfg, dir_out, dir_out_size) != 0) {
        return MIMI_ERR_NO_MEM;
    }

    /* Create directory (direct POSIX, supports absolute path). */
    if (mimi_fs_mkdir_p_direct(dir_out) != 0) {
        /* Best-effort: if directory exists, ignore. */
        struct stat st;
        if (stat(dir_out, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return MIMI_ERR_IO;
        }
    }
    return MIMI_OK;
}

static mimi_err_t trace_file_path(char *path_out, size_t path_out_size, const char *trace_id)
{
    char dir[512];
    mimi_err_t err = ensure_trace_dir(dir, sizeof(dir));
    if (err != MIMI_OK) return err;

    mimi_cfg_obj_t tracing = mimi_cfg_section("tracing");
    const char *mode = mimi_cfg_get_str(tracing, "mode", "perSession"); /* "single" | "perSession" */

    if (mode && strcmp(mode, "perSession") == 0) {
        const trace_bind_t *b = find_bind(trace_id);
        if (b && b->channel[0] && b->chat_id[0]) {
            char subdir1[1024];
            char subdir2[1024];

            if (mimi_path_join(dir, b->channel, subdir1, sizeof(subdir1)) != 0) {
                return MIMI_ERR_NO_MEM;
            }
            if (mimi_path_join(subdir1, b->chat_id, subdir2, sizeof(subdir2)) != 0) {
                return MIMI_ERR_NO_MEM;
            }

            /* Create nested directory (direct POSIX, supports absolute path). */
            if (mimi_fs_mkdir_p_direct(subdir2) != 0) {
                struct stat st;
                if (stat(subdir2, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    return MIMI_ERR_IO;
                }
            }

            if (mimi_path_join(subdir2, "chat_trace.jsonl", path_out, path_out_size) != 0) {
                return MIMI_ERR_NO_MEM;
            }
            return MIMI_OK;
        }
        /* No binding yet: fall back to single file. */
    }

    /* Single rolling file. trace_id disambiguates sessions in JSONL. */
    if (mimi_path_join(dir, "chat_trace.jsonl", path_out, path_out_size) != 0) {
        return MIMI_ERR_NO_MEM;
    }
    return MIMI_OK;
}

static long file_size_bytes(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long)st.st_size;
}

static mimi_err_t rotate_if_needed(const char *path, int max_bytes)
{
    if (!path || max_bytes <= 0) return MIMI_OK;
    long sz = file_size_bytes(path);
    if (sz <= 0 || sz < (long)max_bytes) return MIMI_OK;

    char path1[1024];
    char path2[1024];
    snprintf(path1, sizeof(path1), "%s.1", path);
    snprintf(path2, sizeof(path2), "%s.2", path);

    /* Keep 2 history files: .2 (older), .1 (newer). */
    (void)remove(path2);
    (void)rename(path1, path2);
    if (rename(path, path1) != 0) {
        return MIMI_ERR_IO;
    }
    return MIMI_OK;
}

static mimi_err_t append_line(const char *path, const char *line)
{
    if (!path || !line) return MIMI_ERR_INVALID_ARG;
    FILE *fp = fopen(path, "ab");
    if (!fp) return MIMI_ERR_IO;
    size_t n = strlen(line);
    size_t w = fwrite(line, 1, n, fp);
    (void)fwrite("\n", 1, 1, fp);
    fclose(fp);
    return (w == n) ? MIMI_OK : MIMI_ERR_IO;
}

static mimi_err_t write_event_obj(cJSON *obj)
{
    if (!obj) return MIMI_ERR_INVALID_ARG;

    mimi_cfg_obj_t tracing = mimi_cfg_section("tracing");
    if (!mimi_cfg_get_bool(tracing, "enabled", false)) {
        return MIMI_OK;
    }

    const char *trace_id = NULL;
    cJSON *tid = cJSON_GetObjectItem(obj, "trace_id");
    if (tid && cJSON_IsString(tid)) {
        trace_id = tid->valuestring;
    }

    char path[1024];
    mimi_err_t err = trace_file_path(path, sizeof(path), trace_id);
    if (err != MIMI_OK) return err;

    (void)rotate_if_needed(path, mimi_cfg_get_int(tracing, "maxFileBytes", 0));

    char *line = cJSON_PrintUnformatted(obj);
    if (!line) return MIMI_ERR_NO_MEM;
    err = append_line(path, line);
    free(line);
    return err;
}

void llm_trace_make_id(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    static uint32_t s_counter = 0;
    uint64_t ms = mimi_time_ms();
    uint32_t c = ++s_counter;
    snprintf(out, out_size, "%llu-%u", (unsigned long long)ms, (unsigned)c);
    out[out_size - 1] = '\0';
}

mimi_err_t llm_trace_event_kv(const char *trace_id,
                              const char *event,
                              const char *k1, const char *v1,
                              const char *k2, const char *v2,
                              const char *k3, const char *v3,
                              const char *k4, const char *v4)
{
    mimi_cfg_obj_t tracing = mimi_cfg_section("tracing");
    if (!mimi_cfg_get_bool(tracing, "enabled", false)) return MIMI_OK;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return MIMI_ERR_NO_MEM;

    add_ts_fields(obj, mimi_time_ms());
    cJSON_AddStringToObject(obj, "trace_id", trace_id ? trace_id : "");
    cJSON_AddStringToObject(obj, "event", event ? event : "");

    int max_field = mimi_cfg_get_int(tracing, "maxFieldBytes", 0);
    if (k1 && v1) { char *t = dup_truncated(v1, (size_t)max_field); cJSON_AddStringToObject(obj, k1, t ? t : ""); free(t); }
    if (k2 && v2) { char *t = dup_truncated(v2, (size_t)max_field); cJSON_AddStringToObject(obj, k2, t ? t : ""); free(t); }
    if (k3 && v3) { char *t = dup_truncated(v3, (size_t)max_field); cJSON_AddStringToObject(obj, k3, t ? t : ""); free(t); }
    if (k4 && v4) { char *t = dup_truncated(v4, (size_t)max_field); cJSON_AddStringToObject(obj, k4, t ? t : ""); free(t); }

    mimi_err_t err = write_event_obj(obj);
    cJSON_Delete(obj);
    return err;
}

mimi_err_t llm_trace_event_json(const char *trace_id,
                                const char *event,
                                const char *json_str)
{
    mimi_cfg_obj_t tracing = mimi_cfg_section("tracing");
    if (!mimi_cfg_get_bool(tracing, "enabled", false)) return MIMI_OK;

    cJSON *obj = cJSON_CreateObject();
    if (!obj) return MIMI_ERR_NO_MEM;

    add_ts_fields(obj, mimi_time_ms());
    cJSON_AddStringToObject(obj, "trace_id", trace_id ? trace_id : "");
    cJSON_AddStringToObject(obj, "event", event ? event : "");

    int max_field = mimi_cfg_get_int(tracing, "maxFieldBytes", 0);
    char *t = dup_truncated(json_str ? json_str : "", (size_t)max_field);
    cJSON_AddStringToObject(obj, "json", t ? t : "");
    free(t);

    mimi_err_t err = write_event_obj(obj);
    cJSON_Delete(obj);
    return err;
}

