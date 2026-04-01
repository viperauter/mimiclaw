#include "tools/tool_exec.h"

#include "exec.h"
#include "fs/fs.h"
#include "log.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <pty.h>
#if defined(__linux__)
#include <sys/mount.h>
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/syscall.h>

#include <asm/unistd.h>

#ifndef __NR_alarm
#define __NR_alarm 37
#endif
#ifndef __NR_dup2
#define __NR_dup2 33
#endif
#ifndef __NR_open
#define __NR_open __NR_openat
#endif
#ifndef __NR_pipe
#define __NR_pipe 22
#endif
#ifndef __NR_readlink
#define __NR_readlink 78
#endif
#ifndef __NR_getdents
#define __NR_getdents 61
#endif
#ifndef __NR_getdents64
#define __NR_getdents64 62
#endif
#ifndef __NR_unlink
#define __NR_unlink 87
#endif

#endif

static const char *TAG = "tool_exec";

static const char *TOOL_DESCRIPTION =
    "Execute a command in the workspace. Supports optional PTY + background sessions via process.";

static const char *TOOL_SCHEMA =
    "{"
    "\"type\":\"object\","
    "\"properties\":{"
      "\"action\":{\"type\":\"string\",\"enum\":[\"run\",\"poll\",\"send\",\"send-keys\",\"paste\",\"submit\",\"kill\"],\"default\":\"run\"},"
      "\"mode\":{\"type\":\"string\",\"enum\":[\"sync\",\"session\"],\"default\":\"sync\",\"description\":\"Execution lifecycle mode\"},"
      "\"io\":{\"type\":\"string\",\"enum\":[\"pipe\",\"pty\"],\"default\":\"pipe\",\"description\":\"Process I/O transport\"},"
      "\"command\":{\"type\":\"string\",\"description\":\"Command to execute (sh -c)\"},"
      "\"working_directory\":{\"type\":\"string\",\"description\":\"Optional working directory relative to workspace or absolute path\"},"
      "\"timeout_sec\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Optional timeout in seconds\"},"
      "\"env\":{\"type\":\"string\",\"enum\":[\"host\",\"sandbox\"],\"description\":\"Execution mode\"},"
      "\"max_output_bytes\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Maximum captured output bytes\"},"
      "\"start_wait_ms\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"In session mode, optional initial wait before returning\"},"
      "\"sessionId\":{\"type\":\"string\",\"description\":\"Session id for poll/send/kill\"},"
      "\"text\":{\"type\":\"string\",\"description\":\"Text for send/paste\"},"
      "\"keys\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"cursor\":{\"type\":\"integer\",\"minimum\":0,\"default\":0},"
      "\"waitMs\":{\"type\":\"integer\",\"minimum\":0,\"default\":0},"
      "\"maxBytes\":{\"type\":\"integer\",\"minimum\":1,\"default\":8192}"
    "},"
    "\"required\":[],"
    "\"additionalProperties\":false"
    "}";

const char *tool_exec_schema_json(void) { return TOOL_SCHEMA; }
const char *tool_exec_description(void) { return TOOL_DESCRIPTION; }

static cJSON *parse_tool_input(const char *input_json)
{
    if (!input_json || !input_json[0]) return cJSON_CreateObject();
    cJSON *root = cJSON_Parse(input_json);
    if (root && cJSON_IsObject(root)) return root;
    if (root) cJSON_Delete(root);
    return cJSON_CreateObject();
}

typedef struct {
    char id[32];
    pid_t pid;
    int out_fd;
    int in_fd;
    bool is_pty;
    long long deadline_ms;
    size_t output_cap;
    size_t output_total;
    bool truncated;
    char *buf;
    size_t buf_len;
    long long created_ms;
    long long last_access_ms;
} exec_sess_t;

#define MAX_EXEC_SESS 16
static exec_sess_t s_exec_sess[MAX_EXEC_SESS];
static const long long SESSION_TTL_MS = 10LL * 60LL * 1000LL;

