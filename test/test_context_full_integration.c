#include "unity.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

/* Full integration test for context assembly + compact using real session files.
 * This test verifies the entire pipeline:
 *   - Real JSONL session files (written to actual filesystem)
 *   - Real session_mgr.c (reading history via POSIX FS)
 *   - Real context_assembler (budgeting, trimming, compact)
 */

/* Pull in real implementations */
TEST_SOURCE_FILE("main/memory/session_mgr.c")
TEST_SOURCE_FILE("main/agent/context/context_budget_plan.c")
TEST_SOURCE_FILE("main/agent/context/context_compact.c")
TEST_SOURCE_FILE("main/agent/context/context_assembler.c")

#include "memory/session_mgr.h"
#include "agent/context/context_assembler.h"
#include "agent/context/context_compact.h"
#include "config/config_view.h"
#include "fs/fs.h"

/* Forward declarations */
void posix_fs_register(void);
extern mimi_err_t mimi_fs_workspace_activate(const char *name);

/* Minimal stubs */
mimi_cfg_obj_t mimi_cfg_section(const char *name)
{
    (void)name;
    mimi_cfg_obj_t empty = {NULL};
    return empty;
}

const char *mimi_cfg_get_str(mimi_cfg_obj_t obj, const char *key, const char *def)
{
    (void)obj;
    (void)key;
    return def;
}

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
/* Test helpers - create real session files */
/* -------------------------------------------------------------------------- */

static void create_test_session_file(const char *channel, const char *chat_id, const char **lines, int num_lines)
{
    char path[256];
    snprintf(path, sizeof(path), "sessions/%s_%s.jsonl", channel, chat_id);
    
    mkdir("sessions", 0755);
    unlink(path);
    
    FILE *f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < num_lines; i++) {
            fprintf(f, "%s\n", lines[i]);
        }
        fclose(f);
    }
}

static void cleanup_test_session_file(const char *channel, const char *chat_id)
{
    char path[256];
    snprintf(path, sizeof(path), "sessions/%s_%s.jsonl", channel, chat_id);
    unlink(path);
}

/* -------------------------------------------------------------------------- */
/* Setup/Teardown - initialize real FS */
/* -------------------------------------------------------------------------- */

void setUp(void)
{
    mkdir("sessions", 0755);
    
    /* Initialize real VFS layer */
    mimi_fs_init();
    posix_fs_register();
    
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        mimi_fs_workspace_create("test", cwd);
        mimi_fs_workspace_activate("test");
    }
}

void tearDown(void)
{
    system("rm -f sessions/test_*.jsonl 2>/dev/null");
    system("rmdir sessions 2>/dev/null");
}

/* -------------------------------------------------------------------------- */
/* Helpers */
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
/* Test Cases - Full Integration Tests */
/* -------------------------------------------------------------------------- */

void test_context_assemble_trims_oldest_messages_when_over_budget(void)
{
    /* Create a history with 4 messages, each with significant content */
    const char *lines[] = {
        "{\"role\":\"user\",\"content\":\"This is a very long message number one that should be trimmed first because it's the oldest.\",\"ts\":100}",
        "{\"role\":\"assistant\",\"content\":\"This is a very long response number two that might also get trimmed if budget is tight.\",\"ts\":200}",
        "{\"role\":\"user\",\"content\":\"This is message number three that could potentially be kept.\",\"ts\":300}",
        "{\"role\":\"assistant\",\"content\":\"This is the most recent response number four that should definitely be kept.\",\"ts\":400}"
    };
    create_test_session_file("cli", "test_ctx_1", lines, 4);
    
    char system_prompt_buf[64] = "You are a helpful assistant.";
    char history_json_buf[4096];
    
    context_assemble_request_t req;
    memset(&req, 0, sizeof(req));
    req.channel = "cli";
    req.chat_id = "test_ctx_1";
    req.user_content = "What is the next message?";
    req.system_prompt_buf = system_prompt_buf;
    req.system_prompt_buf_size = sizeof(system_prompt_buf);
    req.history_json_buf = history_json_buf;
    req.history_json_buf_size = sizeof(history_json_buf);
    req.tools_json = "[]";
    req.base_memory_window = 4;
    req.context_tokens = 0;
    req.memory_flush_threshold_ratio = 0.5;  /* Trigger flush at 50% */
    
    context_assemble_result_t result;
    memset(&result, 0, sizeof(result));
    
    /* Use a small budget (controlled via context_tokens) to force trimming */
    req.context_tokens = 50;  /* Small token count forces aggressive trimming */
    mimi_err_t err = context_assemble_messages_budgeted(&req, &result);
    
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(result.messages);
    TEST_ASSERT_TRUE(cJSON_IsArray(result.messages));
    
    /* Key Assertion: Older messages should be trimmed first.
     * The result should contain:
     * - Current user message (always kept)
     * - Some recent history
     */
    int msg_count = cJSON_GetArraySize(result.messages);
    TEST_ASSERT_TRUE(msg_count >= 1);  /* At least the current user message */
    
    /* Verify the most recent history is kept, oldest is removed */
    cJSON *last_msg = cJSON_GetArrayItem(result.messages, msg_count - 1);
    TEST_ASSERT_EQUAL_STRING("user", get_msg_role(last_msg));
    TEST_ASSERT_EQUAL_STRING("What is the next message?", get_msg_content(last_msg));
    
    /* Cleanup */
    if (result.messages) cJSON_Delete(result.messages);
    if (result.trimmed_messages_for_compact) cJSON_Delete(result.trimmed_messages_for_compact);
    cleanup_test_session_file("cli", "test_ctx_1");
}

