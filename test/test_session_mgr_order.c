#include "unity.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

/* Test session_mgr.c using real platform layer (POSIX file system).
 * This is a semi-integration test that verifies the message ordering is correct.
 */

/* Pull in the real implementation */
TEST_SOURCE_FILE("main/memory/session_mgr.c")

/* Dependencies will be linked from project support files.
 * We only need to provide minimal stubs for functions that the test runner doesn't provide.
 */

#include "memory/session_mgr.h"
#include "config/config_view.h"
#include "fs/fs.h"

/* Config stubs - minimal implementation */
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

/* Forward declarations for FS initialization */
void posix_fs_register(void);
extern mimi_err_t mimi_fs_workspace_activate(const char *name);

/* -------------------------------------------------------------------------- */
/* Test helper to create a test session file with given messages             */
/* -------------------------------------------------------------------------- */

static void create_test_session_file(const char *channel, const char *chat_id, const char **lines, int num_lines)
{
    char path[256];
    snprintf(path, sizeof(path), "sessions/%s_%s.jsonl", channel, chat_id);
    
    /* Ensure directory exists */
    mkdir("sessions", 0755);
    
    /* Remove existing file if any */
    unlink(path);
    
    /* Write lines to file using standard C file IO (not mimi_fs) */
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
/* Test setup/teardown                                                       */
/* -------------------------------------------------------------------------- */

void setUp(void)
{
    /* Initialize real FS - required for mimi_fs_* functions to work */
    mkdir("sessions", 0755);
    
    /* Initialize VFS layer */
    mimi_fs_init();
    posix_fs_register();
    
    /* Create and activate a workspace with current directory as base */
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        mimi_fs_workspace_create("test", cwd);
        mimi_fs_workspace_activate("test");
    }
}

void tearDown(void)
{
    /* Clean up any test files */
    system("rm -f sessions/test_*.jsonl 2>/dev/null");
}

/* -------------------------------------------------------------------------- */
/* Test cases - verify message ordering from session_get_history_json        */
/* -------------------------------------------------------------------------- */

void test_session_get_history_json_returns_oldest_first_order(void)
{
    /* JSONL file: lines are written chronologically (oldest first in file).
     * Line 1 = msg1 (oldest), Line 2 = msg2, Line 3 = msg3 (newest).
     */
    const char *lines[] = {
        "{\"role\":\"user\",\"content\":\"msg1_oldest\",\"ts\":100}",
        "{\"role\":\"assistant\",\"content\":\"msg2_middle\",\"ts\":200}",
        "{\"role\":\"user\",\"content\":\"msg3_newest\",\"ts\":300}"
    };
    create_test_session_file("cli", "test_order_1", lines, 3);
    
    char buf[1024];
    mimi_err_t err = session_get_history_json("cli", "test_order_1", buf, sizeof(buf), 10);
    
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    
    /* Parse and verify order */
    cJSON *arr = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_TRUE(cJSON_IsArray(arr));
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(arr));
    
    /* KEY ASSERTION: JSON array should be [OLDEST, ..., NEWEST] */
    cJSON *m0 = cJSON_GetArrayItem(arr, 0);
    cJSON *m1 = cJSON_GetArrayItem(arr, 1);
    cJSON *m2 = cJSON_GetArrayItem(arr, 2);
    
    TEST_ASSERT_EQUAL_STRING("msg1_oldest", cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")));
    TEST_ASSERT_EQUAL_STRING("msg2_middle", cJSON_GetStringValue(cJSON_GetObjectItem(m1, "content")));
    TEST_ASSERT_EQUAL_STRING("msg3_newest", cJSON_GetStringValue(cJSON_GetObjectItem(m2, "content")));
    
    cJSON_Delete(arr);
    cleanup_test_session_file("cli", "test_order_1");
}

