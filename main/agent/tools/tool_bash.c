#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "tools/tool_bash.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include "log.h"
#include "cJSON.h"
#include "fs/fs.h"

#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <fcntl.h>
#include <time.h>
#define TOOL_BASH_HAS_SANDBOX 1
#else
#define TOOL_BASH_HAS_SANDBOX 0
#endif

static const char *TAG = "tool_bash";

#define MAX_COMMAND_LENGTH 4096
#define MAX_OUTPUT_SIZE 32768
#define COMMAND_TIMEOUT_SEC 60
#define STACK_SIZE (1024 * 1024)

static tool_bash_env_t s_exec_env = TOOL_BASH_ENV_HOST;
static char s_workspace[512] = "/tmp";

static const char *s_forbidden_commands[] = {
    "rm -rf /",
    "rm -rf /*",
    "mkfs",
    "dd if=",
    "reboot",
    "halt",
    "poweroff",
    "shutdown",
    NULL
};

static bool is_command_safe(const char *cmd)
{
    if (!cmd) return false;

    size_t len = strlen(cmd);
    if (len == 0 || len > MAX_COMMAND_LENGTH) {
        return false;
    }

    for (int i = 0; s_forbidden_commands[i] != NULL; i++) {
        if (strstr(cmd, s_forbidden_commands[i]) != NULL) {
            MIMI_LOGW(TAG, "Forbidden command pattern detected: %s", s_forbidden_commands[i]);
            return false;
        }
    }

    return true;
}

static cJSON *parse_tool_input(const char *input_json)
{
    if (!input_json) {
        return NULL;
    }

    cJSON *root = cJSON_Parse(input_json);
    if (root) {
        return root;
    }

    const char *start = strchr(input_json, '{');
    const char *end = strrchr(input_json, '}');
    if (!start || !end || end <= start) {
        return NULL;
    }

    size_t len = (size_t)(end - start + 1);
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';

    root = cJSON_Parse(buf);
    free(buf);
    return root;
}

#if TOOL_BASH_HAS_SANDBOX

static int install_seccomp(void)
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

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_uname, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_arch_prctl, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getcwd, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getdents, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getpid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getuid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getgid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_geteuid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getegid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getppid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getpgrp, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_setsid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_setpgid, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_umask, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_time, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_gettimeofday, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_ioctl, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_dup, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_dup2, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_stat, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_lstat, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_fstat, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_statfs, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_getdents64, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_fcntl, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_faccessat, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_pipe, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_pipe2, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_newfstatat, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_utimensat, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_epoll_create, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_timer_create, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_timer_settime, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_timer_delete, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_clock_gettime, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, __NR_clock_getres, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_ALLOW),

        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_KILL_PROCESS),
    };

    struct sock_fprog prog = {
        .len = sizeof(filter) / sizeof(filter[0]),
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        MIMI_LOGE(TAG, "prctl(NO_NEW_PRIVS) failed: %s", strerror(errno));
        return -1;
    }

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
        MIMI_LOGE(TAG, "prctl(SECCOMP) failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int setup_mount_namespace(const char *workspace)
{
    if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL)) {
        MIMI_LOGE(TAG, "mount(MS_PRIVATE) failed: %s", strerror(errno));
        return -1;
    }

    char sandbox_root[512];
    snprintf(sandbox_root, sizeof(sandbox_root), "%s/.sandbox", workspace);

    if (mkdir(sandbox_root, 0755) < 0 && errno != EEXIST) {
        MIMI_LOGE(TAG, "mkdir sandbox root failed: %s", strerror(errno));
        return -1;
    }

    if (chdir(sandbox_root) < 0) {
        MIMI_LOGE(TAG, "chdir to sandbox root failed: %s", strerror(errno));
        return -1;
    }

    if (chroot(".") < 0) {
        MIMI_LOGE(TAG, "chroot failed: %s", strerror(errno));
        return -1;
    }

    if (chdir("/") < 0) {
        MIMI_LOGE(TAG, "chdir to / failed: %s", strerror(errno));
        return -1;
    }

    if (mkdir("/workspace", 0755) < 0 && errno != EEXIST) {
    }

    if (mount(workspace, "/workspace", NULL, MS_BIND | MS_REC, NULL)) {
        MIMI_LOGE(TAG, "mount workspace failed: %s", strerror(errno));
        return -1;
    }

    if (chdir("/workspace") < 0) {
        MIMI_LOGE(TAG, "chdir to /workspace failed: %s", strerror(errno));
        return -1;
    }

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL)) {
        MIMI_LOGW(TAG, "mount /proc failed: %s", strerror(errno));
    }

    mkdir("/tmp", 01777);
    mkdir("/dev", 0755);

    return 0;
}

