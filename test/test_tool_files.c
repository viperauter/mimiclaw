#include "unity.h"
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

    /* VFS needs explicit init + POSIX backend registration */
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

void test_write_file_basic(void)
{
    const char *input = "{\"path\": \"test.txt\", \"content\": \"hello world\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_write_file_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "OK"));
    TEST_ASSERT_NOT_NULL(strstr(output, "11 bytes"));

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/test.txt", test_dir);
    FILE *f = fopen(filepath, "r");
    TEST_ASSERT_NOT_NULL(f);
    if (f) {
        char content[100] = {0};
        fread(content, 1, sizeof(content), f);
        fclose(f);
        TEST_ASSERT_EQUAL_STRING("hello world", content);
    }
}

void test_write_file_creates_parent_directory(void)
{
    const char *input = "{\"path\": \"subdir/nested/test.txt\", \"content\": \"nested content\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_write_file_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/subdir/nested/test.txt", test_dir);
    FILE *f = fopen(filepath, "r");
    TEST_ASSERT_NOT_NULL(f);
    if (f) {
        fclose(f);
    }
}

void test_write_file_rejects_path_traversal(void)
{
    const char *input = "{\"path\": \"../escape.txt\", \"content\": \"escape attempt\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_write_file_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_ERR_INVALID_ARG, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "'..'"));
}

void test_write_file_missing_content(void)
{
    const char *input = "{\"path\": \"test.txt\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_write_file_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_ERR_INVALID_ARG, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "missing"));
}

void test_write_file_invalid_json(void)
{
    const char *input = "not valid json";
    char output[1024] = {0};

    mimi_err_t err = tool_write_file_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_ERR_INVALID_ARG, err);
}

void test_read_file_basic(void)
{
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/read_test.txt", test_dir);
    FILE *f = fopen(filepath, "w");
    TEST_ASSERT_NOT_NULL(f);
    if (f) {
        fprintf(f, "test content for reading");
        fclose(f);
    }

    const char *input = "{\"path\": \"read_test.txt\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_read_file_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_EQUAL_STRING("test content for reading", output);
}

void test_read_file_not_found(void)
{
    const char *input = "{\"path\": \"nonexistent.txt\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_read_file_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_ERR_NOT_FOUND, err);
}

void test_read_file_rejects_path_traversal(void)
{
    const char *input = "{\"path\": \"../etc/passwd\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_read_file_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_ERR_INVALID_ARG, err);
}

void test_edit_file_basic(void)
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

    mimi_err_t err = tool_edit_file_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "OK"));

    f = fopen(filepath, "r");
    TEST_ASSERT_NOT_NULL(f);
    if (f) {
        char content[100] = {0};
        fread(content, 1, sizeof(content), f);
        fclose(f);
        TEST_ASSERT_EQUAL_STRING("hello universe", content);
    }
}

void test_edit_file_old_string_not_found(void)
{
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/edit_test2.txt", test_dir);
    FILE *f = fopen(filepath, "w");
    TEST_ASSERT_NOT_NULL(f);
    if (f) {
        fprintf(f, "hello world");
        fclose(f);
    }

    const char *input =
        "{\"path\": \"edit_test2.txt\", \"old_string\": \"nonexistent\", \"new_string\": \"replacement\"}";
    char output[1024] = {0};

    mimi_err_t err = tool_edit_file_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_ERR_NOT_FOUND, err);
}

void test_list_dir_basic(void)
{
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/file1.txt", test_dir);
    FILE *f = fopen(filepath, "w");
    if (f) fclose(f);

    snprintf(filepath, sizeof(filepath), "%s/file2.txt", test_dir);
    f = fopen(filepath, "w");
    if (f) fclose(f);

    const char *input = "{}";
    char output[2048] = {0};

    mimi_err_t err = tool_list_dir_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "file1.txt"));
    TEST_ASSERT_NOT_NULL(strstr(output, "file2.txt"));
}

void test_list_dir_with_prefix(void)
{
    char filepath[512];

    snprintf(filepath, sizeof(filepath), "%s/mem_file1.txt", test_dir);
    FILE *f = fopen(filepath, "w");
    if (f) fclose(f);

    snprintf(filepath, sizeof(filepath), "%s/mem_file2.txt", test_dir);
    f = fopen(filepath, "w");
    if (f) fclose(f);

    snprintf(filepath, sizeof(filepath), "%s/other.txt", test_dir);
    f = fopen(filepath, "w");
    if (f) fclose(f);

    const char *input = "{\"prefix\": \"mem_\"}";
    char output[2048] = {0};

    mimi_err_t err = tool_list_dir_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_NOT_NULL(strstr(output, "mem_file1.txt"));
    TEST_ASSERT_NOT_NULL(strstr(output, "mem_file2.txt"));
    TEST_ASSERT_NULL(strstr(output, "other.txt"));
}

void test_absolute_path_passthrough(void)
{
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "/tmp/mimiclaw_abs_test_%d.txt", getpid());
    FILE *f = fopen(filepath, "w");
    TEST_ASSERT_NOT_NULL(f);
    if (f) {
        fprintf(f, "absolute path test");
        fclose(f);
    }

    char input[512];
    snprintf(input, sizeof(input), "{\"path\": \"%s\"}", filepath);
    char output[1024] = {0};

    mimi_err_t err = tool_read_file_execute(input, output, sizeof(output), &session_ctx);

    TEST_ASSERT_EQUAL_INT(MIMI_OK, err);
    TEST_ASSERT_EQUAL_STRING("absolute path test", output);

    unlink(filepath);
}