void test_session_get_history_json_ring_buffer_keeps_newest_messages(void)
{
    /* File has 5 messages, but we ask for max 3 */
    const char *lines[] = {
        "{\"role\":\"user\",\"content\":\"msg1_evicted\",\"ts\":100}",   /* Oldest - will be evicted */
        "{\"role\":\"assistant\",\"content\":\"msg2_evicted\",\"ts\":200}", /* Oldest - will be evicted */
        "{\"role\":\"user\",\"content\":\"msg3_kept\",\"ts\":300}",
        "{\"role\":\"assistant\",\"content\":\"msg4_kept\",\"ts\":400}",
        "{\"role\":\"user\",\"content\":\"msg5_newest\",\"ts\":500}"       /* Newest */
    };
    create_test_session_file("cli", "test_order_2", lines, 5);
    
    char buf[1024];
    mimi_err_t err = session_get_history_json("cli", "test_order_2", buf, sizeof(buf), 3);
    
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    
    cJSON *arr = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_TRUE(cJSON_IsArray(arr));
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(arr));
    
    /* KEY ASSERTION: When ring buffer wraps, returns [msg3, msg4, msg5] - OLDEST of the kept ones first */
    cJSON *m0 = cJSON_GetArrayItem(arr, 0);
    cJSON *m1 = cJSON_GetArrayItem(arr, 1);
    cJSON *m2 = cJSON_GetArrayItem(arr, 2);
    
    TEST_ASSERT_EQUAL_STRING("msg3_kept", cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")));
    TEST_ASSERT_EQUAL_STRING("msg4_kept", cJSON_GetStringValue(cJSON_GetObjectItem(m1, "content")));
    TEST_ASSERT_EQUAL_STRING("msg5_newest", cJSON_GetStringValue(cJSON_GetObjectItem(m2, "content")));
    
    cJSON_Delete(arr);
    cleanup_test_session_file("cli", "test_order_2");
}

void test_session_get_history_json_exact_max_returns_all_in_order(void)
{
    /* File has exactly max_msgs messages */
    const char *lines[] = {
        "{\"role\":\"user\",\"content\":\"msg1\",\"ts\":100}",
        "{\"role\":\"assistant\",\"content\":\"msg2\",\"ts\":200}",
        "{\"role\":\"user\",\"content\":\"msg3\",\"ts\":300}",
        "{\"role\":\"assistant\",\"content\":\"msg4\",\"ts\":400}"
    };
    create_test_session_file("cli", "test_order_3", lines, 4);
    
    char buf[1024];
    mimi_err_t err = session_get_history_json("cli", "test_order_3", buf, sizeof(buf), 4);
    
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    
    cJSON *arr = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_TRUE(cJSON_IsArray(arr));
    TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(arr));
    
    /* Verify all messages present in OLDEST -> NEWEST order */
    TEST_ASSERT_EQUAL_STRING("msg1", cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "content")));
    TEST_ASSERT_EQUAL_STRING("msg2", cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 1), "content")));
    TEST_ASSERT_EQUAL_STRING("msg3", cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 2), "content")));
    TEST_ASSERT_EQUAL_STRING("msg4", cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 3), "content")));
    
    cJSON_Delete(arr);
    cleanup_test_session_file("cli", "test_order_3");
}

void test_session_get_history_json_single_slot_returns_newest_only(void)
{
    const char *lines[] = {
        "{\"role\":\"user\",\"content\":\"msg1_old\",\"ts\":100}",
        "{\"role\":\"assistant\",\"content\":\"msg2_old\",\"ts\":200}",
        "{\"role\":\"user\",\"content\":\"msg3_newest\",\"ts\":300}"
    };
    create_test_session_file("cli", "test_order_5", lines, 3);
    
    char buf[1024];
    mimi_err_t err = session_get_history_json("cli", "test_order_5", buf, sizeof(buf), 1);
    
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    
    cJSON *arr = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(arr);
    TEST_ASSERT_TRUE(cJSON_IsArray(arr));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(arr));
    
    /* With max_msgs=1, should only return the NEWEST message (msg3) */
    cJSON *m0 = cJSON_GetArrayItem(arr, 0);
    TEST_ASSERT_EQUAL_STRING("msg3_newest", cJSON_GetStringValue(cJSON_GetObjectItem(m0, "content")));
    
    cJSON_Delete(arr);
    cleanup_test_session_file("cli", "test_order_5");
}

void test_session_get_history_json_nonexistent_file_returns_empty_array(void)
{
    char buf[1024];
    mimi_err_t err = session_get_history_json("cli", "test_nonexistent", buf, sizeof(buf), 10);
    
    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_EQUAL_STRING("[]", buf);
}
