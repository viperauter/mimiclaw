#include "llm/llm_proxy.h"
#include "config.h"

#include "http/http.h"
#include "log.h"
#include "mimi_err.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "llm_posix";

#define LLM_API_KEY_MAX_LEN 320
#define LLM_MODEL_MAX_LEN   64
#define LLM_DUMP_MAX_BYTES   (16 * 1024)
#define LLM_DUMP_CHUNK_BYTES 320

#define LLM_LOG_VERBOSE_PAYLOAD 0
#define LLM_LOG_PREVIEW_BYTES 160

static const char *ANTHROPIC_API_URL  = "https://api.anthropic.com/v1/messages";
static const char *OPENAI_API_URL     = "https://api.openai.com/v1/chat/completions";
static const char *OPENROUTER_API_URL = "https://openrouter.ai/api/v1/chat/completions";
static const char *ANTHROPIC_VERSION  = "2023-06-01";

static char s_api_key[LLM_API_KEY_MAX_LEN] = {0};
static char s_model[LLM_MODEL_MAX_LEN] = {0};
static char s_provider[16] = {0};
static char s_api_override[256] = {0}; /* cfg.apiUrl or cfg.api_base */

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void llm_log_payload(const char *label, const char *payload)
{
    if (!payload) {
        MIMI_LOGI(TAG, "%s: <null>", label);
        return;
    }

    size_t total = strlen(payload);
#if LLM_LOG_VERBOSE_PAYLOAD
    size_t shown = total > LLM_DUMP_MAX_BYTES ? LLM_DUMP_MAX_BYTES : total;
    MIMI_LOGI(TAG, "%s (%u bytes)%s",
              label,
              (unsigned) total,
              (shown < total) ? " [truncated]" : "");

    char chunk[LLM_DUMP_CHUNK_BYTES + 1];
    for (size_t off = 0; off < shown; off += LLM_DUMP_CHUNK_BYTES) {
        size_t n = shown - off;
        if (n > LLM_DUMP_CHUNK_BYTES) n = LLM_DUMP_CHUNK_BYTES;
        memcpy(chunk, payload + off, n);
        chunk[n] = '\0';
        MIMI_LOGI(TAG, "%s[%u]: %s", label, (unsigned) off, chunk);
    }
#else
    if (LLM_LOG_PREVIEW_BYTES > 0) {
        size_t shown = total > (size_t)LLM_LOG_PREVIEW_BYTES ? (size_t)LLM_LOG_PREVIEW_BYTES : total;
        char preview[LLM_LOG_PREVIEW_BYTES + 1];
        memcpy(preview, payload, shown);
        preview[shown] = '\0';
        for (size_t i = 0; i < shown; i++) {
            if (preview[i] == '\n' || preview[i] == '\r' || preview[i] == '\t') {
                preview[i] = ' ';
            }
        }
        MIMI_LOGI(TAG, "%s (%u bytes): %s%s",
                  label,
                  (unsigned) total,
                  preview,
                  (shown < total) ? " ..." : "");
    } else {
        MIMI_LOGI(TAG, "%s (%u bytes)", label, (unsigned) total);
    }
#endif
}

static bool provider_is_openai(void)
{
    return strcmp(s_provider, "openai") == 0;
}

static bool provider_is_openrouter(void)
{
    return strcmp(s_provider, "openrouter") == 0;
}

static const char *llm_api_url(void)
{
    if (s_api_override[0] != '\0') return s_api_override;
    if (provider_is_openrouter()) return OPENROUTER_API_URL;
    if (provider_is_openai()) return OPENAI_API_URL;
    return ANTHROPIC_API_URL;
}

/* ── Init & setters (POSIX: in-memory only) ───────────────────── */