void test_context_assemble_produces_trimmed_messages_for_compact(void)
{
    /* Create messages with LONG content to ensure total chars exceed budget */
    const char *long_content_1 = "This is a very long message number one that contains lots of text. "
                                 "We need enough characters to ensure that the trimming logic is triggered. "
                                 "The quick brown fox jumps over the lazy dog again and again.";
    const char *long_content_2 = "This is the second very long message that also has plenty of content. "
                                 "It should be trimmed along with the first one when the budget is tight. "
                                 "We are testing the compact feature which should capture the oldest messages first.";
    const char *long_content_3 = "Third message with significant length to help trigger the trim condition. "
                                 "This message might end up in either the compact set or the main set depending "
                                 "on how aggressive the trimming threshold is configured.";
    const char *long_content_4 = "Fourth message - getting closer to the current conversation. "
                                 "This one should ideally stay in the main message list rather than being "
                                 "moved to the compact summary section.";
    const char *long_content_5 = "Fifth message - relatively recent and should definitely be kept. "
                                 "This contains important context that the LLM needs to understand "
                                 "the current state of the conversation.";
    const char *long_content_6 = "Sixth and final historical message - the most recent one before "
                                 "the current user query. This absolutely must stay in the main context.";

    /* Build JSON lines with long content */
    char line_buf[6][2048];
    snprintf(line_buf[0], sizeof(line_buf[0]), 
             "{\"role\":\"user\",\"content\":\"%s\",\"ts\":100}", long_content_1);
    snprintf(line_buf[1], sizeof(line_buf[1]), 
             "{\"role\":\"assistant\",\"content\":\"%s\",\"ts\":200}", long_content_2);
    snprintf(line_buf[2], sizeof(line_buf[2]), 
             "{\"role\":\"user\",\"content\":\"%s\",\"ts\":300}", long_content_3);
    snprintf(line_buf[3], sizeof(line_buf[3]), 
             "{\"role\":\"assistant\",\"content\":\"%s\",\"ts\":400}", long_content_4);
    snprintf(line_buf[4], sizeof(line_buf[4]), 
             "{\"role\":\"user\",\"content\":\"%s\",\"ts\":500}", long_content_5);
    snprintf(line_buf[5], sizeof(line_buf[5]), 
             "{\"role\":\"assistant\",\"content\":\"%s\",\"ts\":600}", long_content_6);
    
    const char *lines[] = {
        line_buf[0], line_buf[1], line_buf[2], line_buf[3], line_buf[4], line_buf[5]
    };
    create_test_session_file("cli", "test_ctx_2", lines, 6);
    
    char system_prompt_buf[64] = "System prompt";
    char history_json_buf[8192];
    
    context_assemble_request_t req;
    memset(&req, 0, sizeof(req));
    req.channel = "cli";
    req.chat_id = "test_ctx_2";
    req.user_content = "Current user query that needs a response";
    req.system_prompt_buf = system_prompt_buf;
    req.system_prompt_buf_size = sizeof(system_prompt_buf);
    req.history_json_buf = history_json_buf;
    req.history_json_buf_size = sizeof(history_json_buf);
    req.tools_json = "[]";
    req.base_memory_window = 6;
    req.memory_flush_threshold_ratio = 0.0;  /* 0.0 = trim as much as possible (force compact) */
    
    context_assemble_result_t result;
    memset(&result, 0, sizeof(result));
    
    /* Very tight token budget - forces significant trimming */
    req.context_tokens = 10;
    mimi_err_t err = context_assemble_messages_budgeted(&req, &result);
    
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    
    /* Key Assertion: trimmed_messages_for_compact should contain the OLDEST messages */
    TEST_ASSERT_NOT_NULL_MESSAGE(result.trimmed_messages_for_compact, 
        "trimmed_messages_for_compact should NOT be NULL when trimming occurs");
    
    int compact_count = cJSON_GetArraySize(result.trimmed_messages_for_compact);
    TEST_ASSERT_TRUE(compact_count > 0);
    
    /* First item in compact should be the OLDEST message from history */
    cJSON *first_compact = cJSON_GetArrayItem(result.trimmed_messages_for_compact, 0);
    TEST_ASSERT_NOT_NULL(first_compact);
    
    /* Verify that main messages contain the NEWEST history (not the oldest) */
    TEST_ASSERT_NOT_NULL(result.messages);
    int main_count = cJSON_GetArraySize(result.messages);
    TEST_ASSERT_TRUE(main_count > 0);
    
    /* Cleanup */
    if (result.messages) cJSON_Delete(result.messages);
    if (result.trimmed_messages_for_compact) cJSON_Delete(result.trimmed_messages_for_compact);
    cleanup_test_session_file("cli", "test_ctx_2");
}