static void make_id(char out[32])
{
    unsigned r = (unsigned)getpid() ^ (unsigned)rand();
    snprintf(out, 32, "%08x", r);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static exec_sess_t *sess_alloc(void)
{
    for (int i = 0; i < MAX_EXEC_SESS; i++) {
        if (!s_exec_sess[i].id[0]) return &s_exec_sess[i];
    }
    return NULL;
}

static exec_sess_t *sess_find(const char *id)
{
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < MAX_EXEC_SESS; i++) {
        if (s_exec_sess[i].id[0] && strcmp(s_exec_sess[i].id, id) == 0) return &s_exec_sess[i];
    }
    return NULL;
}

static void sess_free(exec_sess_t *s)
{
    if (!s) return;
    if (s->in_fd >= 0) close(s->in_fd);
    if (s->out_fd >= 0) close(s->out_fd);
    if (s->pid > 0) {
        (void)kill(s->pid, SIGTERM);
        (void)waitpid(s->pid, NULL, 0);
    }
    free(s->buf);
    memset(s, 0, sizeof(*s));
    s->in_fd = -1;
    s->out_fd = -1;
}

static void sessions_gc(void)
{
    long long now = now_ms();
    for (int i = 0; i < MAX_EXEC_SESS; i++) {
        exec_sess_t *s = &s_exec_sess[i];
        if (!s->id[0]) continue;
        if (s->last_access_ms > 0 && now - s->last_access_ms > SESSION_TTL_MS) {
            sess_free(s);
        }
    }
}

static int pty_open(int *master_fd, int *slave_fd)
{
    return openpty(master_fd, slave_fd, NULL, NULL, NULL);
}

static void append_clamped(char *dst, size_t dst_size, size_t *len, const char *src, size_t src_len, bool *truncated)
{
    if (!dst || dst_size == 0 || !len || !src || src_len == 0) return;
    size_t remain = (*len < dst_size - 1) ? (dst_size - 1 - *len) : 0;
    if (remain == 0) {
        if (truncated) *truncated = true;
        return;
    }
    size_t cpy = src_len < remain ? src_len : remain;
    memcpy(dst + *len, src, cpy);
    *len += cpy;
    dst[*len] = '\0';
    if (cpy < src_len && truncated) *truncated = true;
}

#if defined(__linux__)
static int session_sandbox_install_seccomp(void)
{
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD + BPF_W + BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_read, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_write, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_brk, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_mmap, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_mprotect, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_rt_sigreturn, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_exit, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_exit_group, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_execve, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_kill, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_rt_sigaction, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_rt_sigprocmask, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_alarm, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_setsid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_ioctl, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_dup, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_dup2, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_close, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_fcntl, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_open, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_openat, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_pipe, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_readlink, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getcwd, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_chdir, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getdents, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getppid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getuid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_geteuid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getgid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getegid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_gettid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getpgid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_setpgid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_wait4, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_writev, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_readv, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_newfstatat, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_fstat, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_lseek, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_unlink, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_umask, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_times, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_gettimeofday, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_umount2, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_KILL_PROCESS),
    };

    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) return -1;
    return 0;
}

static int session_sandbox_setup_mount_namespace(const char *workspace)
{
    if (!workspace || !workspace[0]) return -1;
    if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL)) return -1;

    char sandbox_root[512];
    snprintf(sandbox_root, sizeof(sandbox_root), "%s/.sandbox", workspace);
    (void)mkdir(sandbox_root, 0755);
    if (chdir(sandbox_root) < 0) return -1;
    if (chroot(".") < 0) return -1;
    if (chdir("/") < 0) return -1;

    (void)mkdir("/workspace", 0755);
    if (mount(workspace, "/workspace", NULL, MS_BIND | MS_REC, NULL)) return -1;
    if (chdir("/workspace") < 0) return -1;

    (void)mkdir("/proc", 0555);
    (void)mount("proc", "/proc", "proc", 0, NULL);
    (void)mkdir("/tmp", 01777);
    (void)mkdir("/dev", 0755);
    (void)mkdir("/dev/pts", 0755);
    if (mount("devpts", "/dev/pts", "devpts", 0, NULL) < 0) {
        (void)mkdir("/dev/ptmx", 0644);
    }
    return 0;
}
#endif

