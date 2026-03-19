#include "unity.h"
#include "fff.h"
#include "fff_mocks.h"

TEST_SOURCE_FILE("main/agent/tools/tool_registry.c")
TEST_SOURCE_FILE("main/agent/tools/tool_files.c")
TEST_SOURCE_FILE("main/agent/tools/tool_exec.c")

#include "tools/tool_registry.h"
#include "tools/tool_files.h"
#include "fs/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Define FFF fake functions in test file to avoid multiple definitions */
DEFINE_FAKE_VALUE_FUNC0(mimi_err_t, tool_provider_registry_init);
DEFINE_FAKE_VOID_FUNC0(tool_provider_registry_deinit);
DEFINE_FAKE_VALUE_FUNC1(mimi_err_t, tool_provider_register, const void *);
DEFINE_FAKE_VALUE_FUNC0(const char *, tool_provider_get_all_tools_json);
DEFINE_FAKE_VALUE_FUNC0(const void *, mcp_stdio_provider_get);

DEFINE_FAKE_VALUE_FUNC1(mimi_cfg_obj_t, mimi_cfg_section, const char *);
DEFINE_FAKE_VALUE_FUNC2(mimi_cfg_obj_t, mimi_cfg_get_obj, mimi_cfg_obj_t, const char *);
DEFINE_FAKE_VALUE_FUNC3(const char *, mimi_cfg_get_str, mimi_cfg_obj_t, const char *, const char *);

static char test_dir[256];
static mimi_session_ctx_t session_ctx;

void posix_fs_register(void);

/* =========================================================================
 * OS Layer Stub Functions - Direct implementation for output pointer handling
 * ========================================================================= */

mimi_err_t mimi_mutex_create(mimi_mutex_t **out)
{
    if (out) {
        *out = (mimi_mutex_t *)0x1234;
    }
    return MIMI_OK;
}

void mimi_mutex_destroy(mimi_mutex_t *mutex)
{
    (void)mutex;
}

mimi_err_t mimi_mutex_lock(mimi_mutex_t *mutex)
{
    (void)mutex;
    return MIMI_OK;
}

mimi_err_t mimi_mutex_unlock(mimi_mutex_t *mutex)
{
    (void)mutex;
    return MIMI_OK;
}

mimi_err_t mimi_cond_create(mimi_cond_t **out)
{
    if (out) {
        *out = (mimi_cond_t *)0x5678;
    }
    return MIMI_OK;
}

void mimi_cond_destroy(mimi_cond_t *cond)
{
    (void)cond;
}

mimi_err_t mimi_cond_wait(mimi_cond_t *cond, mimi_mutex_t *mutex, uint32_t timeout_ms)
{
    (void)cond;
    (void)mutex;
    (void)timeout_ms;
    return MIMI_OK;
}

mimi_err_t mimi_cond_signal(mimi_cond_t *cond)
{
    (void)cond;
    return MIMI_OK;
}

mimi_err_t mimi_cond_broadcast(mimi_cond_t *cond)
{
    (void)cond;
    return MIMI_OK;
}

mimi_err_t mimi_task_create(const char *name, mimi_task_fn_t fn, void *arg,
                            uint32_t stack_size, int prio, mimi_task_handle_t *out_handle)
{
    (void)name;
    (void)fn;
    (void)arg;
    (void)stack_size;
    (void)prio;
    if (out_handle) {
        *out_handle = (mimi_task_handle_t)0xabcd;
    }
    return MIMI_OK;
}

mimi_err_t mimi_task_delete(mimi_task_handle_t handle)
{
    (void)handle;
    return MIMI_OK;
}

uint64_t mimi_time_ms(void)
{
    return 0;
}

void mimi_sleep_ms(uint32_t ms)
{
    (void)ms;
}

/* =========================================================================
 * Stub implementations for missing functions
 * ========================================================================= */

/* Tool Provider */
mimi_err_t tool_provider_execute(const char *name, const char *in, char *out, size_t out_sz, const void *ctx)
{
    (void)name;
    (void)in;
    (void)out;
    (void)out_sz;
    (void)ctx;
    return MIMI_ERR_NOT_FOUND;
}

bool tool_provider_requires_confirmation(const char *name)
{
    (void)name;
    return false;
}

/* Cron Tools */
const char *tool_cron_add_description(void) { return ""; }
const char *tool_cron_add_name(void) { return "cron_add"; }
const char *tool_cron_add_schema_json(void) { return "{}"; }
mimi_err_t tool_cron_add_execute(const char *in, char *out, size_t out_sz, const mimi_session_ctx_t *ctx)
{
    (void)in; (void)out; (void)out_sz; (void)ctx;
    return MIMI_OK;
}