mimi_err_t llm_proxy_init(void)
{
    const mimi_config_t *cfg = mimi_config_get();
    if (cfg->api_key[0] != '\0') {
        safe_copy(s_api_key, sizeof(s_api_key), cfg->api_key);
    }
    if (cfg->model[0] != '\0') {
        safe_copy(s_model, sizeof(s_model), cfg->model);
    }
    if (cfg->provider[0] != '\0') {
        safe_copy(s_provider, sizeof(s_provider), cfg->provider);
    }
    if (cfg->api_url[0] != '\0') {
        safe_copy(s_api_override, sizeof(s_api_override), cfg->api_url);
    } else if (cfg->api_base[0] != '\0') {
        safe_copy(s_api_override, sizeof(s_api_override), cfg->api_base);
    }

    if (s_api_key[0]) {
        MIMI_LOGI(TAG, "LLM proxy initialized (provider=%s, model=%s)", s_provider, s_model);
    } else {
        MIMI_LOGW(TAG, "No API key configured. Set in config.json (providers.*.apiKey)");
    }
    return MIMI_OK;
}

mimi_err_t llm_set_api_key(const char *api_key)
{
    safe_copy(s_api_key, sizeof(s_api_key), api_key);
    MIMI_LOGI(TAG, "API key updated (POSIX, not persisted)");
    return MIMI_OK;
}

mimi_err_t llm_set_model(const char *model)
{
    safe_copy(s_model, sizeof(s_model), model);
    MIMI_LOGI(TAG, "Model set to: %s", s_model);
    return MIMI_OK;
}

mimi_err_t llm_set_provider(const char *provider)
{
    safe_copy(s_provider, sizeof(s_provider), provider);
    MIMI_LOGI(TAG, "Provider set to: %s", s_provider);
    return MIMI_OK;
}

/* ── Public: chat with tools (non-streaming) ──────────────────── */

void llm_response_free(llm_response_t *resp)
{
    if (!resp) return;
    free(resp->text);
    resp->text = NULL;
    resp->text_len = 0;
    for (int i = 0; i < resp->call_count; i++) {
        free(resp->calls[i].input);
        resp->calls[i].input = NULL;
        resp->calls[i].input_len = 0;
    }
    resp->call_count = 0;
    resp->tool_use = false;
}

/* Helpers copied from ESP implementation, trimmed for POSIX HTTP */
static cJSON *convert_tools_openai(const char *tools_json);
static cJSON *convert_messages_openai(const char *system_prompt, cJSON *messages);

