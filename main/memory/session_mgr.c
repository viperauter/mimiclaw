#include "session_mgr.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "log.h"
#include "os/os.h"
#include "cJSON.h"
#include "fs/fs.h"

static const char *TAG = "session";

static void session_path(const char *channel, const char *chat_id, char *buf, size_t size)
{
    const mimi_config_t *cfg = mimi_config_get();
    const char *dir = cfg->session_dir[0] ? cfg->session_dir : "sessions";
    const char *chan = (channel && channel[0]) ? channel : "unknown";
    const char *id = (chat_id && chat_id[0]) ? chat_id : "unknown";
    snprintf(buf, size, "%s/%s_%s.jsonl", dir, chan, id);
}

void session_ctx_from_msg(const mimi_msg_t *msg, mimi_session_ctx_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!msg) return;
    strncpy(out->channel, msg->channel, sizeof(out->channel) - 1);
    strncpy(out->chat_id, msg->chat_id, sizeof(out->chat_id) - 1);
    if (msg->channel[0] && msg->chat_id[0]) {
        snprintf(out->workspace_root, sizeof(out->workspace_root),
                 "workspaces/%s_%s", msg->channel, msg->chat_id);
    }
}

mimi_err_t session_mgr_init(void)
{
    const mimi_config_t *cfg = mimi_config_get();
    MIMI_LOGI(TAG, "Session manager initialized at %s", cfg->session_dir);
    return MIMI_OK;
}

mimi_err_t session_append(const char *channel, const char *chat_id, const char *role, const char *content)
{
    char path[256];
    session_path(channel, chat_id, path, sizeof(path));

    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(path, "a", &f);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Cannot open session file %s", path);
        return err;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)(mimi_time_ms() / 1000ULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        size_t written = 0;
        (void)mimi_fs_write(f, line, strlen(line), &written);
        const char nl = '\n';
        (void)mimi_fs_write(f, &nl, 1, &written);
        free(line);
    }

    mimi_fs_close(f);
    return MIMI_OK;
}

mimi_err_t session_get_history_json(const char *channel, const char *chat_id, char *buf, size_t size, int max_msgs)
{
    if (max_msgs <= 0) max_msgs = 100;
    char path[256];
    session_path(channel, chat_id, path, sizeof(path));

    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(path, "r", &f);
    if (err != MIMI_OK) {
        /* No history yet */
        snprintf(buf, size, "[]");
        return MIMI_OK;
    }

    /* Read all lines into a ring buffer of cJSON objects */
    cJSON **messages = (cJSON **)calloc((size_t)max_msgs, sizeof(cJSON *));
    if (!messages) {
        mimi_fs_close(f);
        snprintf(buf, size, "[]");
        return MIMI_ERR_NO_MEM;
    }
    int count = 0;
    int write_idx = 0;

    char line[2048];
    for (;;) {
        bool eof = false;
        err = mimi_fs_read_line(f, line, sizeof(line), &eof);
        if (err != MIMI_OK) break;
        if (eof) break;
        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        /* Ring buffer: overwrite oldest if full */
        if (count >= max_msgs) {
            cJSON_Delete(messages[write_idx]);
        }
        messages[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    mimi_fs_close(f);

    /* Build JSON array with only role + content */
    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = messages[idx];

        cJSON *entry = cJSON_CreateObject();
        cJSON *role = cJSON_GetObjectItem(src, "role");
        cJSON *content = cJSON_GetObjectItem(src, "content");
        if (role && content) {
            cJSON_AddStringToObject(entry, "role", role->valuestring);
            cJSON_AddStringToObject(entry, "content", content->valuestring);
        }
        cJSON_AddItemToArray(arr, entry);
    }

    /* Cleanup ring buffer */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (cleanup_start + i) % max_msgs;
        cJSON_Delete(messages[idx]);
    }
    free(messages);

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[]");
    }

    return MIMI_OK;
}

mimi_err_t session_clear(const char *channel, const char *chat_id)
{
    char path[256];
    session_path(channel, chat_id, path, sizeof(path));

    if (mimi_fs_remove(path) == MIMI_OK) {
        MIMI_LOGI(TAG, "Session %s cleared", chat_id);
        return MIMI_OK;
    }
    return MIMI_ERR_NOT_FOUND;
}

void session_list(void)
{
    const mimi_config_t *cfg = mimi_config_get();
    const char *base_dir = cfg->session_dir[0] ? cfg->session_dir : "sessions";
    mimi_dir_t *dir = NULL;
    mimi_err_t err = mimi_fs_opendir(base_dir, &dir);
    if (err != MIMI_OK) {
        MIMI_LOGW(TAG, "Cannot open sessions directory: %s", base_dir);
        return;
    }

    int count = 0;
    for (;;) {
        bool has = false;
        char name[256];
        err = mimi_fs_readdir(dir, name, sizeof(name), &has);
        if (err != MIMI_OK) break;
        if (!has) break;
        /* List all JSONL session files: "<channel>_<chat_id>.jsonl" */
        const size_t n = strlen(name);
        if (n >= 6 && strcmp(name + n - 6, ".jsonl") == 0) {
            MIMI_LOGI(TAG, "  Session: %s", name);
            count++;
        }
    }
    mimi_fs_closedir(dir);

    if (count == 0) {
        MIMI_LOGI(TAG, "  No sessions found");
    }
}
