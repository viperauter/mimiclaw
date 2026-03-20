#include "unity.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

/* Pull in the real context pipeline implementation under test. */
TEST_SOURCE_FILE("main/agent/context/context_budget_plan.c")
TEST_SOURCE_FILE("main/agent/context/context_compact.c")
TEST_SOURCE_FILE("main/agent/context/context_assembler.c")

#include "agent/context/context_assembler.h"
#include "agent/context/context_compact.h"

/* CMock-generated mocks for session history IO. */
#include "mock_session_mgr.h"

/* Minimal trace stub for unit tests.
 * Context compact/assembler trace code is best-effort and should not
 * interfere with the functional tests here. */
mimi_err_t llm_trace_event_kv(const char *trace_id,
                              const char *event,
                              const char *k1, const char *v1,
                              const char *k2, const char *v2,
                              const char *k3, const char *v3,
                              const char *k4, const char *v4)
{
    (void)trace_id;
    (void)event;
    (void)k1; (void)v1;
    (void)k2; (void)v2;
    (void)k3; (void)v3;
    (void)k4; (void)v4;
    return MIMI_OK;
}

/* -------------------------------------------------------------------------- */
/* CMock stubs                                                             */
/* -------------------------------------------------------------------------- */

static const char *s_history_json = NULL;

static mimi_err_t session_get_history_json_cb(const char *channel,
                                                 const char *chat_id,
                                                 char *buf,
                                                 size_t size,
                                                 int max_msgs)
{
    (void)channel;
    (void)chat_id;
    (void)max_msgs;

    if (!buf || size == 0) return MIMI_ERR_INVALID_ARG;
    if (!s_history_json) {
        buf[0] = '\0';
        return MIMI_OK;
    }

    /* Write the fixed JSON array into the caller-provided buffer. */
    snprintf(buf, size, "%s", s_history_json);
    buf[size - 1] = '\0';
    return MIMI_OK;
}

static mimi_err_t session_append_cb(const char *channel,
                                     const char *chat_id,
                                     const char *role,
                                     const char *content)
{
    (void)channel;
    (void)chat_id;
    (void)role;
    (void)content;
    return MIMI_OK;
}

/* -------------------------------------------------------------------------- */
/* Test setup/teardown                                                     */
/* -------------------------------------------------------------------------- */

void setUp(void)
{
    /* Ensure a deterministic assembler input. */
    s_history_json = "["
                      "{\"role\":\"user\",\"content\":\"m1\"},"
                      "{\"role\":\"assistant\",\"content\":\"m2\"},"
                      "{\"role\":\"user\",\"content\":\"m3\"},"
                      "{\"role\":\"assistant\",\"content\":\"m4\"}"
                      "]";

    /* Allow context_assembler to load history and append the current user. */
    session_get_history_json_fake.custom_fake = session_get_history_json_cb;
    session_get_history_json_fake.return_val = MIMI_OK;
    session_append_fake.custom_fake = session_append_cb;
    session_append_fake.return_val = MIMI_OK;
}