static int child_func(void *arg)
{
    char **argv = arg;
    char *cmd = argv[0];
    char *workspace = argv[1];

    if (setup_mount_namespace(workspace) < 0) {
        return 1;
    }

    if (install_seccomp() < 0) {
        MIMI_LOGW(TAG, "seccomp install failed, continuing without it");
    }

    char *sh_args[] = {"sh", "-c", cmd, NULL};
    execvp("/bin/sh", sh_args);

    perror("execvp");
    return 1;
}

static mimi_err_t exec_in_sandbox(const char *cmd, const char *workspace,
                                  char *output, size_t output_size)
{
    if (!cmd || !output || output_size == 0) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (!is_command_safe(cmd)) {
        snprintf(output, output_size, "Error: command contains forbidden patterns");
        return MIMI_ERR_INVALID_ARG;
    }

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        snprintf(output, output_size, "Error: failed to allocate stack");
        return MIMI_ERR_NO_MEM;
    }

    int stdout_pipe[2];
    if (pipe(stdout_pipe) < 0) {
        free(stack);
        snprintf(output, output_size, "Error: failed to create pipe");
        return MIMI_ERR_FAIL;
    }

    int stderr_pipe[2];
    if (pipe(stderr_pipe) < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        free(stack);
        snprintf(output, output_size, "Error: failed to create stderr pipe");
        return MIMI_ERR_FAIL;
    }

    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD;

    char *child_argv[] = {(char *)cmd, (char *)workspace, NULL};
    pid_t pid = clone(child_func, stack + STACK_SIZE, flags, child_argv);
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        free(stack);
        snprintf(output, output_size, "Error: clone failed: %s", strerror(errno));
        return MIMI_ERR_FAIL;
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int status;
    int total_read = 0;
    fd_set read_fds;
    char buf[512];

    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec > COMMAND_TIMEOUT_SEC) {
            MIMI_LOGW(TAG, "Command timed out, killing process");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            snprintf(output, output_size, "Error: command timed out after %d seconds", COMMAND_TIMEOUT_SEC);
            break;
        }

        FD_ZERO(&read_fds);
        FD_SET(stdout_pipe[0], &read_fds);
        FD_SET(stderr_pipe[0], &read_fds);

        struct timeval tv = {1, 0};
        int ready = select(stdout_pipe[0] > stderr_pipe[0] ? stdout_pipe[0] + 1 : stderr_pipe[0] + 1,
                          &read_fds, NULL, NULL, &tv);

        if (ready > 0) {
            if (FD_ISSET(stdout_pipe[0], &read_fds)) {
                ssize_t n = read(stdout_pipe[0], buf, sizeof(buf) - 1);
                if (n > 0) {
                    if (total_read + n < (int)output_size - 1) {
                        memcpy(output + total_read, buf, n);
                        total_read += n;
                    }
                }
            }
            if (FD_ISSET(stderr_pipe[0], &read_fds)) {
                ssize_t n = read(stderr_pipe[0], buf, sizeof(buf) - 1);
                if (n > 0) {
                    if (total_read + n < (int)output_size - 1) {
                        memcpy(output + total_read, buf, n);
                        total_read += n;
                    }
                }
            }
        }

        int ret = waitpid(pid, &status, WNOHANG);
        if (ret > 0) {
            break;
        } else if (ret < 0) {
            break;
        }
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    if (total_read == 0) {
        output[0] = '\0';
    } else if (total_read > 0 && output[total_read - 1] == '\n') {
        output[total_read - 1] = '\0';
    }

    MIMI_LOGD(TAG, "Sandbox command exited with status: %d", WEXITSTATUS(status));

    free(stack);
    return MIMI_OK;
}

