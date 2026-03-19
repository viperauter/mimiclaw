#include "unity.h"
#include "tools/tool_registry.h"
#include "tools/tool_files.h"
#include "fs/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

void test_registry_init(void)
{
    const char *tools_json = tool_registry_get_tools_json();
    TEST_ASSERT_NOT_NULL(tools_json);
    TEST_ASSERT_TRUE(strstr(tools_json, "write_file") != NULL);
    TEST_ASSERT_TRUE(strstr(tools_json, "read_file") != NULL);
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

    f = fopen(filepath, "r");
    TEST_ASSERT_NOT_NULL(f);
    if (f) {
        char content[128] = {0};
        fread(content, 1, sizeof(content) - 1, f);
        fclose(f);
        TEST_ASSERT_EQUAL_STRING("hello universe", content);
    }
}

void test_registry_execute_list_dir(void)
{
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/file1.txt", test_dir);
    fclose(fopen(filepath, "w"));

    snprintf(filepath, sizeof(filepath), "%s/file2.txt", test_dir);
    fclose(fopen(filepath, "w"));

    const char *input = "{}";
    char output[2048] = {0};

    mimi_err_t err = tool_registry_execute("list_dir", input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_TRUE(strstr(output, "file1.txt") != NULL);
    TEST_ASSERT_TRUE(strstr(output, "file2.txt") != NULL);
}

void test_registry_unknown_tool(void)
{
    const char *input = "{}";
    char output[1024] = {0};

    mimi_err_t err = tool_registry_execute("nonexistent_tool", input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_ERR_NOT_FOUND, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "unknown tool"));
}

void test_registry_requires_confirmation(void)
{
    TEST_ASSERT_FALSE(tool_registry_requires_confirmation("read_file"));
    TEST_ASSERT_TRUE(tool_registry_requires_confirmation("write_file"));
    TEST_ASSERT_TRUE(tool_registry_requires_confirmation("edit_file"));
    TEST_ASSERT_TRUE(tool_registry_requires_confirmation("exec"));
    TEST_ASSERT_FALSE(tool_registry_requires_confirmation("unknown"));
}