mimi_err_t llm_chat_tools(const char *system_prompt,
                          cJSON *messages,
                          const char *tools_json,
                          llm_response_t *resp)
{
    if (!resp) return MIMI_ERR_INVALID_ARG;
    memset(resp, 0, sizeof(*resp));

    if (s_api_key[0] == '\0') return MIMI_ERR_INVALID_STATE;

    const mimi_config_t *cfg = mimi_config_get();
    int max_tokens = (cfg->max_tokens > 0) ? cfg->max_tokens : 8192;
    double temperature = cfg->temperature;

    cJSON *body = cJSON_CreateObject();
    if (!body) return MIMI_ERR_NO_MEM;

    cJSON_AddStringToObject(body, "model", s_model);
    if (provider_is_openai() || provider_is_openrouter()) {
        cJSON_AddNumberToObject(body, "max_completion_tokens", max_tokens);
        cJSON_AddNumberToObject(body, "temperature", temperature);
    } else {
        cJSON_AddNumberToObject(body, "max_tokens", max_tokens);
        cJSON_AddNumberToObject(body, "temperature", temperature);
    }

    if (provider_is_openai() || provider_is_openrouter()) {
        cJSON *openai_msgs = convert_messages_openai(system_prompt, messages);
        cJSON_AddItemToObject(body, "messages", openai_msgs);

        if (tools_json) {
            cJSON *tools = convert_tools_openai(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
                cJSON_AddStringToObject(body, "tool_choice", "auto");
            }
        }
    } else {
        cJSON_AddStringToObject(body, "system", system_prompt ? system_prompt : "");
        cJSON *msgs_copy = cJSON_Duplicate(messages, 1);
        if (msgs_copy) {
            cJSON_AddItemToObject(body, "messages", msgs_copy);
        }
        if (tools_json) {
            cJSON *tools = cJSON_Parse(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
            }
        }
    }

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return MIMI_ERR_NO_MEM;

    llm_log_payload("LLM tools request", post_data);

    /* Build HTTP request */
    char headers[LLM_API_KEY_MAX_LEN + 256];
    if (provider_is_openai() || provider_is_openrouter()) {
        char auth[LLM_API_KEY_MAX_LEN + 16];
        snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
        snprintf(headers, sizeof(headers),
                 "Content-Type: application/json\r\n"
                 "Authorization: %s\r\n",
                 auth);
    } else {
        snprintf(headers, sizeof(headers),
                 "Content-Type: application/json\r\n"
                 "x-api-key: %s\r\n"
                 "anthropic-version: %s\r\n",
                 s_api_key, ANTHROPIC_VERSION);
    }

    mimi_http_request_t req = {
        .method = "POST",
        .url = llm_api_url(),
        .headers = headers,
        .body = (const uint8_t *) post_data,
        .body_len = strlen(post_data),
        .timeout_ms = 120000,
    };
    mimi_http_response_t hresp;
    mimi_err_t herr = mimi_http_exec(&req, &hresp);
    free(post_data);

    if (herr != MIMI_OK) {
        MIMI_LOGE(TAG, "HTTP request failed: %s", mimi_err_to_name(herr));
        return herr;
    }

    if (hresp.status != 200 || !hresp.body) {
        MIMI_LOGE(TAG, "LLM API error %d: %.500s",
                  hresp.status,
                  hresp.body ? (char *) hresp.body : "");
        mimi_http_response_free(&hresp);
        return MIMI_ERR_FAIL;
    }

    llm_log_payload("LLM tools raw response", (char *) hresp.body);

    cJSON *root = cJSON_Parse((char *) hresp.body);
    mimi_http_response_free(&hresp);
    if (!root) {
        MIMI_LOGE(TAG, "Failed to parse API response JSON");
        return MIMI_ERR_FAIL;
    }

    /* Parsing logic largely mirrors original ESP implementation */
    if (provider_is_openai() || provider_is_openrouter()) {
        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *choice0 = choices && cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        if (choice0) {
            cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
            if (finish && cJSON_IsString(finish)) {
                resp->tool_use = (strcmp(finish->valuestring, "tool_calls") == 0);
            }

            cJSON *message = cJSON_GetObjectItem(choice0, "message");
            if (message) {
                cJSON *content = cJSON_GetObjectItem(message, "content");
                if (content && cJSON_IsString(content)) {
                    size_t tlen = strlen(content->valuestring);
                    resp->text = calloc(1, tlen + 1);
                    if (resp->text) {
                        memcpy(resp->text, content->valuestring, tlen);
                        resp->text_len = tlen;
                    }
                } else {
                    /* OpenRouter (and some providers) may put text in non-standard fields like \"reasoning\". */
                    cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning");
                    if (reasoning && cJSON_IsString(reasoning)) {
                        size_t tlen = strlen(reasoning->valuestring);
                        resp->text = calloc(1, tlen + 1);
                        if (resp->text) {
                            memcpy(resp->text, reasoning->valuestring, tlen);
                            resp->text_len = tlen;
                        }
                    }
                }

                cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
                if (tool_calls && cJSON_IsArray(tool_calls)) {
                    cJSON *tc;
                    cJSON_ArrayForEach(tc, tool_calls) {
                        if (resp->call_count >= LLM_MAX_TOOL_CALLS) break;
                        llm_tool_call_t *call = &resp->calls[resp->call_count];
                        cJSON *id = cJSON_GetObjectItem(tc, "id");
                        cJSON *func = cJSON_GetObjectItem(tc, "function");
                        if (id && cJSON_IsString(id)) {
                            strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
                        }
                        if (func) {
                            cJSON *name = cJSON_GetObjectItem(func, "name");
                            cJSON *args = cJSON_GetObjectItem(func, "arguments");
                            if (name && cJSON_IsString(name)) {
                                strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                            }
                            if (args && cJSON_IsString(args)) {
                                call->input = strdup(args->valuestring);
                                if (call->input) call->input_len = strlen(call->input);
                            }
                        }
                        resp->call_count++;
                    }
                    if (resp->call_count > 0) {
                        resp->tool_use = true;
                    }
                }
            }
        }
    } else {
        cJSON *stop_reason = cJSON_GetObjectItem(root, "stop_reason");
        if (stop_reason && cJSON_IsString(stop_reason)) {
            resp->tool_use = (strcmp(stop_reason->valuestring, "tool_use") == 0);
        }

        cJSON *content = cJSON_GetObjectItem(root, "content");
        if (content && cJSON_IsArray(content)) {
            size_t total_text = 0;
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        total_text += strlen(text->valuestring);
                    }
                }
            }
            if (total_text > 0) {
                resp->text = calloc(1, total_text + 1);
                if (resp->text) {
                    cJSON_ArrayForEach(block, content) {
                        cJSON *btype = cJSON_GetObjectItem(block, "type");
                        if (!btype || strcmp(btype->valuestring, "text") != 0) continue;
                        cJSON *text = cJSON_GetObjectItem(block, "text");
                        if (!text || !cJSON_IsString(text)) continue;
                        size_t tlen = strlen(text->valuestring);
                        memcpy(resp->text + resp->text_len, text->valuestring, tlen);
                        resp->text_len += tlen;
                    }
                    resp->text[resp->text_len] = '\0';
                }
            }

            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (!btype || strcmp(btype->valuestring, "tool_use") != 0) continue;
                if (resp->call_count >= LLM_MAX_TOOL_CALLS) break;

                llm_tool_call_t *call = &resp->calls[resp->call_count];

                cJSON *id = cJSON_GetObjectItem(block, "id");
                if (id && cJSON_IsString(id)) {
                    strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
                }

                cJSON *name = cJSON_GetObjectItem(block, "name");
                if (name && cJSON_IsString(name)) {
                    strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                }

                cJSON *input = cJSON_GetObjectItem(block, "input");
                if (input) {
                    char *input_str = cJSON_PrintUnformatted(input);
                    if (input_str) {
                        call->input = input_str;
                        call->input_len = strlen(input_str);
                    }
                }

                resp->call_count++;
            }
        }
    }

    cJSON_Delete(root);

    MIMI_LOGI(TAG, "Response: %d bytes text, %d tool calls, stop=%s",
              (int) resp->text_len, resp->call_count,
              resp->tool_use ? "tool_use" : "end_turn");

    return MIMI_OK;
}

