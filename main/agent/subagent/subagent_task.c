#include "agent/subagent/subagent_task.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "subagent_task";

#define STEER_QUEUE_MAX 8

struct subagent_task {
    mimi_mutex_t *mu;
    mimi_cond_t *cv;

    subagent_record_t rec;
    subagent_join_result_t join;

    const subagent_profile_runtime_t *profile; /* not owned */

    mimi_session_ctx_t child_ctx;

    /* Execution */
    cJSON *messages;            /* owned */
    llm_response_t llm_resp;    /* embedded storage for async API */
    int max_iters;
    uint64_t deadline_ms;       /* 0 => no deadline */

    /* Tool execution bookkeeping */
    int pending_tools;
    int pending_total;
    bool llm_inflight;

    /* Control */
    bool cancel_soft;
    bool cancel_kill;
    char *steer_q[STEER_QUEUE_MAX];
    int steer_head;
    int steer_tail;
    int steer_count;

    /* Finish notification */
    subagent_task_on_finish_fn on_finish;
    void *on_finish_ud;
};

static bool csv_contains_token(const char *csv, const char *token)
{
    if (!csv || !csv[0] || !token || !token[0]) return false;
    char buf[256];
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    for (char *p = strtok_r(buf, ",", &saveptr); p != NULL; p = strtok_r(NULL, ",", &saveptr)) {
        while (*p == ' ' || *p == '\t') p++;
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) {
            p[--n] = '\0';
        }
        if (n == 0) continue;
        if (strcmp(p, token) == 0) return true;
    }
    return false;
}

static void rec_touch(subagent_task_t *t)
{
    t->rec.updated_at_ms = mimi_time_ms();
}

static void set_finished_locked(subagent_task_t *t,
                                subagent_terminal_reason_t reason,
                                bool ok,
                                const char *content,
                                const char *error,
                                bool truncated)
{
    t->rec.state = SUBAGENT_STATE_FINISHED;
    t->rec.reason = reason;
    rec_touch(t);

    t->join.finished = true;
    t->join.reason = reason;
    t->join.ok = ok;
    t->join.truncated = truncated;

    if (content && content[0]) {
        strncpy(t->join.content, content, sizeof(t->join.content) - 1);
        t->join.content[sizeof(t->join.content) - 1] = '\0';
        strncpy(t->rec.last_excerpt, t->join.content, sizeof(t->rec.last_excerpt) - 1);
        t->rec.last_excerpt[sizeof(t->rec.last_excerpt) - 1] = '\0';
    }
    if (error && error[0]) {
        strncpy(t->join.error, error, sizeof(t->join.error) - 1);
        t->join.error[sizeof(t->join.error) - 1] = '\0';
        strncpy(t->rec.last_error, t->join.error, sizeof(t->rec.last_error) - 1);
        t->rec.last_error[sizeof(t->rec.last_error) - 1] = '\0';
    }
    t->rec.truncated = truncated;
    t->join.truncated = truncated;

    mimi_cond_broadcast(t->cv);
}

static void drain_steer_locked(subagent_task_t *t)
{
    if (!t->messages) return;
    while (t->steer_count > 0) {
        char *msg = t->steer_q[t->steer_head];
        t->steer_q[t->steer_head] = NULL;
        t->steer_head = (t->steer_head + 1) % STEER_QUEUE_MAX;
        t->steer_count--;

        if (msg) {
            cJSON *u = cJSON_CreateObject();
            cJSON_AddStringToObject(u, "role", "user");
            char buf[1024];
            snprintf(buf, sizeof(buf), "## Steer\n%s", msg);
            cJSON_AddStringToObject(u, "content", buf);
            cJSON_AddItemToArray(t->messages, u);
            free(msg);
        }
    }
}

static void subagent_step_llm(subagent_task_t *t);

typedef struct {
    subagent_task_t *t;
    char tool_call_id[80];
} tool_ud_t;

