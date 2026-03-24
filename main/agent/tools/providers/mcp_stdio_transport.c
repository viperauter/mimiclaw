#include "tools/providers/mcp_provider_internal.h"

#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>

static const char *TAG = "mcp_stdio_transport";

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static int split_cmd(const char *cmd, char **argv, int argv_cap, char *storage, size_t storage_cap)
{
    if (!cmd || !argv || argv_cap < 2 || !storage || storage_cap == 0) return 0;
    strncpy(storage, cmd, storage_cap - 1);
    storage[storage_cap - 1] = '\0';
    int argc = 0;
    char *p = storage;
    while (*p && argc < argv_cap - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static bool jsonrpc_id_matches(cJSON *mid, const char *id_str)
{
    if (!mid || !id_str) return false;
    if (cJSON_IsString(mid)) return strcmp(mid->valuestring, id_str) == 0;
    if (cJSON_IsNumber(mid)) {
        unsigned long long expected = (unsigned long long)strtoull(id_str, NULL, 10);
        unsigned long long got = (unsigned long long)mid->valuedouble;
        return got == expected;
    }
    return false;
}

static mimi_err_t rpc_send_line(mcp_server_t *s, const char *line)
{
    if (!s || !s->started || s->to_child < 0 || !line) return MIMI_ERR_INVALID_ARG;
    size_t n = strlen(line);
    if (write(s->to_child, line, n) != (ssize_t)n) return MIMI_ERR_FAIL;
    if (write(s->to_child, "\n", 1) != 1) return MIMI_ERR_FAIL;
    return MIMI_OK;
}

static mimi_err_t rpc_send_json(mcp_server_t *s, cJSON *obj)
{
    if (!s || !obj) return MIMI_ERR_INVALID_ARG;
    char *line = cJSON_PrintUnformatted(obj);
    if (!line) return MIMI_ERR_NO_MEM;
    mimi_err_t err = rpc_send_line(s, line);
    free(line);
    return err;
}

mimi_err_t mcp_stdio_send_json(mcp_server_t *s, cJSON *obj)
{
    return rpc_send_json(s, obj);
}

static mimi_err_t rpc_read_line(mcp_server_t *s, char *buf, size_t buf_sz, int timeout_ms)
{
    if (!s || !s->started || s->from_child < 0 || !buf || buf_sz == 0) return MIMI_ERR_INVALID_ARG;
    size_t off = 0;
    long long deadline = (timeout_ms > 0) ? (now_ms() + (long long)timeout_ms) : 0;
    while (off + 1 < buf_sz) {
        if (timeout_ms > 0) {
            long long rem = deadline - now_ms();
            if (rem <= 0) return MIMI_ERR_TIMEOUT;
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(s->from_child, &rfds);
            struct timeval tv;
            tv.tv_sec = (long)(rem / 1000LL);
            tv.tv_usec = (long)((rem % 1000LL) * 1000LL);
            int ready = select(s->from_child + 1, &rfds, NULL, NULL, &tv);
            if (ready <= 0) return MIMI_ERR_TIMEOUT;
        }
        char c;
        ssize_t r = read(s->from_child, &c, 1);
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            return MIMI_ERR_FAIL;
        }
        if (c == '\n') break;
        buf[off++] = c;
    }
    buf[off] = '\0';
    return (off > 0) ? MIMI_OK : MIMI_ERR_FAIL;
}

mimi_err_t mcp_stdio_start(mcp_server_t *s)
{
    if (!s || !s->command[0]) return MIMI_ERR_INVALID_ARG;
    if (s->started) return MIMI_OK;

    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) < 0) return MIMI_ERR_FAIL;
    if (pipe(out_pipe) < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        return MIMI_ERR_FAIL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return MIMI_ERR_FAIL;
    }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        char cmd_storage[512];
        char args_storage[512];
        char *cmd_argv[32];
        char *args_argv[32];

        int cmd_argc = split_cmd(s->command, cmd_argv, 32, cmd_storage, sizeof(cmd_storage));
        if (cmd_argc <= 0) _exit(127);

        int argc = cmd_argc;
        if (s->args[0]) {
            int extra_argc = split_cmd(s->args, args_argv, 32, args_storage, sizeof(args_storage));
            for (int i = 0; i < extra_argc && argc < 31; i++) {
                cmd_argv[argc++] = args_argv[i];
            }
        }
        cmd_argv[argc] = NULL;

        execvp(cmd_argv[0], cmd_argv);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    s->pid = pid;
    s->to_child = in_pipe[1];
    s->from_child = out_pipe[0];
    s->started = true;
    s->initialized = false;
    s->last_ping_ms = 0;
    MIMI_LOGI(TAG, "Started MCP stdio server %s pid=%d", s->name, (int)pid);
    return MIMI_OK;
}

char *mcp_stdio_exchange(mcp_server_t *s, uint64_t *rpc_id_next, const char *method, cJSON *params,
                         mcp_server_msg_cb_t on_notification, mcp_server_msg_cb_t on_request)
{
    if (!s || !rpc_id_next || !method) return NULL;

    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%llu", (unsigned long long)(*rpc_id_next)++);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "id", id_str);
    cJSON_AddStringToObject(req, "method", method);
    if (params) cJSON_AddItemToObject(req, "params", params);
    char *line = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!line) return NULL;
    mimi_err_t err = rpc_send_line(s, line);
    free(line);
    if (err != MIMI_OK) return NULL;

    long long start = now_ms();
    const int timeout_ms = 30000;
    for (;;) {
        int rem_ms = timeout_ms;
        long long elapsed = now_ms() - start;
        rem_ms = (int)((elapsed >= (long long)timeout_ms) ? 0 : (long long)timeout_ms - elapsed);
        if (rem_ms <= 0) return NULL;

        char msg_line[16384];
        mimi_err_t rerr = rpc_read_line(s, msg_line, sizeof(msg_line), rem_ms);
        if (rerr != MIMI_OK) return NULL;

        cJSON *msg = cJSON_Parse(msg_line);
        if (!msg || !cJSON_IsObject(msg)) {
            cJSON_Delete(msg);
            continue;
        }
        cJSON *mid = cJSON_GetObjectItemCaseSensitive(msg, "id");
        if (mid && jsonrpc_id_matches(mid, id_str)) {
            char *ret = strdup(msg_line);
            cJSON_Delete(msg);
            return ret;
        }
        cJSON *mmethod = cJSON_GetObjectItemCaseSensitive(msg, "method");
        if (mmethod && cJSON_IsString(mmethod)) {
            if (mid) {
                if (on_request) on_request(s, msg);
            } else {
                if (on_notification) on_notification(s, msg);
            }
        }
        cJSON_Delete(msg);
    }
}

mimi_err_t mcp_stdio_notify(mcp_server_t *s, const char *method, cJSON *params)
{
    if (!s || !method) return MIMI_ERR_INVALID_ARG;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", method);
    if (params) cJSON_AddItemToObject(req, "params", params);
    mimi_err_t err = rpc_send_json(s, req);
    cJSON_Delete(req);
    return err;
}
