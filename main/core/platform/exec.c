#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "exec.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <sys/syscall.h>
#endif

static long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

static void append_output(char *output, size_t output_size, size_t *total,
                          bool *truncated, const char *buf, size_t n)
{
    if (!output || output_size == 0 || !total || !buf || n == 0) return;
    size_t remain = (output_size > 0 && *total < output_size - 1) ? (output_size - 1 - *total) : 0;
    if (remain == 0) {
        *truncated = true;
        return;
    }
    size_t cpy = n < remain ? n : remain;
    memcpy(output + *total, buf, cpy);
    *total += cpy;
    output[*total] = '\0';
    if (cpy < n) *truncated = true;
}

static bool is_forbidden_command(const char *cmd)
{
    static const char *forbidden[] = {
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
    if (!cmd || !cmd[0]) return true;
    for (int i = 0; forbidden[i]; i++) {
        if (strstr(cmd, forbidden[i]) != NULL) return true;
    }
    return false;
}

#if defined(__linux__)
#define STACK_SIZE (1024 * 1024)
typedef struct {
    const mimi_exec_spec_t *spec;
    int stdout_fd;
    int stderr_fd;
} sandbox_child_arg_t;

static int sandbox_install_seccomp(void)
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
        BPF_STMT(BPF_RET + BPF_K, SECCOMP_RET_KILL_PROCESS),
    };

    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        return -1;
    }
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
        return -1;
    }
    return 0;
}

static int sandbox_setup_mount_namespace(const char *workspace)
{
    if (!workspace || !workspace[0]) return -1;
    if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL)) {
        return -1;
    }

    char sandbox_root[512];
    snprintf(sandbox_root, sizeof(sandbox_root), "%s/.sandbox", workspace);
    (void)mkdir(sandbox_root, 0755);
    if (chdir(sandbox_root) < 0) return -1;
    if (chroot(".") < 0) return -1;
    if (chdir("/") < 0) return -1;

    (void)mkdir("/workspace", 0755);
    if (mount(workspace, "/workspace", NULL, MS_BIND | MS_REC, NULL)) {
        return -1;
    }
    if (chdir("/workspace") < 0) return -1;

    (void)mkdir("/proc", 0555);
    (void)mount("proc", "/proc", "proc", 0, NULL);
    (void)mkdir("/tmp", 01777);
    (void)mkdir("/dev", 0755);
    return 0;
}

static int sandbox_child_main(void *arg)
{
    sandbox_child_arg_t *a = (sandbox_child_arg_t *)arg;
    const mimi_exec_spec_t *spec = a ? a->spec : NULL;
    if (!spec || !spec->command) return 127;

    (void)dup2(a->stdout_fd, STDOUT_FILENO);
    (void)dup2(a->stderr_fd, STDERR_FILENO);

    if (a->stdout_fd > STDERR_FILENO) close(a->stdout_fd);
    if (a->stderr_fd > STDERR_FILENO) close(a->stderr_fd);

    const char *workspace = spec->workspace_root;
    if (!workspace || !workspace[0]) return 127;
    if (sandbox_setup_mount_namespace(workspace) < 0) return 126;
    (void)sandbox_install_seccomp();

    if (spec->cwd && spec->cwd[0]) {
        (void)chdir(spec->cwd[0] == '/' ? spec->cwd : "/workspace");
    }

    char *argv[] = {"sh", "-c", (char *)spec->command, NULL};
    execvp("/bin/sh", argv);
    return 127;
}
#endif

mimi_err_t mimi_exec_run(const mimi_exec_spec_t *spec,
                         char *output, size_t output_size,
                         mimi_exec_result_t *result)
{
    if (!spec || !spec->command || !spec->command[0] || !output || output_size == 0 || !result) {
        return MIMI_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));
    output[0] = '\0';
    int timeout_ms = spec->timeout_ms > 0 ? spec->timeout_ms : 60000;
    if (is_forbidden_command(spec->command)) {
        snprintf(output, output_size, "Error: command contains forbidden patterns");
        return MIMI_ERR_INVALID_ARG;
    }

    int out_pipe[2];
    int err_pipe[2];
    if (pipe(out_pipe) < 0) return MIMI_ERR_FAIL;
    if (pipe(err_pipe) < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        return MIMI_ERR_FAIL;
    }

    pid_t pid = -1;