static void tool_cb(mimi_err_t result, const char *tool_name, const char *output, void *user_data)
{
    tool_ud_t *ud = (tool_ud_t *)user_data;
    subagent_task_t *t = ud ? ud->t : NULL;
    if (!t) {
        free(ud);
        return;
    }

    mimi_mutex_lock(t->mu);
    if (t->rec.state == SUBAGENT_STATE_FINISHED) {
        mimi_mutex_unlock(t->mu);
        free(ud);
        return;
    }

    if (t->messages) {
        cJSON *tool_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_msg, "role", "tool");
        cJSON_AddStringToObject(tool_msg, "tool_call_id", ud->tool_call_id);
        cJSON_AddStringToObject(tool_msg, "content", output ? output : "");
        cJSON_AddItemToArray(t->messages, tool_msg);
    }

    t->rec.tool_calls++;
    rec_touch(t);

    if (result != MIMI_OK) {
        snprintf(t->rec.last_error, sizeof(t->rec.last_error),
                 "tool %s failed: %s", tool_name ? tool_name : "tool", mimi_err_to_name(result));
    }

    if (t->pending_tools > 0) t->pending_tools--;
    bool done = (t->pending_tools == 0);

    mimi_mutex_unlock(t->mu);
    free(ud);

    if (done) {
        subagent_step_llm(t);
    }
}

static void llm_cb(mimi_err_t result, llm_response_t *resp, void *user_data)
{
    subagent_task_t *t = (subagent_task_t *)user_data;
    if (!t || !resp) return;

    mimi_mutex_lock(t->mu);
    t->llm_inflight = false;

    if (t->rec.state == SUBAGENT_STATE_FINISHED) {
        mimi_mutex_unlock(t->mu);
        llm_response_free(resp);
        return;
    }

    if (t->cancel_kill) {
        set_finished_locked(t, SUBAGENT_REASON_KILLED, false, "", "killed", false);
        mimi_mutex_unlock(t->mu);
        llm_response_free(resp);
        if (t->on_finish) t->on_finish(t, t->on_finish_ud);
        return;
    }
    if (t->cancel_soft) {
        set_finished_locked(t, SUBAGENT_REASON_CANCELLED, false, "", "cancelled", false);
        mimi_mutex_unlock(t->mu);
        llm_response_free(resp);
        if (t->on_finish) t->on_finish(t, t->on_finish_ud);
        return;
    }

    if (result != MIMI_OK) {
        const char *err = llm_get_last_error();
        char buf[256];
        snprintf(buf, sizeof(buf), "llm error: %s", mimi_err_to_name(result));
        set_finished_locked(t, SUBAGENT_REASON_FAILED, false, "", err && err[0] ? err : buf, false);
        mimi_mutex_unlock(t->mu);
        llm_response_free(resp);
        if (t->on_finish) t->on_finish(t, t->on_finish_ud);
        return;
    }

    if (!resp->tool_use) {
        const char *text = (resp->text && resp->text_len > 0) ? resp->text : "";
        bool trunc = false;
        char excerpt[4096] = {0};
        if (text && text[0]) {
            strncpy(excerpt, text, sizeof(excerpt) - 1);
            excerpt[sizeof(excerpt) - 1] = '\0';
            if (strlen(text) >= sizeof(excerpt) - 1) trunc = true;
        }
        set_finished_locked(t, SUBAGENT_REASON_COMPLETED, true, excerpt, "", trunc);
        mimi_mutex_unlock(t->mu);
        llm_response_free(resp);
        if (t->on_finish) t->on_finish(t, t->on_finish_ud);
        return;
    }

    /* Append assistant message with tool_calls (OpenAI format). */
    if (t->messages) {
        cJSON *asst = cJSON_CreateObject();
        cJSON_AddStringToObject(asst, "role", "assistant");
        cJSON_AddStringToObject(asst, "content", resp->text ? resp->text : "");
        cJSON *tool_calls = cJSON_CreateArray();
        for (int i = 0; i < resp->call_count; i++) {
            const llm_tool_call_t *call = &resp->calls[i];
            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id", call->id[0] ? call->id : "");
            cJSON_AddStringToObject(tc, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", call->name[0] ? call->name : "");
            cJSON_AddStringToObject(fn, "arguments", call->input ? call->input : "{}");
            cJSON_AddItemToObject(tc, "function", fn);
            cJSON_AddItemToArray(tool_calls, tc);
        }
        cJSON_AddItemToObject(asst, "tool_calls", tool_calls);
        cJSON_AddItemToArray(t->messages, asst);
    }

    /* Dispatch tools asynchronously. */
    t->pending_tools = resp->call_count;
    t->pending_total = resp->call_count;
    rec_touch(t);

    const char *allow_csv = t->profile && t->profile->cfg.tools_csv[0] ? t->profile->cfg.tools_csv : NULL;
    /* Spec override lives only in initial spawn; manager already resolved it into profile tools_json.
     * Here we enforce runtime allowlist based on profile cfg.tools_csv. */

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        char tool_out[256];
        tool_out[0] = '\0';

        if (allow_csv && !csv_contains_token(allow_csv, call->name)) {
            /* Reject immediately, but still append a tool message and count down. */
            snprintf(tool_out, sizeof(tool_out), "Error: tool '%s' not allowed", call->name);
            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", call->id[0] ? call->id : "");
            cJSON_AddStringToObject(tool_msg, "content", tool_out);
            cJSON_AddItemToArray(t->messages, tool_msg);
            t->pending_tools--;
            continue;
        }

        tool_ud_t *ud = (tool_ud_t *)calloc(1, sizeof(*ud));
        if (!ud) {
            t->pending_tools--;
            continue;
        }
        ud->t = t;
        strncpy(ud->tool_call_id, call->id[0] ? call->id : "", sizeof(ud->tool_call_id) - 1);

        const char *input = (call->input && call->input[0]) ? call->input : "{}";
        mimi_err_t te = tool_registry_execute_async(call->name, input, &t->child_ctx, tool_cb, ud);
        if (te != MIMI_OK) {
            /* Fallback: append error tool message. */
            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", ud->tool_call_id);
            snprintf(tool_out, sizeof(tool_out), "Error: tool dispatch failed (%s)", mimi_err_to_name(te));
            cJSON_AddStringToObject(tool_msg, "content", tool_out);
            cJSON_AddItemToArray(t->messages, tool_msg);
            t->pending_tools--;
            free(ud);
        }
    }

    t->rec.iters_used++;
    rec_touch(t);

    bool no_pending = (t->pending_tools == 0);
    mimi_mutex_unlock(t->mu);

    llm_response_free(resp);

    if (no_pending) {
        subagent_step_llm(t);
    }
}