const char *tool_cron_list_description(void) { return ""; }
const char *tool_cron_list_name(void) { return "cron_list"; }
const char *tool_cron_list_schema_json(void) { return "{}"; }
mimi_err_t tool_cron_list_execute(const char *in, char *out, size_t out_sz, const mimi_session_ctx_t *ctx)
{
    (void)in; (void)out; (void)out_sz; (void)ctx;
    return MIMI_OK;
}

const char *tool_cron_remove_description(void) { return ""; }
const char *tool_cron_remove_name(void) { return "cron_remove"; }
const char *tool_cron_remove_schema_json(void) { return "{}"; }
mimi_err_t tool_cron_remove_execute(const char *in, char *out, size_t out_sz, const mimi_session_ctx_t *ctx)
{
    (void)in; (void)out; (void)out_sz; (void)ctx;
    return MIMI_OK;
}

/* Time Tool */
const char *tool_get_time_description(void) { return ""; }
const char *tool_get_time_name(void) { return "get_time"; }
const char *tool_get_time_schema_json(void) { return "{}"; }
mimi_err_t tool_get_time_execute(const char *in, char *out, size_t out_sz, const mimi_session_ctx_t *ctx)
{
    (void)in; (void)out; (void)out_sz; (void)ctx;
    return MIMI_OK;
}

/* Web Search Tool */
mimi_err_t tool_web_search_init(void) { return MIMI_OK; }
const char *tool_web_search_description(void) { return ""; }
const char *tool_web_search_get_name(void) { return "web_search"; }
const char *tool_web_search_get_schema_json(void) { return "{}"; }
const char *tool_web_search_schema_json(void) { return "{}"; }
mimi_err_t tool_web_search_execute(const char *in, char *out, size_t out_sz, const mimi_session_ctx_t *ctx)
{
    (void)in; (void)out; (void)out_sz; (void)ctx;
    return MIMI_OK;
}

/* =========================================================================
 * Test Setup / Teardown
 * ========================================================================= */

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

    /* Reset all FFF fake functions */
    FFF_RESET_ALL();

    /* Set default return values */
    tool_provider_registry_init_fake.return_val = MIMI_OK;
    tool_provider_register_fake.return_val = MIMI_OK;
    tool_provider_get_all_tools_json_fake.return_val = "[]";
    mcp_stdio_provider_get_fake.return_val = NULL;

    /* Config default values */
    mimi_cfg_obj_t fake_cfg = {.node = (const void *)0x1000};
    mimi_cfg_section_fake.return_val = fake_cfg;
    mimi_cfg_get_obj_fake.return_val = fake_cfg;
    mimi_cfg_get_str_fake.return_val = "test_value";

    tool_registry_init();
}

void tearDown(void)
{
    tool_registry_deinit();
    (void)mimi_fs_shutdown();

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    system(cmd);
}

/* =========================================================================
 * Test Cases
 * ========================================================================= */

void test_registry_init(void)
{
    const char *tools_json = tool_registry_get_tools_json();
    TEST_ASSERT_NOT_NULL(tools_json);
    TEST_ASSERT_TRUE(strstr(tools_json, "write_file") != NULL);
    TEST_ASSERT_TRUE(strstr(tools_json, "read_file") != NULL);

    /* Verify FFF fake function call counts */
    TEST_ASSERT_EQUAL_INT(1, tool_provider_registry_init_fake.call_count);
}

void test_registry_execute_write_file(void)
{
    const char *input = "{\"path\": \"test.txt\", \"content\": \"hello from registry\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_registry_execute("write_file", input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "OK"));
}

void test_registry_execute_read_file(void)
{
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/read_test.txt", test_dir);
    FILE *f = fopen(filepath, "w");
    TEST_ASSERT_NOT_NULL(f);
    if (f) {
        fprintf(f, "content for reading");
        fclose(f);
    }

    const char *input = "{\"path\": \"read_test.txt\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_registry_execute("read_file", input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_EQUAL_STRING("content for reading", output);
}

void test_registry_execute_edit_file(void)
{
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/edit_test.txt", test_dir);
    FILE *f = fopen(filepath, "w");
    TEST_ASSERT_NOT_NULL(f);
    if (f) {
        fprintf(f, "hello world");
        fclose(f);
    }

    const char *input = "{\"path\": \"edit_test.txt\", \"old_string\": \"world\", \"new_string\": \"universe\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_registry_execute("edit_file", input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "OK"));
}

void test_registry_execute_unknown_tool(void)
{
    const char *input = "{}";
    char output[1024] = {0};

    mimi_err_t err = tool_registry_execute("unknown_tool_that_does_not_exist", input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_ERR_NOT_FOUND, err);
}