#if defined(__linux__)
    char *stack = NULL;
    sandbox_child_arg_t child_arg = {0};
    if (spec->env == MIMI_EXEC_ENV_SANDBOX) {
        if (geteuid() != 0) {
            close(out_pipe[0]); close(out_pipe[1]);
            close(err_pipe[0]); close(err_pipe[1]);
            snprintf(output, output_size, "Error: sandbox requires root privileges");
            return MIMI_ERR_NOT_SUPPORTED;
        }
        stack = (char *)malloc(STACK_SIZE);
        if (!stack) {
            close(out_pipe[0]); close(out_pipe[1]);
            close(err_pipe[0]); close(err_pipe[1]);
            return MIMI_ERR_NO_MEM;
        }
        child_arg.spec = spec;
        child_arg.stdout_fd = out_pipe[1];
        child_arg.stderr_fd = err_pipe[1];
        int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD;
        pid = clone(sandbox_child_main, stack + STACK_SIZE, flags, &child_arg);
    } else {
        pid = fork();
    }
#else
    pid = fork();
#endif
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
#if defined(__linux__)
        free(stack);
#endif
        return MIMI_ERR_FAIL;
    }

    if (pid == 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        if (spec->cwd && spec->cwd[0]) {
            (void)chdir(spec->cwd);
        }
        char *argv[] = {"sh", "-c", (char *)spec->command, NULL};
        execvp("/bin/sh", argv);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    long long start = monotonic_ms();
    int status = 0;
    size_t total = 0;
    bool out_open = true;
    bool err_open = true;
    char buf[512];

    while (out_open || err_open) {
        if (timeout_ms > 0 && monotonic_ms() - start > timeout_ms) {
            kill(pid, SIGKILL);
            result->timed_out = true;
            break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (out_open) { FD_SET(out_pipe[0], &rfds); if (out_pipe[0] > maxfd) maxfd = out_pipe[0]; }
        if (err_open && spec->merge_stderr) { FD_SET(err_pipe[0], &rfds); if (err_pipe[0] > maxfd) maxfd = err_pipe[0]; }

        struct timeval tv = {0, 200000};
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready > 0) {
            if (out_open && FD_ISSET(out_pipe[0], &rfds)) {
                ssize_t n = read(out_pipe[0], buf, sizeof(buf));
                if (n > 0) append_output(output, output_size, &total, &result->truncated, buf, (size_t)n);
                else out_open = false;
            }
            if (err_open && spec->merge_stderr && FD_ISSET(err_pipe[0], &rfds)) {
                ssize_t n = read(err_pipe[0], buf, sizeof(buf));
                if (n > 0) append_output(output, output_size, &total, &result->truncated, buf, (size_t)n);
                else err_open = false;
            }
        }

        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            if (out_open) {
                ssize_t n;
                while ((n = read(out_pipe[0], buf, sizeof(buf))) > 0) {
                    append_output(output, output_size, &total, &result->truncated, buf, (size_t)n);
                }
                out_open = false;
            }
            if (err_open && spec->merge_stderr) {
                ssize_t n;
                while ((n = read(err_pipe[0], buf, sizeof(buf))) > 0) {
                    append_output(output, output_size, &total, &result->truncated, buf, (size_t)n);
                }
                err_open = false;
            }
            break;
        }
    }

    close(out_pipe[0]);
    close(err_pipe[0]);
    if (result->timed_out) {
        waitpid(pid, &status, 0);
    } else if (waitpid(pid, &status, 0) < 0 && errno != ECHILD) {
#if defined(__linux__)
        free(stack);
#endif
        return MIMI_ERR_FAIL;
    }

    result->output_len = total;
    if (WIFEXITED(status)) {
        result->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result->term_signal = WTERMSIG(status);
        result->exit_code = 128 + result->term_signal;
    } else {
        result->exit_code = -1;
    }
#if defined(__linux__)
    free(stack);
#endif
    (void)spec->max_output_bytes;
    return MIMI_OK;
}
