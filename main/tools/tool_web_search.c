#include "tools/tool_web_search.h"
#include "mimi_config.h"

#include "platform/http.h"
#include "platform/log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cJSON.h"

static const char *TAG = "web_search_posix";

static char s_search_key[128] = {0};

#define SEARCH_RESULT_COUNT 5

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; *src && pos < dst_size - 3; src++) {
        unsigned char c = (unsigned char) *src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

static void format_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *web = cJSON_GetObjectItem(root, "web");
    if (!web) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    cJSON *results = cJSON_GetObjectItem(web, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT || off >= output_size - 1) break;

        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *url = cJSON_GetObjectItem(item, "url");
        cJSON *desc = cJSON_GetObjectItem(item, "description");

        off += snprintf(output + off, output_size - off,
                        "%d. %s\n   %s\n   %s\n\n",
                        idx + 1,
                        (title && cJSON_IsString(title)) ? title->valuestring : "(no title)",
                        (url && cJSON_IsString(url)) ? url->valuestring : "",
                        (desc && cJSON_IsString(desc)) ? desc->valuestring : "");
        idx++;
    }
}

mimi_err_t tool_web_search_init(void)
{
    if (MIMI_SECRET_SEARCH_KEY[0] != '\0') {
        strncpy(s_search_key, MIMI_SECRET_SEARCH_KEY, sizeof(s_search_key) - 1);
    }

    if (s_search_key[0]) {
        MIMI_LOGI(TAG, "Web search initialized (key configured)");
    } else {
        MIMI_LOGW(TAG, "No search API key. Set MIMI_SECRET_SEARCH_KEY in mimi_secrets.h");
    }
    return MIMI_OK;
}

mimi_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_search_key[0] == '\0') {
        snprintf(output, output_size, "Error: No search API key configured. Set MIMI_SECRET_SEARCH_KEY in mimi_secrets.h");
        return MIMI_ERR_INVALID_STATE;
    }

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return MIMI_ERR_INVALID_ARG;
    }

    cJSON *query = cJSON_GetObjectItem(input, "query");
    if (!query || !cJSON_IsString(query) || query->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return MIMI_ERR_INVALID_ARG;
    }

    MIMI_LOGI(TAG, "Searching: %s", query->valuestring);

    char encoded_query[256];
    url_encode(query->valuestring, encoded_query, sizeof(encoded_query));
    cJSON_Delete(input);

    char url[512];
    snprintf(url, sizeof(url),
             "https://api.search.brave.com/res/v1/web/search?q=%s&count=%d",
             encoded_query, SEARCH_RESULT_COUNT);

    char headers[256];
    snprintf(headers, sizeof(headers),
             "Accept: application/json\r\n"
             "X-Subscription-Token: %s\r\n",
             s_search_key);

    mimi_http_request_t req = {
        .method = "GET",
        .url = url,
        .headers = headers,
        .body = NULL,
        .body_len = 0,
        .timeout_ms = 15000,
    };
    mimi_http_response_t resp;
    mimi_err_t err = mimi_http_exec(&req, &resp);
    if (err != MIMI_OK) {
        snprintf(output, output_size, "Error: Search request failed (%s)", mimi_err_to_name(err));
        return err;
    }

    if (resp.status != 200 || !resp.body) {
        snprintf(output, output_size, "Error: Search API returned %d", resp.status);
        mimi_http_response_free(&resp);
        return MIMI_ERR_FAIL;
    }

    cJSON *root = cJSON_Parse((char *) resp.body);
    mimi_http_response_free(&resp);
    if (!root) {
        snprintf(output, output_size, "Error: Failed to parse search results");
        return MIMI_ERR_FAIL;
    }

    format_results(root, output, output_size);
    cJSON_Delete(root);

    MIMI_LOGI(TAG, "Search complete, %d bytes result", (int) strlen(output));
    return MIMI_OK;
}

mimi_err_t tool_web_search_set_key(const char *api_key)
{
    strncpy(s_search_key, api_key, sizeof(s_search_key) - 1);
    MIMI_LOGI(TAG, "Search API key updated (POSIX, not persisted)");
    return MIMI_OK;
}