/* Minimal copies of conversion helpers (same as ESP version) */

static cJSON *convert_tools_openai(const char *tools_json)
{
    if (!tools_json) return NULL;
    cJSON *arr = cJSON_Parse(tools_json);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON *out = cJSON_CreateArray();
    cJSON *tool;
    cJSON_ArrayForEach(tool, arr) {
        cJSON *name = cJSON_GetObjectItem(tool, "name");
        cJSON *desc = cJSON_GetObjectItem(tool, "description");
        cJSON *schema = cJSON_GetObjectItem(tool, "input_schema");
        if (!name || !cJSON_IsString(name)) continue;

        cJSON *func = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", name->valuestring);
        if (desc && cJSON_IsString(desc)) {
            cJSON_AddStringToObject(func, "description", desc->valuestring);
        }
        if (schema) {
            cJSON_AddItemToObject(func, "parameters", cJSON_Duplicate(schema, 1));
        }

        cJSON *wrap = cJSON_CreateObject();
        cJSON_AddStringToObject(wrap, "type", "function");
        cJSON_AddItemToObject(wrap, "function", func);
        cJSON_AddItemToArray(out, wrap);
    }
    cJSON_Delete(arr);
    return out;
}

static cJSON *convert_messages_openai(const char *system_prompt, cJSON *messages)
{
    cJSON *out = cJSON_CreateArray();
    if (system_prompt && system_prompt[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(out, sys);
    }

    if (!messages || !cJSON_IsArray(messages)) return out;

    cJSON *msg;
    cJSON_ArrayForEach(msg, messages) {
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (!role || !cJSON_IsString(role)) continue;

        if (content && cJSON_IsString(content)) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", role->valuestring);
            cJSON_AddStringToObject(m, "content", content->valuestring);
            cJSON_AddItemToArray(out, m);
            continue;
        }
    }
    return out;
}