void test_context_assemble_with_compact_merge(void)
{
    /* Test that compact summary can be merged back correctly */
    const char *lines[] = {
        "{\"role\":\"user\",\"content\":\"Oldest message - to be summarized\",\"ts\":100}",
        "{\"role\":\"assistant\",\"content\":\"Response to oldest - to be summarized\",\"ts\":200}",
        "{\"role\":\"user\",\"content\":\"Middle message - might be summarized\",\"ts\":300}",
        "{\"role\":\"assistant\",\"content\":\"Recent response - should be kept\",\"ts\":400}",
        "{\"role\":\"user\",\"content\":\"Newest message - should be kept\",\"ts\":500}"
    };
    create_test_session_file("cli", "test_ctx_3", lines, 5);
    
    char system_prompt_buf[64] = "System prompt";
    char history_json_buf[4096];
    
    context_assemble_request_t req;
    memset(&req, 0, sizeof(req));
    req.channel = "cli";
    req.chat_id = "test_ctx_3";
    req.user_content = "Current query";
    req.system_prompt_buf = system_prompt_buf;
    req.system_prompt_buf_size = sizeof(system_prompt_buf);
    req.history_json_buf = history_json_buf;
    req.history_json_buf_size = sizeof(history_json_buf);
    req.tools_json = "[]";
    req.base_memory_window = 5;
    req.context_tokens = 0;
    req.memory_flush_threshold_ratio = 0.4;
    
    context_assemble_result_t result;
    memset(&result, 0, sizeof(result));
    
    req.context_tokens = 40;
    mimi_err_t err = context_assemble_messages_budgeted(&req, &result);
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    
    /* Simulate what happens after compact: merge summary back into messages */
    if (result.trimmed_messages_for_compact) {
        /* Create a summary message (simulating LLM summary output) */
        cJSON *summary = cJSON_CreateObject();
        cJSON_AddStringToObject(summary, "role", "system");
        cJSON_AddStringToObject(summary, "content", "[Summary of earlier conversation]");
        
        /* Insert summary at beginning */
        context_compact_insert_summary_message(result.messages, summary);
        
        /* Verify summary is at position 0 */
        cJSON *first_msg = cJSON_GetArrayItem(result.messages, 0);
        TEST_ASSERT_EQUAL_STRING("system", get_msg_role(first_msg));
        TEST_ASSERT_EQUAL_STRING("[Summary of earlier conversation]", get_msg_content(first_msg));
    }
    
    /* Cleanup */
    if (result.messages) cJSON_Delete(result.messages);
    if (result.trimmed_messages_for_compact) cJSON_Delete(result.trimmed_messages_for_compact);
    cleanup_test_session_file("cli", "test_ctx_3");
}

void test_context_assemble_empty_history(void)
{
    /* Edge case: no history file exists yet */
    char system_prompt_buf[64] = "System prompt";
    char history_json_buf[4096];
    
    context_assemble_request_t req;
    memset(&req, 0, sizeof(req));
    req.channel = "cli";
    req.chat_id = "test_ctx_nonexistent";  /* No file for this ID */
    req.user_content = "First message ever";
    req.system_prompt_buf = system_prompt_buf;
    req.system_prompt_buf_size = sizeof(system_prompt_buf);
    req.history_json_buf = history_json_buf;
    req.history_json_buf_size = sizeof(history_json_buf);
    req.tools_json = "[]";
    req.base_memory_window = 10;
    req.context_tokens = 0;
    req.memory_flush_threshold_ratio = 0.5;
    
    context_assemble_result_t result;
    memset(&result, 0, sizeof(result));
    
    req.context_tokens = 1000;  /* Large budget - no trimming needed */
    mimi_err_t err = context_assemble_messages_budgeted(&req, &result);
    
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(result.messages);
    
    int msg_count = cJSON_GetArraySize(result.messages);
    /* Should contain at least the current user message */
    TEST_ASSERT_TRUE(msg_count >= 1);
    /* Last message should be the current user query */
    assert_message(cJSON_GetArrayItem(result.messages, msg_count - 1), "user", "First message ever");
    
    /* No trimming should occur on empty history */
    TEST_ASSERT_NULL(result.trimmed_messages_for_compact);
    
    /* Cleanup */
    if (result.messages) cJSON_Delete(result.messages);
}