static void subagent_step_llm(subagent_task_t *t)
{
    if (!t) return;

    mimi_mutex_lock(t->mu);
    if (t->rec.state == SUBAGENT_STATE_FINISHED) {
        mimi_mutex_unlock(t->mu);
        return;
    }
    if (t->cancel_kill) {
        set_finished_locked(t, SUBAGENT_REASON_KILLED, false, "", "killed", false);
        mimi_mutex_unlock(t->mu);
        if (t->on_finish) t->on_finish(t, t->on_finish_ud);
        return;
    }
    if (t->cancel_soft) {
        set_finished_locked(t, SUBAGENT_REASON_CANCELLED, false, "", "cancelled", false);
        mimi_mutex_unlock(t->mu);
        if (t->on_finish) t->on_finish(t, t->on_finish_ud);
        return;
    }

    if (t->deadline_ms > 0 && mimi_time_ms() > t->deadline_ms) {
        set_finished_locked(t, SUBAGENT_REASON_TIMED_OUT, false, "", "timed out", false);
        mimi_mutex_unlock(t->mu);
        if (t->on_finish) t->on_finish(t, t->on_finish_ud);
        return;
    }

    if (t->rec.iters_used >= t->max_iters) {
        set_finished_locked(t, SUBAGENT_REASON_RESOURCE_EXHAUSTED, false, "", "max iterations reached", false);
        mimi_mutex_unlock(t->mu);
        if (t->on_finish) t->on_finish(t, t->on_finish_ud);
        return;
    }

    drain_steer_locked(t);

    llm_chat_req_t req = {
        .system_prompt = (t->profile && t->profile->system_prompt[0]) ? t->profile->system_prompt : "",
        .messages = t->messages,
        .tools_json = (t->profile && t->profile->tools_json) ? t->profile->tools_json : tool_registry_get_tools_json(),
        .trace_id = NULL,
    };
    memset(&t->llm_resp, 0, sizeof(t->llm_resp));
    t->llm_inflight = true;
    rec_touch(t);
    mimi_mutex_unlock(t->mu);

    mimi_err_t err = llm_chat_tools_async_req(&req, &t->llm_resp, llm_cb, t);
    if (err != MIMI_OK) {
        mimi_mutex_lock(t->mu);
        t->llm_inflight = false;
        set_finished_locked(t, SUBAGENT_REASON_FAILED, false, "", "failed to dispatch llm call", false);
        mimi_mutex_unlock(t->mu);
        if (t->on_finish) t->on_finish(t, t->on_finish_ud);
    }
}

