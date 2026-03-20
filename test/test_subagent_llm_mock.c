#include "unity.h"
#include "fff.h"
#include "fff_mocks.h"

#include "agent/subagent/subagent_manager.h"
#include "agent/subagent/subagent_config.h"
#include "services/llm/llm_proxy.h"
#include "tools/tool_registry.h"
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

DEFINE_FAKE_VALUE_FUNC0(mimi_err_t, llm_proxy_init);
DEFINE_FAKE_VALUE_FUNC2(mimi_err_t, llm_chat_tools_req, const llm_chat_req_t *, llm_response_t *);
DEFINE_FAKE_VALUE_FUNC4(mimi_err_t, llm_chat_tools_async_req, const llm_chat_req_t *, llm_response_t *, llm_callback_t, void *);
DEFINE_FAKE_VOID_FUNC1(llm_response_free, llm_response_t *);
DEFINE_FAKE_VALUE_FUNC0(const char *, llm_get_last_error);

/* Subagent config related */
static subagent_profile_runtime_t s_test_profile_runtime;

static char test_dir[256];
static mimi_session_ctx_t session_ctx;

/* Test state for tracking LLM async callbacks */
static llm_callback_t s_saved_llm_callback = NULL;
static void *s_saved_user_data = NULL;
static llm_response_t *s_saved_llm_resp = NULL;

void posix_fs_register(void);

/* =========================================================================
 * OS Layer Stub Functions - Same as test_tool_registry.c
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

const char *tool_web_search_schema_json(void) { return "{}"; }

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

const char *tool_get_time_description(void) { return ""; }
const char *tool_get_time_name(void) { return "get_time"; }
const char *tool_get_time_schema_json(void) { return "{}"; }
mimi_err_t tool_get_time_execute(const char *in, char *out, size_t out_sz, const mimi_session_ctx_t *ctx)
{
    (void)in; (void)out; (void)out_sz; (void)ctx;
    return MIMI_OK;
}

mimi_err_t tool_web_search_init(void) { return MIMI_OK; }
const char *tool_web_search_description(void) { return ""; }
const char *tool_web_search_get_name(void) { return "web_search"; }
const char *tool_web_search_get_schema_json(void) { return "{}"; }
mimi_err_t tool_web_search_execute(const char *in, char *out, size_t out_sz, const mimi_session_ctx_t *ctx)
{
    (void)in; (void)out; (void)out_sz; (void)ctx;
    return MIMI_OK;
}

/* Subagent config stubs */
mimi_err_t subagent_config_init(void)
{
    return MIMI_OK;
}

void subagent_config_deinit(void)
{
}

const subagent_profile_runtime_t *subagent_profile_get(const char *profile)
{
    (void)profile;
    return &s_test_profile_runtime;
}

/* Tool registry async stub */
mimi_err_t tool_registry_execute_async(const char *name, const char *input_json,
                                       const mimi_session_ctx_t *session_ctx,
                                       tool_callback_t callback, void *user_data)
{
    (void)name;
    (void)input_json;
    (void)session_ctx;
    
    if (callback) {
        callback(MIMI_OK, name, "{\"status\": \"OK\"}", user_data);
    }
    return MIMI_OK;
}

/* =========================================================================
 * Helper Functions for LLM Mocking
 * ========================================================================= */

static mimi_err_t mock_llm_chat_tools_async_req(const llm_chat_req_t *req,
                                                   llm_response_t *resp,
                                                   llm_callback_t callback,
                                                   void *user_data)
{
    (void)req;
    s_saved_llm_callback = callback;
    s_saved_user_data = user_data;
    s_saved_llm_resp = resp;
    return MIMI_OK;
}

static void setup_mock_llm_response_text(llm_response_t *resp, const char *text)
{
    memset(resp, 0, sizeof(*resp));
    if (text && text[0]) {
        size_t len = strlen(text);
        resp->text = (char *)malloc(len + 1);
        if (resp->text) {
            strcpy(resp->text, text);
            resp->text_len = len;
        }
    }
    resp->tool_use = false;
}

static void setup_mock_llm_response_with_tool(llm_response_t *resp,
                                                const char *tool_name,
                                                const char *tool_input)
{
    memset(resp, 0, sizeof(*resp));
    
    llm_tool_call_t *call = &resp->calls[0];
    strncpy(call->id, "toolu_12345", sizeof(call->id) - 1);
    strncpy(call->name, tool_name, sizeof(call->name) - 1);
    if (tool_input && tool_input[0]) {
        call->input = strdup(tool_input);
        call->input_len = strlen(tool_input);
    }
    resp->call_count = 1;
    resp->tool_use = true;
}

static void trigger_llm_callback_success_with_text(const char *text)
{
    TEST_ASSERT_NOT_NULL(s_saved_llm_callback);
    TEST_ASSERT_NOT_NULL(s_saved_llm_resp);
    
    setup_mock_llm_response_text(s_saved_llm_resp, text);
    
    llm_callback_t callback = s_saved_llm_callback;
    void *user_data = s_saved_user_data;
    llm_response_t *resp = s_saved_llm_resp;
    
    s_saved_llm_callback = NULL;
    s_saved_user_data = NULL;
    s_saved_llm_resp = NULL;
    
    callback(MIMI_OK, resp, user_data);
}