void tearDown(void)
{
    s_history_json = NULL;
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                  */
/* -------------------------------------------------------------------------- */

static const char *get_msg_role(const cJSON *msg)
{
    const cJSON *role = msg ? cJSON_GetObjectItem((cJSON *)msg, "role") : NULL;
    return (role && cJSON_IsString((cJSON *)role) && role->valuestring) ? role->valuestring : NULL;
}

static const char *get_msg_content(const cJSON *msg)
{
    const cJSON *content = msg ? cJSON_GetObjectItem((cJSON *)msg, "content") : NULL;
    return (content && cJSON_IsString((cJSON *)content) && content->valuestring) ? content->valuestring : NULL;
}

static void assert_message(cJSON *msg, const char *role, const char *content)
{
    TEST_ASSERT_NOT_NULL(msg);
    TEST_ASSERT_EQUAL_STRING(role, get_msg_role(msg));
    TEST_ASSERT_EQUAL_STRING(content, get_msg_content(msg));
}

/* -------------------------------------------------------------------------- */
/* Test cases                                                               */
/* -------------------------------------------------------------------------- */

void test_compact_summary_injects_system_at_index_zero(void)
{
    /* Small history buffer size forces trim to happen. */
    char system_prompt_buf[16] = "SYS";
    const char *tools_json = "[]";
    char history_json_buf[512];

    context_assemble_request_t req;
    memset(&req, 0, sizeof(req));
    req.channel = "cli";
    req.chat_id = "chat1";
    req.user_content = "U0";

    req.system_prompt_buf = system_prompt_buf;
    req.system_prompt_buf_size = sizeof(system_prompt_buf);
    req.tools_json = tools_json;
    req.base_memory_window = 10;
    req.context_tokens = 0;

    /* Flush earlier => for this test we just need trimmed_messages_for_compact to exist. */
    req.memory_flush_threshold_ratio = 0.0;

    req.history_json_buf = history_json_buf;
    req.history_json_buf_size = sizeof(history_json_buf);
    req.hooks = NULL;

    context_assemble_result_t out;
    memset(&out, 0, sizeof(out));

    mimi_err_t err = context_assemble_messages_budgeted(&req, &out);
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(out.messages);

    TEST_ASSERT_NOT_NULL(out.trimmed_messages_for_compact);
    TEST_ASSERT_TRUE(cJSON_IsArray(out.trimmed_messages_for_compact));

    int main_n = cJSON_GetArraySize(out.messages);
    /* With ratio=0, assembler trim keeps only the newest message (the appended user). */
    TEST_ASSERT_EQUAL_INT(1, main_n);

    /* Build summary message and inject it to index=0. */
    cJSON *summary = cJSON_CreateObject();
    TEST_ASSERT_NOT_NULL(summary);
    cJSON_AddStringToObject(summary, "role", "system");
    cJSON_AddStringToObject(summary, "content", "SUMMARY");

    mimi_err_t ins = context_compact_insert_summary_message(out.messages, summary);
    TEST_ASSERT_EQUAL_INT(MIMI_OK, ins);

    /* Now messages should be: [summary, newest_user_message] */
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(out.messages));
    cJSON *m0 = cJSON_GetArrayItem(out.messages, 0);
    cJSON *m1 = cJSON_GetArrayItem(out.messages, 1);
    assert_message(m0, "system", "SUMMARY");
    assert_message(m1, "user", "U0");

    cJSON_Delete(out.messages);
    cJSON_Delete(out.trimmed_messages_for_compact);
}

void test_compact_failure_merges_compact_source_back_in_order(void)
{
    char system_prompt_buf[16] = "SYS";
    const char *tools_json = "[]";
    char history_json_buf[512];

    context_assemble_request_t req;
    memset(&req, 0, sizeof(req));
    req.channel = "cli";
    req.chat_id = "chat1";
    req.user_content = "U0";

    req.system_prompt_buf = system_prompt_buf;
    req.system_prompt_buf_size = sizeof(system_prompt_buf);
    req.tools_json = tools_json;
    req.base_memory_window = 10;
    req.context_tokens = 0;

    /* Force flush budget to 0 => compact_source_messages will contain the oldest messages. */
    req.memory_flush_threshold_ratio = 0.0;

    req.history_json_buf = history_json_buf;
    req.history_json_buf_size = sizeof(history_json_buf);
    req.hooks = NULL;

    context_assemble_result_t out;
    memset(&out, 0, sizeof(out));

    mimi_err_t err = context_assemble_messages_budgeted(&req, &out);
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(out.messages);
    TEST_ASSERT_NOT_NULL(out.trimmed_messages_for_compact);

    /* Pre-merge state: messages contains only newest user message. */
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(out.messages));
    TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(out.trimmed_messages_for_compact));

    mimi_err_t merge_err =
        context_compact_merge_compact_source_to_messages(&out.messages, out.trimmed_messages_for_compact);
    TEST_ASSERT_EQUAL_INT(MIMI_OK, merge_err);

    /* Post-merge state:
     * [m1, m2, m3, m4, U0]
     */
    TEST_ASSERT_EQUAL_INT(5, cJSON_GetArraySize(out.messages));
    assert_message(cJSON_GetArrayItem(out.messages, 0), "user", "m1");
    assert_message(cJSON_GetArrayItem(out.messages, 1), "assistant", "m2");
    assert_message(cJSON_GetArrayItem(out.messages, 2), "user", "m3");
    assert_message(cJSON_GetArrayItem(out.messages, 3), "assistant", "m4");
    assert_message(cJSON_GetArrayItem(out.messages, 4), "user", "U0");

    /* The compact_source_messages container should now be empty (items detached). */
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(out.trimmed_messages_for_compact));

    cJSON_Delete(out.messages);
    cJSON_Delete(out.trimmed_messages_for_compact);
}