subagent_task_t *subagent_task_create(const char *id,
                                      const subagent_spawn_spec_t *spec,
                                      const subagent_profile_runtime_t *profile,
                                      const mimi_session_ctx_t *parent_ctx,
                                      subagent_task_on_finish_fn on_finish,
                                      void *user_data)
{
    if (!id || !spec || !profile || !parent_ctx) return NULL;
    subagent_task_t *t = (subagent_task_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;

    mimi_err_t err = mimi_mutex_create(&t->mu);
    if (err != MIMI_OK) {
        free(t);
        return NULL;
    }
    err = mimi_cond_create(&t->cv);
    if (err != MIMI_OK) {
        mimi_mutex_destroy(t->mu);
        free(t);
        return NULL;
    }

    t->profile = profile;
    t->on_finish = on_finish;
    t->on_finish_ud = user_data;

    memset(&t->rec, 0, sizeof(t->rec));
    strncpy(t->rec.id, id, sizeof(t->rec.id) - 1);
    strncpy(t->rec.requester_session_key, parent_ctx->requester_session_key, sizeof(t->rec.requester_session_key) - 1);
    t->rec.state = SUBAGENT_STATE_PENDING;
    t->rec.reason = SUBAGENT_REASON_NONE;
    t->rec.created_at_ms = mimi_time_ms();
    t->rec.updated_at_ms = t->rec.created_at_ms;

    memset(&t->join, 0, sizeof(t->join));

    /* Build child session ctx inheriting requester key but marking subagent. */
    t->child_ctx = *parent_ctx;
    strncpy(t->child_ctx.caller_session_key, parent_ctx->requester_session_key, sizeof(t->child_ctx.caller_session_key) - 1);
    t->child_ctx.caller_is_subagent = true;
    strncpy(t->child_ctx.subagent_id, id, sizeof(t->child_ctx.subagent_id) - 1);

    t->messages = cJSON_CreateArray();
    if (!t->messages) {
        subagent_task_destroy(t);
        return NULL;
    }

    char user_content[8192];
    if (spec->context[0]) {
        snprintf(user_content, sizeof(user_content),
                 "%.*s\n\n## Context\n%.*s",
                 3500, spec->task,
                 3500, spec->context);
    } else {
        snprintf(user_content, sizeof(user_content), "%.*s", 7000, spec->task);
    }
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_content);
    cJSON_AddItemToArray(t->messages, user_msg);

    t->max_iters = (spec->max_iters > 0) ? spec->max_iters : profile->cfg.max_iters;
    if (t->max_iters <= 0) t->max_iters = 20;

    int timeout = (spec->timeout_sec > 0) ? spec->timeout_sec : profile->cfg.timeout_sec;
    if (timeout > 0) {
        t->deadline_ms = mimi_time_ms() + (uint64_t)timeout * 1000ULL;
    }

    return t;
}