static mimi_err_t start_background_session(const char *cmd, const char *cwd, int use_pty,
                                          mimi_exec_env_t env,
                                          const char *workspace_root,
                                          int timeout_sec,
                                          size_t output_cap,
                                          exec_sess_t **out_sess)
{
    if (!cmd || !out_sess) return MIMI_ERR_INVALID_ARG;
    *out_sess = NULL;

#if defined(__linux__)
    if (env == MIMI_EXEC_ENV_SANDBOX && geteuid() != 0) return MIMI_ERR_NOT_SUPPORTED;
#else
    if (env == MIMI_EXEC_ENV_SANDBOX) return MIMI_ERR_NOT_SUPPORTED;
#endif

    sessions_gc();
    exec_sess_t *s = sess_alloc();
    if (!s) {
        /* LRU evict */
        int lru = -1;
        long long best = 0;
        for (int i = 0; i < MAX_EXEC_SESS; i++) {
            if (!s_exec_sess[i].id[0]) continue;
            long long la = s_exec_sess[i].last_access_ms;
            if (lru < 0 || la < best) {
                lru = i;
                best = la;
            }
        }
        if (lru >= 0) {
            sess_free(&s_exec_sess[lru]);
            s = sess_alloc();
        }
    }
    if (!s) return MIMI_ERR_NO_MEM;

    int in_fd = -1;
    int out_fd = -1;
    int in_pipe[2] = {-1,-1};
    int out_pipe[2] = {-1,-1};
    int master = -1, slave = -1;

    if (use_pty) {
        if (pty_open(&master, &slave) < 0) return MIMI_ERR_FAIL;
        in_fd = master;
        out_fd = master;
    } else {
        if (pipe(in_pipe) < 0) return MIMI_ERR_FAIL;
        if (pipe(out_pipe) < 0) {
            close(in_pipe[0]); close(in_pipe[1]);
            return MIMI_ERR_FAIL;
        }
        in_fd = in_pipe[1];
        out_fd = out_pipe[0];
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (use_pty) {
            close(master);
            close(slave);
        } else {
            close(in_pipe[0]); close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
        }
        return MIMI_ERR_FAIL;
    }
    if (pid == 0) {
        if (use_pty) {
            setsid();
            dup2(slave, STDIN_FILENO);
            dup2(slave, STDOUT_FILENO);
            dup2(slave, STDERR_FILENO);
            close(master);
            close(slave);
        } else {
            dup2(in_pipe[0], STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(out_pipe[1], STDERR_FILENO);
            close(in_pipe[0]); close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
        }
#if defined(__linux__)
        if (env == MIMI_EXEC_ENV_SANDBOX) {
            if (session_sandbox_setup_mount_namespace(workspace_root) < 0) _exit(126);
            if (session_sandbox_install_seccomp() < 0) _exit(126);
        }
#else
        (void)workspace_root;
#endif
        if (cwd && cwd[0]) (void)chdir(cwd);
        char *argv[] = {"sh", "-c", (char *)cmd, NULL};
        execvp("/bin/sh", argv);
        _exit(127);
    }

    if (use_pty) {
        close(slave);
    } else {
        close(in_pipe[0]);
        close(out_pipe[1]);
    }

    memset(s, 0, sizeof(*s));
    s->pid = pid;
    s->in_fd = in_fd;
    s->out_fd = out_fd;
    s->is_pty = use_pty ? true : false;
    s->deadline_ms = (timeout_sec > 0) ? (now_ms() + (long long)timeout_sec * 1000LL) : 0;
    s->output_cap = (output_cap > 0) ? output_cap : (size_t)32768;
    s->output_total = 0;
    s->truncated = false;
    s->buf = (char *)calloc(1, s->output_cap + 1);
    s->buf_len = 0;
    s->created_ms = now_ms();
    s->last_access_ms = s->created_ms;
    /* non-blocking reads for poll */
    int flags = fcntl(s->out_fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(s->out_fd, F_SETFL, flags | O_NONBLOCK);
    make_id(s->id);
    *out_sess = s;
    return MIMI_OK;
}

static void write_poll_json(char *output, size_t output_size,
                            const char *session_id,
                            bool exited, int exit_code,
                            const char *chunk,
                            bool truncated, size_t total,
                            size_t cursor, size_t next_cursor)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "sessionId", session_id ? session_id : "");
    cJSON_AddBoolToObject(resp, "running", !exited);
    cJSON_AddBoolToObject(resp, "exited", exited);
    cJSON_AddNumberToObject(resp, "exit_code", exit_code);
    cJSON_AddBoolToObject(resp, "truncated", truncated);
    cJSON_AddNumberToObject(resp, "totalBytes", (double)total);
    cJSON_AddNumberToObject(resp, "cursor", (double)cursor);
    cJSON_AddNumberToObject(resp, "nextCursor", (double)next_cursor);
    cJSON_AddStringToObject(resp, "output", chunk ? chunk : "");
    char *js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!js) {
        snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"json_encode_failed\"}");
        return;
    }
    strncpy(output, js, output_size - 1);
    output[output_size - 1] = '\0';
    free(js);
}