static mimi_err_t exec_in_host(const char *cmd, char *output, size_t output_size)
{
    if (!cmd || !output || output_size == 0) {
        return MIMI_ERR_INVALID_ARG;
    }

    if (!is_command_safe(cmd)) {
        snprintf(output, output_size, "Error: command contains forbidden patterns");
        return MIMI_ERR_INVALID_ARG;
    }

    int stdout_pipe[2];
    if (pipe(stdout_pipe) < 0) {
        snprintf(output, output_size, "Error: failed to create pipe");
        return MIMI_ERR_FAIL;
    }

    int stderr_pipe[2];
    if (pipe(stderr_pipe) < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        snprintf(output, output_size, "Error: failed to create stderr pipe");
        return MIMI_ERR_FAIL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        snprintf(output, output_size, "Error: fork failed: %s", strerror(errno));
        return MIMI_ERR_FAIL;
    }

    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);

        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        if (chdir(s_workspace) < 0) {
        }

        char *sh_args[] = {"sh", "-c", (char *)cmd, NULL};
        execvp("/bin/sh", sh_args);
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int status;
    int total_read = 0;
    fd_set read_fds;
    char buf[512];

    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec > COMMAND_TIMEOUT_SEC) {
            MIMI_LOGW(TAG, "Command timed out, killing process");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            snprintf(output, output_size, "Error: command timed out after %d seconds", COMMAND_TIMEOUT_SEC);
            break;
        }

        FD_ZERO(&read_fds);
        FD_SET(stdout_pipe[0], &read_fds);
        FD_SET(stderr_pipe[0], &read_fds);

        struct timeval tv = {1, 0};
        int ready = select(stdout_pipe[0] > stderr_pipe[0] ? stdout_pipe[0] + 1 : stderr_pipe[0] + 1,
                          &read_fds, NULL, NULL, &tv);

        if (ready > 0) {
            if (FD_ISSET(stdout_pipe[0], &read_fds)) {
                ssize_t n = read(stdout_pipe[0], buf, sizeof(buf) - 1);
                if (n > 0) {
                    if (total_read + n < (int)output_size - 1) {
                        memcpy(output + total_read, buf, n);
                        total_read += n;
                    }
                }
            }
            if (FD_ISSET(stderr_pipe[0], &read_fds)) {
                ssize_t n = read(stderr_pipe[0], buf, sizeof(buf) - 1);
                if (n > 0) {
                    if (total_read + n < (int)output_size - 1) {
                        memcpy(output + total_read, buf, n);
                        total_read += n;
                    }
                }
            }
        }

        int ret = waitpid(pid, &status, WNOHANG);
        if (ret > 0) {
            break;
        } else if (ret < 0) {
            break;
        }
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    if (total_read == 0) {
        output[0] = '\0';
    } else if (total_read > 0 && output[total_read - 1] == '\n') {
        output[total_read - 1] = '\0';
    }

    MIMI_LOGD(TAG, "Host command exited with status: %d", WEXITSTATUS(status));

    return MIMI_OK;
}

#else

static mimi_err_t exec_in_host(const char *cmd, char *output, size_t output_size)
{
    (void)cmd;
    snprintf(output, output_size, "Error: bash tool not supported on this platform");
    return MIMI_ERR_NOT_SUPPORTED;
}

static mimi_err_t exec_in_sandbox(const char *cmd, const char *workspace,
                                  char *output, size_t output_size)
{
    (void)cmd;
    (void)workspace;
    snprintf(output, output_size, "Error: sandbox not supported on this platform");
    return MIMI_ERR_NOT_SUPPORTED;
}

#endif

mimi_err_t tool_bash_execute(const char *input_json, char *output, size_t output_size,
                             const mimi_session_ctx_t *session_ctx)
{
    cJSON *root = parse_tool_input(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return MIMI_ERR_INVALID_ARG;
    }

    cJSON *cmd_json = cJSON_GetObjectItem(root, "command");
    if (!cmd_json || !cJSON_IsString(cmd_json)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing or invalid 'command' field");
        return MIMI_ERR_INVALID_ARG;
    }

    const char *cmd = cJSON_GetStringValue(cmd_json);
    if (!cmd || strlen(cmd) == 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: empty command");
        return MIMI_ERR_INVALID_ARG;
    }

    const char *workspace = s_workspace;
    if (session_ctx && session_ctx->workspace_root[0]) {
        workspace = session_ctx->workspace_root;
    }

    mimi_err_t result;
    if (s_exec_env == TOOL_BASH_ENV_SANDBOX) {
#if TOOL_BASH_HAS_SANDBOX
        result = exec_in_sandbox(cmd, workspace, output, output_size);
#else
        snprintf(output, output_size, "Error: sandbox not supported on this platform");
        result = MIMI_ERR_NOT_SUPPORTED;
#endif
    } else {
        result = exec_in_host(cmd, output, output_size);
    }

    cJSON_Delete(root);
    return result;
}

void tool_bash_set_env(tool_bash_env_t env)
{
    s_exec_env = env;
    MIMI_LOGI(TAG, "Execution environment set to: %s",
              env == TOOL_BASH_ENV_SANDBOX ? "sandbox" : "host");
}

tool_bash_env_t tool_bash_get_env(void)
{
    return s_exec_env;
}

void tool_bash_set_workspace(const char *workspace)
{
    if (workspace) {
        strncpy(s_workspace, workspace, sizeof(s_workspace) - 1);
        MIMI_LOGI(TAG, "Workspace set to: %s", s_workspace);
    }
}