void subagent_task_destroy(subagent_task_t *t)
{
    if (!t) return;
    if (t->messages) {
        cJSON_Delete(t->messages);
        t->messages = NULL;
    }
    for (int i = 0; i < STEER_QUEUE_MAX; i++) {
        free(t->steer_q[i]);
        t->steer_q[i] = NULL;
    }
    if (t->cv) mimi_cond_destroy(t->cv);
    if (t->mu) mimi_mutex_destroy(t->mu);
    free(t);
}

mimi_err_t subagent_task_start(subagent_task_t *t)
{
    if (!t) return MIMI_ERR_INVALID_ARG;
    mimi_mutex_lock(t->mu);
    if (t->rec.state != SUBAGENT_STATE_PENDING) {
        mimi_mutex_unlock(t->mu);
        return MIMI_ERR_INVALID_STATE;
    }
    t->rec.state = SUBAGENT_STATE_RUNNING;
    rec_touch(t);
    mimi_mutex_unlock(t->mu);

    subagent_step_llm(t);
    return MIMI_OK;
}

void subagent_task_cancel(subagent_task_t *t, subagent_cancel_mode_t mode)
{
    if (!t) return;
    mimi_mutex_lock(t->mu);
    if (t->rec.state == SUBAGENT_STATE_FINISHED) {
        mimi_mutex_unlock(t->mu);
        return;
    }
    if (mode == SUBAGENT_CANCEL_KILL) t->cancel_kill = true;
    else t->cancel_soft = true;
    rec_touch(t);
    mimi_mutex_unlock(t->mu);
}

mimi_err_t subagent_task_steer(subagent_task_t *t, const char *msg, int *out_depth)
{
    if (!t || !msg || !msg[0]) return MIMI_ERR_INVALID_ARG;
    mimi_mutex_lock(t->mu);
    if (t->rec.state != SUBAGENT_STATE_RUNNING) {
        mimi_mutex_unlock(t->mu);
        return MIMI_ERR_INVALID_STATE;
    }
    if (t->steer_count >= STEER_QUEUE_MAX) {
        mimi_mutex_unlock(t->mu);
        return MIMI_ERR_NO_MEM;
    }
    char *dup = strdup(msg);
    if (!dup) {
        mimi_mutex_unlock(t->mu);
        return MIMI_ERR_NO_MEM;
    }
    t->steer_q[t->steer_tail] = dup;
    t->steer_tail = (t->steer_tail + 1) % STEER_QUEUE_MAX;
    t->steer_count++;
    if (out_depth) *out_depth = t->steer_count;
    rec_touch(t);
    mimi_mutex_unlock(t->mu);
    return MIMI_OK;
}

void subagent_task_snapshot(subagent_task_t *t, subagent_record_t *out_rec)
{
    if (!t || !out_rec) return;
    mimi_mutex_lock(t->mu);
    *out_rec = t->rec;
    mimi_mutex_unlock(t->mu);
}

bool subagent_task_is_finished(subagent_task_t *t)
{
    if (!t) return true;
    mimi_mutex_lock(t->mu);
    bool fin = (t->rec.state == SUBAGENT_STATE_FINISHED);
    mimi_mutex_unlock(t->mu);
    return fin;
}

void subagent_task_join_result(subagent_task_t *t, subagent_join_result_t *out)
{
    if (!t || !out) return;
    mimi_mutex_lock(t->mu);
    *out = t->join;
    mimi_mutex_unlock(t->mu);
}

bool subagent_task_wait(subagent_task_t *t, int wait_ms)
{
    if (!t) return true;
    if (wait_ms <= 0) return subagent_task_is_finished(t);

    const uint64_t deadline = mimi_time_ms() + (uint64_t)wait_ms;
    mimi_mutex_lock(t->mu);
    while (t->rec.state != SUBAGENT_STATE_FINISHED) {
        uint64_t now = mimi_time_ms();
        if (now >= deadline) break;
        int slice = (int)(deadline - now);
        if (slice > 50) slice = 50;
        mimi_cond_wait(t->cv, t->mu, slice);
    }
    bool fin = (t->rec.state == SUBAGENT_STATE_FINISHED);
    mimi_mutex_unlock(t->mu);
    return fin;
}

