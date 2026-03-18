#include "agent/subagent/subagent_manager.h"
#include "agent/subagent/subagent_task.h"
#include "agent/subagent/subagent_config.h"
#include "log.h"
#include "os/os.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "subagent_mgr";

#define MAX_SUBAGENTS 16

static mimi_mutex_t *s_mu = NULL;
static subagent_task_t *s_tasks[MAX_SUBAGENTS] = {0};
static subagent_record_t s_recs[MAX_SUBAGENTS];
static int s_count = 0;
static uint32_t s_id_counter = 0;
static bool s_inited = false;

static bool session_allowed(const mimi_session_ctx_t *caller, const subagent_record_t *rec)
{
    if (!caller || !rec) return false;
    if (!caller->requester_session_key[0] || !rec->requester_session_key[0]) return false;
    return strcmp(caller->requester_session_key, rec->requester_session_key) == 0;
}

static int find_idx_by_id(const char *id)
{
    if (!id || !id[0]) return -1;
    for (int i = 0; i < MAX_SUBAGENTS; i++) {
        if (s_tasks[i] && s_recs[i].id[0] && strcmp(s_recs[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static void on_task_finish(subagent_task_t *task, void *user_data)
{
    (void)user_data;
    if (!task || !s_mu) return;

    mimi_mutex_lock(s_mu);
    for (int i = 0; i < MAX_SUBAGENTS; i++) {
        if (s_tasks[i] == task) {
            subagent_task_snapshot(task, &s_recs[i]);
            break;
        }
    }
    mimi_mutex_unlock(s_mu);
}

const char *subagent_state_name(subagent_state_t s)
{
    switch (s) {
        case SUBAGENT_STATE_PENDING: return "pending";
        case SUBAGENT_STATE_RUNNING: return "running";
        case SUBAGENT_STATE_FINISHED: return "finished";
        default: return "unknown";
    }
}

const char *subagent_reason_name(subagent_terminal_reason_t r)
{
    switch (r) {
        case SUBAGENT_REASON_NONE: return "none";
        case SUBAGENT_REASON_COMPLETED: return "completed";
        case SUBAGENT_REASON_FAILED: return "failed";
        case SUBAGENT_REASON_CANCELLED: return "cancelled";
        case SUBAGENT_REASON_TIMED_OUT: return "timed_out";
        case SUBAGENT_REASON_KILLED: return "killed";
        case SUBAGENT_REASON_CRASHED: return "crashed";
        case SUBAGENT_REASON_RESOURCE_EXHAUSTED: return "resource_exhausted";
        default: return "unknown";
    }
}

mimi_err_t subagent_manager_init(void)
{
    if (s_inited) return MIMI_OK;
    mimi_err_t err = mimi_mutex_create(&s_mu);
    if (err != MIMI_OK) return err;
    memset(s_tasks, 0, sizeof(s_tasks));
    memset(s_recs, 0, sizeof(s_recs));
    s_count = 0;
    s_id_counter = 0;
    s_inited = true;
    return MIMI_OK;
}

void subagent_manager_deinit(void)
{
    if (!s_inited) return;
    mimi_mutex_lock(s_mu);
    for (int i = 0; i < MAX_SUBAGENTS; i++) {
        if (s_tasks[i]) {
            subagent_task_cancel(s_tasks[i], SUBAGENT_CANCEL_KILL);
            subagent_task_destroy(s_tasks[i]);
            s_tasks[i] = NULL;
        }
        memset(&s_recs[i], 0, sizeof(s_recs[i]));
    }
    s_count = 0;
    mimi_mutex_unlock(s_mu);
    mimi_mutex_destroy(s_mu);
    s_mu = NULL;
    s_inited = false;
}

mimi_err_t subagent_spawn(const subagent_spawn_spec_t *spec,
                          char *out_id, size_t out_id_size,
                          const mimi_session_ctx_t *parent_ctx)
{
    if (!s_inited) return MIMI_ERR_INVALID_STATE;
    if (!spec || !spec->profile[0] || !spec->task[0] || !out_id || out_id_size == 0 || !parent_ctx) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (parent_ctx->caller_is_subagent) {
        return MIMI_ERR_PERMISSION_DENIED;
    }

    const subagent_profile_runtime_t *prof = subagent_profile_get(spec->profile);
    if (!prof) {
        return MIMI_ERR_NOT_FOUND;
    }

    mimi_mutex_lock(s_mu);
    if (s_count >= MAX_SUBAGENTS) {
        mimi_mutex_unlock(s_mu);
        return MIMI_ERR_NO_MEM;
    }

    int slot = -1;
    for (int i = 0; i < MAX_SUBAGENTS; i++) {
        if (!s_tasks[i]) { slot = i; break; }
    }
    if (slot < 0) {
        mimi_mutex_unlock(s_mu);
        return MIMI_ERR_NO_MEM;
    }

    char id[64];
    uint64_t ms = mimi_time_ms();
    s_id_counter++;
    snprintf(id, sizeof(id), "sa_%llu_%u", (unsigned long long)ms, (unsigned)s_id_counter);

    subagent_task_t *t = subagent_task_create(id, spec, prof, parent_ctx, on_task_finish, NULL);
    if (!t) {
        mimi_mutex_unlock(s_mu);
        return MIMI_ERR_NO_MEM;
    }
    s_tasks[slot] = t;
    subagent_task_snapshot(t, &s_recs[slot]);
    s_count++;
    mimi_mutex_unlock(s_mu);

    mimi_err_t err = subagent_task_start(t);
    if (err != MIMI_OK) {
        mimi_mutex_lock(s_mu);
        s_tasks[slot] = NULL;
        memset(&s_recs[slot], 0, sizeof(s_recs[slot]));
        s_count--;
        mimi_mutex_unlock(s_mu);
        subagent_task_destroy(t);
        return err;
    }

    strncpy(out_id, id, out_id_size - 1);
    out_id[out_id_size - 1] = '\0';
    return MIMI_OK;
}

mimi_err_t subagent_join(const char *id, int wait_ms,
                         subagent_join_result_t *out,
                         const mimi_session_ctx_t *caller_ctx)
{
    if (!s_inited) return MIMI_ERR_INVALID_STATE;
    if (!id || !id[0] || !out || !caller_ctx) return MIMI_ERR_INVALID_ARG;

    mimi_mutex_lock(s_mu);
    int idx = find_idx_by_id(id);
    if (idx < 0) {
        mimi_mutex_unlock(s_mu);
        return MIMI_ERR_NOT_FOUND;
    }
    if (!session_allowed(caller_ctx, &s_recs[idx])) {
        mimi_mutex_unlock(s_mu);
        return MIMI_ERR_PERMISSION_DENIED;
    }
    subagent_task_t *t = s_tasks[idx];
    mimi_mutex_unlock(s_mu);

    (void)subagent_task_wait(t, wait_ms);
    subagent_task_join_result(t, out);
    return MIMI_OK;
}

mimi_err_t subagent_cancel(const char *id, subagent_cancel_mode_t mode,
                           int *out_count,
                           const mimi_session_ctx_t *caller_ctx)
{
    if (!s_inited) return MIMI_ERR_INVALID_STATE;
    if (!id || !id[0] || !caller_ctx) return MIMI_ERR_INVALID_ARG;
    int count = 0;

    mimi_mutex_lock(s_mu);
    if (strcmp(id, "all") == 0 || strcmp(id, "*") == 0) {
        for (int i = 0; i < MAX_SUBAGENTS; i++) {
            if (!s_tasks[i]) continue;
            if (!session_allowed(caller_ctx, &s_recs[i])) continue;
            subagent_task_cancel(s_tasks[i], mode);
            count++;
        }
        mimi_mutex_unlock(s_mu);
        if (out_count) *out_count = count;
        return MIMI_OK;
    }

    int idx = find_idx_by_id(id);
    if (idx < 0) {
        mimi_mutex_unlock(s_mu);
        return MIMI_ERR_NOT_FOUND;
    }
    if (!session_allowed(caller_ctx, &s_recs[idx])) {
        mimi_mutex_unlock(s_mu);
        return MIMI_ERR_PERMISSION_DENIED;
    }
    subagent_task_cancel(s_tasks[idx], mode);
    count = 1;
    mimi_mutex_unlock(s_mu);

    if (out_count) *out_count = count;
    return MIMI_OK;
}

mimi_err_t subagent_steer(const char *id, const char *message,
                          int *out_queue_depth,
                          const mimi_session_ctx_t *caller_ctx)
{
    if (!s_inited) return MIMI_ERR_INVALID_STATE;
    if (!id || !id[0] || !message || !message[0] || !caller_ctx) return MIMI_ERR_INVALID_ARG;

    mimi_mutex_lock(s_mu);
    int idx = find_idx_by_id(id);
    if (idx < 0) {
        mimi_mutex_unlock(s_mu);
        return MIMI_ERR_NOT_FOUND;
    }
    if (!session_allowed(caller_ctx, &s_recs[idx])) {
        mimi_mutex_unlock(s_mu);
        return MIMI_ERR_PERMISSION_DENIED;
    }
    subagent_task_t *t = s_tasks[idx];
    mimi_mutex_unlock(s_mu);

    return subagent_task_steer(t, message, out_queue_depth);
}

mimi_err_t subagent_list(int recent_minutes,
                         char *out_json, size_t out_json_size,
                         const mimi_session_ctx_t *caller_ctx)
{
    if (!s_inited) return MIMI_ERR_INVALID_STATE;
    if (!out_json || out_json_size == 0 || !caller_ctx) return MIMI_ERR_INVALID_ARG;
    out_json[0] = '\0';

    uint64_t now = mimi_time_ms();
    uint64_t recent_ms = 0;
    if (recent_minutes > 0) {
        recent_ms = (uint64_t)recent_minutes * 60ULL * 1000ULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "requesterSessionKey", caller_ctx->requester_session_key);
    cJSON *arr = cJSON_AddArrayToObject(root, "items");

    mimi_mutex_lock(s_mu);
    for (int i = 0; i < MAX_SUBAGENTS; i++) {
        if (!s_tasks[i]) continue;
        subagent_task_snapshot(s_tasks[i], &s_recs[i]);
        if (!session_allowed(caller_ctx, &s_recs[i])) continue;
        if (recent_ms > 0 && (now - s_recs[i].updated_at_ms) > recent_ms) continue;

        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "id", s_recs[i].id);
        cJSON_AddStringToObject(it, "state", subagent_state_name(s_recs[i].state));
        cJSON_AddStringToObject(it, "reason", subagent_reason_name(s_recs[i].reason));
        cJSON_AddNumberToObject(it, "createdAtMs", (double)s_recs[i].created_at_ms);
        cJSON_AddNumberToObject(it, "updatedAtMs", (double)s_recs[i].updated_at_ms);
        cJSON_AddNumberToObject(it, "itersUsed", s_recs[i].iters_used);
        cJSON_AddNumberToObject(it, "toolCalls", s_recs[i].tool_calls);
        cJSON_AddBoolToObject(it, "truncated", s_recs[i].truncated);
        if (s_recs[i].last_error[0]) cJSON_AddStringToObject(it, "lastError", s_recs[i].last_error);
        if (s_recs[i].last_excerpt[0]) cJSON_AddStringToObject(it, "lastExcerpt", s_recs[i].last_excerpt);
        cJSON_AddItemToArray(arr, it);
    }
    mimi_mutex_unlock(s_mu);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        snprintf(out_json, out_json_size, "{\"status\":\"error\",\"error\":\"oom\"}");
        return MIMI_ERR_NO_MEM;
    }
    strncpy(out_json, json_str, out_json_size - 1);
    out_json[out_json_size - 1] = '\0';
    free(json_str);
    return MIMI_OK;
}

