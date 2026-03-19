#include "unity.h"
TEST_SOURCE_FILE("main/agent/tools/tool_exec.c")
#include "tools/tool_exec.h"
#include "fs/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

static char test_dir[256];
static mimi_session_ctx_t session_ctx;

void posix_fs_register(void);

void setUp(void)
{
    snprintf(test_dir, sizeof(test_dir), "/tmp/mimiclaw_test_%d", getpid());
    mkdir(test_dir, 0755);
    memset(&session_ctx, 0, sizeof(session_ctx));
    strncpy(session_ctx.workspace_root, test_dir, sizeof(session_ctx.workspace_root) - 1);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, mimi_fs_init());
    posix_fs_register();
    TEST_ASSERT_EQUAL_INT(MIMI_OK, mimi_fs_workspace_create("test", test_dir));
    TEST_ASSERT_EQUAL_INT(MIMI_OK, mimi_fs_workspace_activate("test"));
}

void tearDown(void)
{
    (void)mimi_fs_shutdown();

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    system(cmd);
}

/* Extract sessionId from JSON output */
static const char *extract_session_id(const char *json, char *out, size_t out_size)
{
    const char *p = strstr(json, "\"sessionId\":\"");
    if (!p) return NULL;
    p += 13;
    const char *end = strchr(p, '"');
    if (!end) return NULL;
    size_t len = end - p;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

void test_exec_basic_command(void)
{
    const char *input = "{\"command\": \"echo hello world\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_exec_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "hello world"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"exit_code\":0"));
}

void test_exec_with_working_directory(void)
{
    const char *input = "{\"command\": \"pwd\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_exec_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, test_dir));
}

void test_exec_timeout(void)
{
    const char *input = "{\"command\": \"sleep 3\", \"timeout_sec\": 1}";
    char output[1024] = {0};

    mimi_err_t err = tool_exec_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "\"timed_out\":true"));
}

void test_exec_invalid_json(void)
{
    const char *input = "not valid json";
    char output[1024] = {0};

    mimi_err_t err = tool_exec_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_ERR_INVALID_ARG, err);
}

void test_exec_missing_command(void)
{
    const char *input = "{}";
    char output[1024] = {0};

    mimi_err_t err = tool_exec_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_ERR_INVALID_ARG, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "command_required"));
}

void test_exec_exit_code(void)
{
    const char *input = "{\"command\": \"exit 42\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_exec_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "\"exit_code\":42"));
}

void test_exec_merge_stderr(void)
{
    const char *input = "{\"command\": \"echo to stderr >&2\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_exec_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "to stderr"));
}

void test_exec_background_session_pty(void)
{
    const char *input = "{\"command\": \"sleep 30\", \"pty\": true, \"background\": true}";
    char output[1024] = {0};
    char session_id[64] = {0};

    mimi_err_t err = tool_exec_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "\"status\":\"running\""));
    TEST_ASSERT_NOT_NULL(extract_session_id(output, session_id, sizeof(session_id)));
}

void test_exec_poll_session(void)
{
    const char *start_input = "{\"command\": \"sleep 30\", \"pty\": true, \"background\": true}";
    char output[2048] = {0};
    char session_id[64] = {0};

    /* Start background session */
    mimi_err_t err = tool_exec_execute(start_input, output, sizeof(output), &session_ctx);
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(extract_session_id(output, session_id, sizeof(session_id)));

    /* Poll the session */
    char poll_input[256];
    snprintf(poll_input, sizeof(poll_input),
             "{\"action\": \"poll\", \"sessionId\": \"%s\", \"waitMs\": 50}", session_id);

    err = tool_exec_execute(poll_input, output, sizeof(output), &session_ctx);
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "\"running\":true"));
    TEST_ASSERT_NOT_NULL(strstr(output, "\"exited\":false"));
}

void test_exec_kill_session(void)
{
    const char *start_input = "{\"command\": \"sleep 30\", \"pty\": true, \"background\": true}";
    char output[1024] = {0};
    char session_id[64] = {0};

    /* Start background session */
    mimi_err_t err = tool_exec_execute(start_input, output, sizeof(output), &session_ctx);
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(extract_session_id(output, session_id, sizeof(session_id)));

    /* Kill the session */
    char kill_input[256];
    snprintf(kill_input, sizeof(kill_input),
             "{\"action\": \"kill\", \"sessionId\": \"%s\"}", session_id);

    err = tool_exec_execute(kill_input, output, sizeof(output), &session_ctx);
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "\"action\":\"kill\""));
}

void test_exec_poll_unknown_session(void)
{
    const char *input = "{\"action\": \"poll\", \"sessionId\": \"nonexistent\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_exec_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_ERR_NOT_FOUND, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "unknown_session"));
}

void test_exec_description_and_schema(void)
{
    const char *desc = tool_exec_description();
    const char *schema = tool_exec_schema_json();

    TEST_ASSERT_NOT_NULL(desc);
    TEST_ASSERT_NOT_NULL(schema);
    TEST_ASSERT_TRUE(strlen(desc) > 0);
    TEST_ASSERT_TRUE(strlen(schema) > 0);
    /* Verify schema contains required fields */
    TEST_ASSERT_NOT_NULL(strstr(schema, "command"));
    TEST_ASSERT_NOT_NULL(strstr(schema, "action"));
}