/* Exposed for exec(action=...) session control */
mimi_err_t tool_exec_process_attach(const char *session_id,
                                   char *output, size_t output_size,
                                   tool_exec_action_t action,
                                   const char *text,
                                   size_t cursor,
                                   int wait_ms,
                                   int max_bytes)
{
    if (!output || output_size == 0) return MIMI_ERR_INVALID_ARG;
    exec_sess_t *s = sess_find(session_id);
    if (!s) {
        snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"unknown_session\"}");
        return MIMI_ERR_NOT_FOUND;
    }

    if (action == TOOL_EXEC_ACT_KILL) {
        sess_free(s);
        snprintf(output, output_size, "{\"status\":\"ok\",\"action\":\"kill\",\"sessionId\":\"%s\"}", session_id);
        return MIMI_OK;
    }

    if (action == TOOL_EXEC_ACT_SEND) {
        const char *t = text ? text : "";
        (void)write(s->in_fd, t, strlen(t));
        snprintf(output, output_size, "{\"status\":\"ok\",\"action\":\"send\",\"sessionId\":\"%s\"}", session_id);
        return MIMI_OK;
    }

    s->last_access_ms = now_ms();
    if (s->created_ms > 0 && s->last_access_ms - s->created_ms > SESSION_TTL_MS) {
        sess_free(s);
        snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"expired\",\"sessionId\":\"%s\"}", session_id);
        return MIMI_ERR_TIMEOUT;
    }

    if (s->deadline_ms > 0 && now_ms() > s->deadline_ms) {
        sess_free(s);
        snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"timeout\",\"sessionId\":\"%s\"}", session_id);
        return MIMI_ERR_TIMEOUT;
    }

    /* poll */
    if (wait_ms > 0) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s->out_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = wait_ms / 1000;
        tv.tv_usec = (wait_ms % 1000) * 1000;
        (void)select(s->out_fd + 1, &rfds, NULL, NULL, &tv);
    }
    if (max_bytes <= 0) max_bytes = 8192;
    if (max_bytes > 32768) max_bytes = 32768;
    /* Drain available output into session buffer (up to cap). */
    while (s->buf_len < s->output_cap) {
        size_t remain = s->output_cap - s->buf_len;
        char tmp[1024];
        size_t chunk = remain < sizeof(tmp) ? remain : sizeof(tmp);
        ssize_t n = read(s->out_fd, tmp, chunk);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            break;
        }
        memcpy(s->buf + s->buf_len, tmp, (size_t)n);
        s->buf_len += (size_t)n;
        s->buf[s->buf_len] = '\0';
    }
    if (s->buf_len >= s->output_cap) s->truncated = true;
    s->output_total = s->buf_len;

    int st = 0;
    pid_t w = waitpid(s->pid, &st, WNOHANG);
    bool exited = (w == s->pid);
    int exit_code = -1;
    if (exited) {
        if (WIFEXITED(st)) exit_code = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) exit_code = 128 + WTERMSIG(st);
    }
    if (cursor > s->buf_len) cursor = s->buf_len;
    size_t avail = s->buf_len - cursor;
    size_t send_n = avail;
    if (send_n > (size_t)max_bytes) send_n = (size_t)max_bytes;
    size_t next_cursor = cursor + send_n;

    char *out_chunk = (char *)malloc(send_n + 1);
    if (!out_chunk) return MIMI_ERR_NO_MEM;
    memcpy(out_chunk, s->buf + cursor, send_n);
    out_chunk[send_n] = '\0';
    write_poll_json(output, output_size, session_id, exited, exit_code, out_chunk, s->truncated, s->output_total, cursor, next_cursor);
    free(out_chunk);
    if (exited) sess_free(s);
    return MIMI_OK;
}