/* =========================================================================
 * Test Setup / Teardown
 * ========================================================================= */

void setUp(void)
{
    snprintf(test_dir, sizeof(test_dir), "/tmp/mimiclaw_llm_test_%d", getpid());
    mkdir(test_dir, 0755);
    memset(&session_ctx, 0, sizeof(session_ctx));
    strncpy(session_ctx.workspace_root, test_dir, sizeof(session_ctx.workspace_root) - 1);
    strncpy(session_ctx.requester_session_key, "test_session_key", sizeof(session_ctx.requester_session_key) - 1);
    session_ctx.caller_is_subagent = false;

    TEST_ASSERT_EQUAL_INT(MIMI_OK, mimi_fs_init());
    posix_fs_register();
    TEST_ASSERT_EQUAL_INT(MIMI_OK, mimi_fs_workspace_create("test", test_dir));
    TEST_ASSERT_EQUAL_INT(MIMI_OK, mimi_fs_workspace_activate("test"));

    FFF_RESET_ALL();
    s_saved_llm_callback = NULL;
    s_saved_user_data = NULL;
    s_saved_llm_resp = NULL;

    tool_provider_registry_init_fake.return_val = MIMI_OK;
    tool_provider_register_fake.return_val = MIMI_OK;
    tool_provider_get_all_tools_json_fake.return_val = "[]";
    mcp_stdio_provider_get_fake.return_val = NULL;

    mimi_cfg_obj_t fake_cfg = {.node = (const void *)0x1000};
    mimi_cfg_section_fake.return_val = fake_cfg;
    mimi_cfg_get_obj_fake.return_val = fake_cfg;
    mimi_cfg_get_str_fake.return_val = "test_value";

    llm_proxy_init_fake.return_val = MIMI_OK;
    llm_chat_tools_async_req_fake.custom_fake = mock_llm_chat_tools_async_req;
    llm_get_last_error_fake.return_val = "";

    memset(&s_test_profile_runtime, 0, sizeof(s_test_profile_runtime));
    strncpy(s_test_profile_runtime.cfg.profile, "default", sizeof(s_test_profile_runtime.cfg.profile) - 1);
    strncpy(s_test_profile_runtime.system_prompt, "You are a helpful assistant.", sizeof(s_test_profile_runtime.system_prompt) - 1);
    s_test_profile_runtime.cfg.max_iters = 10;
    s_test_profile_runtime.cfg.timeout_sec = 300;
    s_test_profile_runtime.cfg.isolated_context = true;

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

void test_llm_proxy_mock_setup(void)
{
    TEST_ASSERT_EQUAL_INT(MIMI_OK, llm_proxy_init());
    TEST_ASSERT_EQUAL_INT(1, llm_proxy_init_fake.call_count);
}

void test_subagent_spawn_with_mock_llm_text_response(void)
{
    subagent_config_init();
    subagent_manager_init();

    subagent_spawn_spec_t spec;
    memset(&spec, 0, sizeof(spec));
    strncpy(spec.profile, "default", sizeof(spec.profile) - 1);
    strncpy(spec.task, "Test subagent task", sizeof(spec.task) - 1);
    spec.max_iters = 1;
    spec.isolated_context = true;

    char subagent_id[64];
    mimi_err_t err = subagent_spawn(&spec, subagent_id, sizeof(subagent_id), &session_ctx);
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);

    TEST_ASSERT_EQUAL_INT(1, llm_chat_tools_async_req_fake.call_count);

    trigger_llm_callback_success_with_text("Task completed successfully!");

    subagent_join_result_t result;
    err = subagent_join(subagent_id, 0, &result, &session_ctx);
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_TRUE(result.ok);
    TEST_ASSERT_EQUAL_STRING("completed", subagent_reason_name(result.reason));
    TEST_ASSERT_EQUAL_STRING("Task completed successfully!", result.final_text);

    subagent_manager_deinit();
    subagent_config_deinit();
}

void test_llm_response_mock_with_tool_calls(void)
{
    llm_response_t resp;
    
    setup_mock_llm_response_with_tool(&resp, "write_file", "{\"path\": \"test.txt\", \"content\": \"hello\"}");
    
    TEST_ASSERT_TRUE(resp.tool_use);
    TEST_ASSERT_EQUAL_INT(1, resp.call_count);
    TEST_ASSERT_EQUAL_STRING("write_file", resp.calls[0].name);
    TEST_ASSERT_NOT_NULL(resp.calls[0].input);
    TEST_ASSERT_EQUAL_STRING("{\"path\": \"test.txt\", \"content\": \"hello\"}", resp.calls[0].input);
    
    llm_response_free(&resp);
}