mimi_err_t tool_exec_execute(const char *input_json, char *output, size_t output_size,
                            const mimi_session_ctx_t *session_ctx)
{
    if (!output || output_size == 0) return MIMI_ERR_INVALID_ARG;
    output[0] = '\0';

    cJSON *root = parse_tool_input(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"invalid_json\"}");
        return MIMI_ERR_INVALID_ARG;
    }

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    if (!action || !action[0]) action = "run";

    /* Non-run actions map to old process tool behavior. */
    if (strcmp(action, "poll") == 0 || strcmp(action, "send") == 0 ||
        strcmp(action, "send-keys") == 0 || strcmp(action, "paste") == 0 ||
        strcmp(action, "submit") == 0 || strcmp(action, "kill") == 0) {
        const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "sessionId"));
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
        cJSON *keys = cJSON_GetObjectItem(root, "keys");
        size_t cursor = (size_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "cursor"));
        int wait_ms = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "waitMs"));
        int max_bytes = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "maxBytes"));
        if (!sid || !sid[0]) {
            cJSON_Delete(root);
            snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"sessionId_required\"}");
            return MIMI_ERR_INVALID_ARG;
        }

        tool_exec_action_t act = TOOL_EXEC_ACT_POLL;
        const char *send_text = text;
        char *tmp = NULL;

        if (strcmp(action, "kill") == 0) {
            act = TOOL_EXEC_ACT_KILL;
        } else if (strcmp(action, "submit") == 0) {
            act = TOOL_EXEC_ACT_SEND;
            send_text = "\r";
        } else if (strcmp(action, "paste") == 0 || strcmp(action, "send") == 0) {
            act = TOOL_EXEC_ACT_SEND;
        } else if (strcmp(action, "send-keys") == 0) {
            act = TOOL_EXEC_ACT_SEND;
            if (keys && cJSON_IsArray(keys)) {
                size_t cap = 1024;
                tmp = (char *)calloc(1, cap);
                size_t off = 0;
                int n = cJSON_GetArraySize(keys);
                for (int i = 0; i < n; i++) {
                    const char *k = cJSON_GetStringValue(cJSON_GetArrayItem(keys, i));
                    const char *seq = "";
                    if (!k) continue;
                    if (strcmp(k, "Enter") == 0) seq = "\r";
                    else if (strcmp(k, "C-c") == 0) seq = "\x03";
                    else if (strcmp(k, "Up") == 0) seq = "\x1b[A";
                    else if (strcmp(k, "Down") == 0) seq = "\x1b[B";
                    else if (strcmp(k, "Right") == 0) seq = "\x1b[C";
                    else if (strcmp(k, "Left") == 0) seq = "\x1b[D";
                    else if (strcmp(k, "Tab") == 0) seq = "\t";
                    else seq = k;
                    size_t sl = strlen(seq);
                    if (off + sl + 1 >= cap) break;
                    memcpy(tmp + off, seq, sl);
                    off += sl;
                }
                tmp[off] = '\0';
                send_text = tmp;
            } else {
                send_text = "";
            }
        }

        mimi_err_t err = tool_exec_process_attach(sid, output, output_size, act, send_text, cursor, wait_ms, max_bytes);
        free(tmp);
        cJSON_Delete(root);
        return err;
    }

    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(root, "command"));
    if (!cmd || !cmd[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"command_required\"}");
        return MIMI_ERR_INVALID_ARG;
    }

    const char *workspace =
        (session_ctx && session_ctx->workspace_root[0]) ? session_ctx->workspace_root : ".";
    const char *wd = cJSON_GetStringValue(cJSON_GetObjectItem(root, "working_directory"));
    const char *env = cJSON_GetStringValue(cJSON_GetObjectItem(root, "env"));
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(root, "mode"));
    const char *io = cJSON_GetStringValue(cJSON_GetObjectItem(root, "io"));
    int start_wait_ms = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "start_wait_ms"));

    /* Normalize paths through shared resolver so exec cwd matches
     * where file tools (mimi_fs_*) actually read/write. */
    char real_workspace_buf[512];
    char real_cwd_buf[512];
    const char *workspace_real = workspace;
    const char *cwd_real = workspace;
    const char *wd_input = (wd && wd[0]) ? wd : ".";

    if (session_ctx && session_resolve_path(session_ctx, ".", real_workspace_buf, sizeof(real_workspace_buf)) == MIMI_OK) {
        workspace_real = real_workspace_buf;
    } else if (mimi_fs_resolve_path(workspace, real_workspace_buf, sizeof(real_workspace_buf)) == MIMI_OK) {
        workspace_real = real_workspace_buf;
    }

    if (session_ctx && session_resolve_path(session_ctx, wd_input, real_cwd_buf, sizeof(real_cwd_buf)) == MIMI_OK) {
        cwd_real = real_cwd_buf;
    } else {
        char wd_virtual_buf[512];
        const char *wd_virtual = wd_input;
        if (wd_input[0] != '/') {
            int n = snprintf(wd_virtual_buf, sizeof(wd_virtual_buf), "%s/%s", workspace, wd_input);
            if (n > 0 && (size_t)n < sizeof(wd_virtual_buf)) {
                wd_virtual = wd_virtual_buf;
            }
        }
        if (mimi_fs_resolve_path(wd_virtual, real_cwd_buf, sizeof(real_cwd_buf)) == MIMI_OK) {
            cwd_real = real_cwd_buf;
        }
    }

    MIMI_LOGD(TAG,
              "exec path resolve: workspace_virtual='%s', wd_input='%s', workspace_real='%s', cwd_real='%s'",
              workspace, wd_input, workspace_real, cwd_real);

    int timeout_sec = 60;
    cJSON *to = cJSON_GetObjectItem(root, "timeout_sec");
    if (to && cJSON_IsNumber(to) && to->valuedouble > 0) timeout_sec = (int)to->valuedouble;

    size_t max_out = output_size;
    cJSON *mo = cJSON_GetObjectItem(root, "max_output_bytes");
    if (mo && cJSON_IsNumber(mo) && mo->valuedouble > 0) max_out = (size_t)mo->valuedouble;

    bool use_pty = false;
    if (io && io[0]) {
        if (strcmp(io, "pty") == 0) use_pty = true;
        else if (strcmp(io, "pipe") == 0) use_pty = false;
        else {
            cJSON_Delete(root);
            snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"invalid_io\"}");
            return MIMI_ERR_INVALID_ARG;
        }
    }

    bool session_mode = false;
    if (mode && mode[0]) {
        if (strcmp(mode, "session") == 0) session_mode = true;
        else if (strcmp(mode, "sync") == 0) session_mode = false;
        else {
            cJSON_Delete(root);
            snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"invalid_mode\"}");
            return MIMI_ERR_INVALID_ARG;
        }
    }

    int initial_wait_ms = start_wait_ms;
    if (initial_wait_ms < 0) initial_wait_ms = 0;

    if (session_mode && env && strcmp(env, "sandbox") == 0 && !use_pty) {
        cJSON_Delete(root);
        const char *conflict = "mode=session+io=pipe";
        snprintf(output, output_size,
            "{\"status\":\"error\",\"error\":\"sandbox_env_conflict\","
            "\"message\":\"Sandbox mode does not support %s sessions. "
            "Use env=\\\"host\\\" for non-PTY session execution, "
            "or remove %s from your request.\","
            "\"hint\":\"Set env=\\\"host\\\" for session+pipe, or use env=\\\"sandbox\\\" with mode=sync or io=pty.\","
            "\"conflicting_params\":[\"env=sandbox\",\"%s\"]}",
            conflict, conflict, conflict);
        return MIMI_ERR_NOT_SUPPORTED;
    }

    if (session_mode) {
        mimi_exec_env_t exec_env = (env && strcmp(env, "sandbox") == 0) ? MIMI_EXEC_ENV_SANDBOX : MIMI_EXEC_ENV_HOST;
        const char *session_cwd = cwd_real;
        if (exec_env == MIMI_EXEC_ENV_SANDBOX) {
            session_cwd = "/workspace";
        }
        exec_sess_t *sess = NULL;
        mimi_err_t serr = start_background_session(cmd,
                                                   session_cwd,
                                                   (int)use_pty,
                                                   exec_env,
                                                   workspace_real,
                                                   timeout_sec,
                                                   max_out,
                                                   &sess);
        if (serr != MIMI_OK || !sess) {
            cJSON_Delete(root);
            if (serr == MIMI_ERR_NOT_SUPPORTED && exec_env == MIMI_EXEC_ENV_SANDBOX) {
                snprintf(output, output_size,
                         "{\"status\":\"error\",\"error\":\"sandbox_requires_root\","
                         "\"message\":\"Sandbox PTY sessions require root privileges on this platform.\"}");
            } else {
                snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"session_start_failed\"}");
            }
            return serr != MIMI_OK ? serr : MIMI_ERR_FAIL;
        }

        if (initial_wait_ms > 0) {
            char poll_json[8192] = {0};
            (void)tool_exec_process_attach(sess->id, poll_json, sizeof(poll_json),
                                           TOOL_EXEC_ACT_POLL, NULL, 0, initial_wait_ms, 4096);
            cJSON *pj = cJSON_Parse(poll_json);
            bool exited = false;
            if (pj) {
                cJSON *ex = cJSON_GetObjectItem(pj, "exited");
                if (cJSON_IsBool(ex)) exited = cJSON_IsTrue(ex);
            }
            if (exited) {
                if (pj) cJSON_Delete(pj);
                cJSON_Delete(root);
                strncpy(output, poll_json, output_size - 1);
                output[output_size - 1] = '\0';
                return MIMI_OK;
            }
            /* still running: return same shape with status=running and initial output chunk */
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "status", "running");
            cJSON_AddStringToObject(resp, "sessionId", sess->id);
            if (pj && cJSON_IsObject(pj)) {
                cJSON *out = cJSON_GetObjectItem(pj, "output");
                cJSON_AddStringToObject(resp, "output", cJSON_IsString(out) ? out->valuestring : "");
                cJSON *tr = cJSON_GetObjectItem(pj, "truncated");
                cJSON_AddBoolToObject(resp, "truncated", cJSON_IsTrue(tr));
                cJSON *tb = cJSON_GetObjectItem(pj, "totalBytes");
                cJSON_AddNumberToObject(resp, "totalBytes", cJSON_IsNumber(tb) ? tb->valuedouble : 0);
                cJSON *nc = cJSON_GetObjectItem(pj, "nextCursor");
                cJSON_AddNumberToObject(resp, "nextCursor", cJSON_IsNumber(nc) ? nc->valuedouble : 0);
            } else {
                cJSON_AddStringToObject(resp, "output", "");
                cJSON_AddBoolToObject(resp, "truncated", false);
                cJSON_AddNumberToObject(resp, "totalBytes", 0);
                cJSON_AddNumberToObject(resp, "nextCursor", 0);
            }
            if (pj) cJSON_Delete(pj);
            char *js = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
            cJSON_Delete(root);
            if (!js) return MIMI_ERR_FAIL;
            strncpy(output, js, output_size - 1);
            output[output_size - 1] = '\0';
            free(js);
            return MIMI_OK;
        }

        cJSON_Delete(root);
        snprintf(output, output_size, "{\"status\":\"running\",\"sessionId\":\"%s\"}", sess->id);
        return MIMI_OK;
    }
    /* Unified backend for sync path: run through session spawn + poll loop. */
    mimi_exec_env_t exec_env = (env && strcmp(env, "sandbox") == 0) ? MIMI_EXEC_ENV_SANDBOX : MIMI_EXEC_ENV_HOST;
    const char *session_cwd = cwd_real;
    if (exec_env == MIMI_EXEC_ENV_SANDBOX) {
        session_cwd = "/workspace";
    }
    exec_sess_t *sess = NULL;
    mimi_err_t serr = start_background_session(cmd,
                                               session_cwd,
                                               (int)use_pty,
                                               exec_env,
                                               workspace_real,
                                               timeout_sec,
                                               max_out,
                                               &sess);
    if (serr != MIMI_OK || !sess) {
        cJSON_Delete(root);
        if (serr == MIMI_ERR_NOT_SUPPORTED && exec_env == MIMI_EXEC_ENV_SANDBOX) {
            snprintf(output, output_size,
                     "{\"status\":\"error\",\"error\":\"sandbox_requires_root\","
                     "\"message\":\"Sandbox executions require root privileges on this platform.\"}");
        } else {
            snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"session_start_failed\"}");
        }
        return serr != MIMI_OK ? serr : MIMI_ERR_FAIL;
    }

    char full_out[32768] = {0};
    size_t full_len = 0;
    bool timed_out = false;
    bool out_truncated = false;
    bool exited = false;
    int exit_code = -1;
    size_t cursor = 0;
    const int poll_wait_ms = 100;

    while (!exited) {
        char poll_json[8192] = {0};
        mimi_err_t perr = tool_exec_process_attach(sess->id,
                                                   poll_json,
                                                   sizeof(poll_json),
                                                   TOOL_EXEC_ACT_POLL,
                                                   NULL,
                                                   cursor,
                                                   poll_wait_ms,
                                                   4096);
        if (perr == MIMI_ERR_TIMEOUT) {
            timed_out = true;
            break;
        }
        if (perr != MIMI_OK) {
            cJSON_Delete(root);
            snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"poll_failed\"}");
            return perr;
        }

        cJSON *pj = cJSON_Parse(poll_json);
        if (!pj || !cJSON_IsObject(pj)) {
            if (pj) cJSON_Delete(pj);
            cJSON_Delete(root);
            snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"poll_decode_failed\"}");
            return MIMI_ERR_FAIL;
        }

        cJSON *out = cJSON_GetObjectItem(pj, "output");
        if (cJSON_IsString(out) && out->valuestring) {
            append_clamped(full_out,
                           sizeof(full_out),
                           &full_len,
                           out->valuestring,
                           strlen(out->valuestring),
                           &out_truncated);
        }
        cJSON *nc = cJSON_GetObjectItem(pj, "nextCursor");
        if (cJSON_IsNumber(nc) && nc->valuedouble >= 0) cursor = (size_t)nc->valuedouble;
        cJSON *tr = cJSON_GetObjectItem(pj, "truncated");
        if (cJSON_IsTrue(tr)) out_truncated = true;
        cJSON *ex = cJSON_GetObjectItem(pj, "exited");
        if (cJSON_IsTrue(ex)) exited = true;
        cJSON *ec = cJSON_GetObjectItem(pj, "exit_code");
        if (cJSON_IsNumber(ec)) exit_code = (int)ec->valuedouble;

        cJSON_Delete(pj);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", timed_out ? "error" : "ok");
    cJSON_AddStringToObject(resp, "output", full_out);
    cJSON_AddNumberToObject(resp, "exit_code", exit_code);
    cJSON_AddBoolToObject(resp, "timed_out", timed_out);
    cJSON_AddBoolToObject(resp, "truncated", out_truncated);
    cJSON_AddStringToObject(resp, "env", (exec_env == MIMI_EXEC_ENV_SANDBOX) ? "sandbox" : "host");
    if (timed_out) cJSON_AddStringToObject(resp, "error", "timeout");

    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    if (!s) {
        snprintf(output, output_size, "{\"status\":\"error\",\"error\":\"json_encode_failed\"}");
        return MIMI_ERR_FAIL;
    }
    strncpy(output, s, output_size - 1);
    output[output_size - 1] = '\0';
    free(s);
    return timed_out ? MIMI_ERR_TIMEOUT : MIMI_OK;
}

