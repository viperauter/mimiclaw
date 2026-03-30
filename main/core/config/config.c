/*
 * Runtime config: load/save JSON (nanobot-style schema), built-in defaults.
 */

#include "config.h"
#include "config_internal.h"
#include "log.h"
#include "cJSON.h"
#include "fs/fs.h"
#include "path_utils.h"
#include "mimi_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#endif

static const char *TAG = "config";

static mimi_config_t s_config;
static bool s_loaded;
static cJSON *s_json_root; /* merged raw config JSON (source of truth for extensible sections) */
static char s_config_path[512] = {0}; /* last loaded config path - symmetry with mimi_config_load() */

static mimi_err_t write_text_file_vfs(const char *path, const char *data, size_t len)
{
    if (!path || path[0] == '\0' || (!data && len != 0)) return MIMI_ERR_INVALID_ARG;
    mimi_file_t *f = NULL;
    mimi_err_t err = mimi_fs_open(path, "w", &f);
    if (err != MIMI_OK) return err;

    size_t written = 0;
    if (len > 0) {
        err = mimi_fs_write(f, data, len, &written);
        if (err != MIMI_OK || written != len) {
            (void)mimi_fs_close(f);
            return (err != MIMI_OK) ? err : MIMI_ERR_IO;
        }
    }

    return mimi_fs_close(f);
}

/* Bump when the config JSON schema changes.
 * Loader will auto-merge missing keys and write back merged config. */
#define MIMI_CONFIG_SCHEMA_VERSION 5

static void safe_strcpy(char *dst, size_t dst_size, const char *src)
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

static void expand_tilde_inplace(char *path_buf, size_t path_buf_size)
{
    if (!path_buf || path_buf_size == 0) return;
    if (!(path_buf[0] == '~' && path_buf[1] == '/')) return;
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') return;

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s/%s", home, path_buf + 2);
    safe_strcpy(path_buf, path_buf_size, tmp);
}

static void apply_defaults(void)
{
    memset(&s_config, 0, sizeof(s_config));

    /* nanobot-style agent defaults */
    /* Use platform-appropriate default workspace */
    char default_workspace[512];
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || home[0] == '\0') {
        home = getenv("USERPROFILE");
    }
#endif
    if (home && home[0] != '\0') {
        char tmp[512];
        mimi_path_join(home, ".mimiclaw", tmp, sizeof(tmp));
        mimi_path_join(tmp, "workspace", default_workspace, sizeof(default_workspace));
    } else {
        char tmp[512];
        mimi_path_join(".", ".mimiclaw", tmp, sizeof(tmp));
        mimi_path_join(tmp, "workspace", default_workspace, sizeof(default_workspace));
    }
    safe_strcpy(s_config.workspace, sizeof(s_config.workspace), default_workspace);
    safe_strcpy(s_config.timezone, sizeof(s_config.timezone), "PST8PDT,M3.2.0,M11.1.0");
    s_config.max_tokens = 8192;
    s_config.temperature = 0.1;
    s_config.max_tool_iterations = 40;
    s_config.memory_window = 100;
    s_config.api_url[0] = '\0';
    s_config.send_working_status = true;

    /* LLM provider/model (resolved from providers.* by agents.defaults.provider) */
    safe_strcpy(s_config.provider, sizeof(s_config.provider), "openrouter");
    safe_strcpy(s_config.model, sizeof(s_config.model), "nvidia/nemotron-nano-9b-v2:free");
    s_config.api_key[0] = '\0';
    safe_strcpy(s_config.api_base, sizeof(s_config.api_base), "https://openrouter.ai/api/v1/chat/completions");
    /* Default protocol for the above provider is OpenAI-compatible. */
    safe_strcpy(s_config.api_protocol, sizeof(s_config.api_protocol), "openai");

    /* Telegram */
    s_config.telegram_enabled = false;
    s_config.telegram_token[0] = '\0';
    s_config.telegram_allow_from[0] = '\0';

    /* Feishu */
    s_config.feishu_enabled = false;
    s_config.feishu_app_id[0] = '\0';
    s_config.feishu_app_secret[0] = '\0';

    /* QQ */
    s_config.qq_enabled = false;
    s_config.qq_app_id[0] = '\0';
    s_config.qq_token[0] = '\0';

    /* WeChat */
    s_config.wechat_enabled = false;
    s_config.wechat_bot_token[0] = '\0';
    s_config.wechat_bot_id[0] = '\0';
    s_config.wechat_user_id[0] = '\0';

    /* Proxy */
    s_config.proxy_host[0] = '\0';
    s_config.proxy_port[0] = '\0';
    safe_strcpy(s_config.proxy_type, sizeof(s_config.proxy_type), "http");

    /* Tools */
    s_config.search_api_key[0] = '\0';
    s_config.search_enabled = false;

    /* Internal tunables */
    s_config.bus_queue_len = 16;
    s_config.ws_port = 18789;
    s_config.heartbeat_interval_ms = 30 * 60 * 1000;
    s_config.cron_check_interval_ms = 60 * 1000;

    /* Files/directories (treated as real paths; may be absolute or relative) */
    safe_strcpy(s_config.heartbeat_file, sizeof(s_config.heartbeat_file), MIMI_DEFAULT_HEARTBEAT_FILE);
    safe_strcpy(s_config.cron_file, sizeof(s_config.cron_file), MIMI_DEFAULT_CRON_FILE);
    safe_strcpy(s_config.memory_file, sizeof(s_config.memory_file), MIMI_DEFAULT_MEMORY_FILE);
    safe_strcpy(s_config.soul_file, sizeof(s_config.soul_file), MIMI_DEFAULT_SOUL_FILE);
    safe_strcpy(s_config.user_file, sizeof(s_config.user_file), MIMI_DEFAULT_USER_FILE);
    safe_strcpy(s_config.skills_prefix, sizeof(s_config.skills_prefix), MIMI_DEFAULT_SKILLS_PREFIX);
    safe_strcpy(s_config.session_dir, sizeof(s_config.session_dir), MIMI_DEFAULT_SESSION_DIR);

    /* Network
     * Leave empty by default so Mongoose uses system DNS.
     * Override via config.network.dnsServer or env MIMI_DNS_SERVER.
     */
    s_config.dns_server[0] = '\0';
    
    /* Logging */
    safe_strcpy(s_config.log_level, sizeof(s_config.log_level), "info");
    s_config.log_to_file = false;
    s_config.log_to_stderr = true;
    safe_strcpy(s_config.log_dir, sizeof(s_config.log_dir), "logs");
    safe_strcpy(s_config.log_file, sizeof(s_config.log_file), "mimiclaw.log");
    s_config.log_max_file_bytes = 5 * 1024 * 1024; /* 5MB */
    s_config.log_max_files = 3;

    /* Tracing (disabled by default) */
    s_config.llm_trace_enabled = false;
    safe_strcpy(s_config.llm_trace_dir, sizeof(s_config.llm_trace_dir), "logs/traces");
    s_config.llm_trace_max_file_bytes = 10 * 1024 * 1024; /* 10MB */
    s_config.llm_trace_max_field_bytes = 16 * 1024;       /* 16KB per field */

    /* Subagents: none by default */
    s_config.subagent_count = 0;
}

/* Copy string from cJSON; only if value is non-empty or allow_empty. */
static void json_str_to_buf(const cJSON *j, char *buf, size_t buf_size, bool allow_empty)
{
    if (!buf || buf_size == 0) return;
    const char *s = cJSON_GetStringValue(j);
    if (!s) return;
    if (!allow_empty && s[0] == '\0') return;
    safe_strcpy(buf, buf_size, s);
}

/* Keep first allowFrom entry for now. */
static void parse_allow_from(cJSON *arr, char *buf, size_t buf_size)
{
    if (!arr || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) return;
    cJSON *first = cJSON_GetArrayItem(arr, 0);
    const char *s = cJSON_GetStringValue(first);
    if (s) safe_strcpy(buf, buf_size, s);
}

static int json_get_int(const cJSON *obj, const char *key, int fallback)
{
    if (!obj || !cJSON_IsObject(obj) || !key) return fallback;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)obj, key);
    if (v && cJSON_IsNumber(v)) return (int)v->valuedouble;
    return fallback;
}

static int config_get_schema_version(const cJSON *root)
{
    int v = json_get_int(root, "schemaVersion", 0);
    if (v <= 0) v = json_get_int(root, "configVersion", 0);
    return v;
}

static bool json_any_missing_expected(const cJSON *expected, const cJSON *have)
{
    if (!expected) return false;

    if (cJSON_IsObject(expected)) {
        if (!have || !cJSON_IsObject(have)) return true;
        for (const cJSON *child = expected->child; child; child = child->next) {
            if (!child->string || child->string[0] == '\0') continue;
            const cJSON *h = cJSON_GetObjectItemCaseSensitive((cJSON *)have, child->string);
            if (!h) return true;
            if (json_any_missing_expected(child, h)) return true;
        }
        return false;
    }

    if (cJSON_IsArray(expected)) {
        return !(have && cJSON_IsArray(have));
    }

    return false;
}

static void json_merge_object_into(cJSON *dst_obj, const cJSON *src_obj)
{
    if (!dst_obj || !cJSON_IsObject(dst_obj) || !src_obj || !cJSON_IsObject(src_obj)) return;

    for (const cJSON *src_child = src_obj->child; src_child; src_child = src_child->next) {
        if (!src_child->string || src_child->string[0] == '\0') continue;

        cJSON *dst_child = cJSON_GetObjectItemCaseSensitive(dst_obj, src_child->string);
        if (!dst_child) {
            cJSON *dup = cJSON_Duplicate((cJSON *)src_child, 1);
            if (dup) cJSON_AddItemToObject(dst_obj, src_child->string, dup);
            continue;
        }

        if (cJSON_IsObject(dst_child) && cJSON_IsObject(src_child)) {
            json_merge_object_into(dst_child, src_child);
            continue;
        }

        cJSON *dup = cJSON_Duplicate((cJSON *)src_child, 1);
        if (dup) cJSON_ReplaceItemInObjectCaseSensitive(dst_obj, src_child->string, dup);
    }
}

static cJSON *config_build_json_full_from_config(const mimi_config_t *cfg, int schema_version)
{
    if (!cfg) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddNumberToObject(root, "schemaVersion", schema_version);

    cJSON *providers = cJSON_CreateObject();
    if (providers) {
        cJSON *prov = cJSON_CreateObject();
        if (prov) {
            cJSON_AddStringToObject(prov, "apiKey", cfg->api_key);
            cJSON_AddStringToObject(prov, "apiBase", cfg->api_base);
            cJSON_AddItemToObject(providers, cfg->provider[0] ? cfg->provider : "anthropic", prov);
        }
        cJSON_AddItemToObject(root, "providers", providers);
    }

    cJSON *agents = cJSON_CreateObject();
    cJSON *defaults = cJSON_CreateObject();
    if (agents && defaults) {
        cJSON_AddStringToObject(defaults, "workspace", cfg->workspace);
        cJSON_AddStringToObject(defaults, "timezone", cfg->timezone);
        cJSON_AddStringToObject(defaults, "model", cfg->model);
        cJSON_AddStringToObject(defaults, "provider", cfg->provider);
        cJSON_AddStringToObject(defaults, "apiProtocol", cfg->api_protocol);
        cJSON_AddNumberToObject(defaults, "maxTokens", cfg->max_tokens);
        cJSON_AddNumberToObject(defaults, "temperature", cfg->temperature);
        cJSON_AddNumberToObject(defaults, "maxToolIterations", cfg->max_tool_iterations);
        /* Preferred name for subagent iteration cap.
         * Backward compatible with maxToolIterations (used as fallback). */
        cJSON_AddNumberToObject(defaults, "defaultMaxIters", cfg->max_tool_iterations);
        cJSON_AddNumberToObject(defaults, "memoryWindow", cfg->memory_window);

        /* Context/token-budget (best-effort token->chars approximation) */
        cJSON_AddNumberToObject(defaults, "contextTokens", 100000);

        /* Context compaction/summary controls (wired into context assembler). */
        cJSON *compaction = cJSON_CreateObject();
        if (compaction) {
            cJSON_AddStringToObject(compaction, "model", "");
            cJSON *memoryFlush = cJSON_CreateObject();
            if (memoryFlush) {
                cJSON_AddNumberToObject(memoryFlush, "thresholdRatio", 0.75);
                cJSON_AddItemToObject(compaction, "memoryFlush", memoryFlush);
            }
            cJSON_AddItemToObject(defaults, "compaction", compaction);
        }

        cJSON_AddStringToObject(defaults, "apiUrl", cfg->api_url);
        cJSON_AddBoolToObject(defaults, "sendWorkingStatus", cfg->send_working_status);
        cJSON_AddItemToObject(agents, "defaults", defaults);
        cJSON_AddItemToObject(root, "agents", agents);
    }

    cJSON *channels = cJSON_CreateObject();
    if (channels) {
        cJSON *tg = cJSON_CreateObject();
        if (tg) {
            cJSON_AddBoolToObject(tg, "enabled", cfg->telegram_enabled);
            cJSON_AddStringToObject(tg, "token", cfg->telegram_token);
            cJSON *arr = cJSON_CreateArray();
            if (arr) {
                if (cfg->telegram_allow_from[0]) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(cfg->telegram_allow_from));
                }
                cJSON_AddItemToObject(tg, "allowFrom", arr);
            }
            cJSON_AddItemToObject(channels, "telegram", tg);
        }

        cJSON *feishu = cJSON_CreateObject();
        if (feishu) {
            cJSON_AddBoolToObject(feishu, "enabled", cfg->feishu_enabled);
            cJSON_AddStringToObject(feishu, "appId", cfg->feishu_app_id);
            cJSON_AddStringToObject(feishu, "appSecret", cfg->feishu_app_secret);
            cJSON_AddItemToObject(channels, "feishu", feishu);
        }

        cJSON *qq = cJSON_CreateObject();
        if (qq) {
            cJSON_AddBoolToObject(qq, "enabled", cfg->qq_enabled);
            cJSON_AddStringToObject(qq, "appId", cfg->qq_app_id);
            cJSON_AddStringToObject(qq, "secret", cfg->qq_token);
            cJSON_AddItemToObject(channels, "qq", qq);
        }

        cJSON *wechat = cJSON_CreateObject();
        if (wechat) {
            cJSON_AddBoolToObject(wechat, "enabled", cfg->wechat_enabled);
            cJSON_AddStringToObject(wechat, "bot_token", cfg->wechat_bot_token);
            cJSON_AddStringToObject(wechat, "bot_id", cfg->wechat_bot_id);
            cJSON_AddStringToObject(wechat, "user_id", cfg->wechat_user_id);
            cJSON_AddItemToObject(channels, "wechat", wechat);
        }

        cJSON_AddItemToObject(root, "channels", channels);
    }

    cJSON *proxy = cJSON_CreateObject();
    if (proxy) {
        cJSON_AddStringToObject(proxy, "host", cfg->proxy_host);
        cJSON_AddStringToObject(proxy, "port", cfg->proxy_port);
        cJSON_AddStringToObject(proxy, "type", cfg->proxy_type);
        cJSON_AddItemToObject(root, "proxy", proxy);
    }

    cJSON *tools = cJSON_CreateObject();
    cJSON *search = cJSON_CreateObject();
    if (tools && search) {
        cJSON_AddBoolToObject(search, "enabled", cfg->search_enabled);
        cJSON_AddStringToObject(search, "apiKey", cfg->search_api_key);
        cJSON_AddItemToObject(tools, "search", search);
        cJSON_AddItemToObject(root, "tools", tools);
    }

    cJSON *logging = cJSON_CreateObject();
    if (logging) {
        cJSON_AddStringToObject(logging, "level", cfg->log_level);
        cJSON_AddBoolToObject(logging, "toFile", cfg->log_to_file);
        cJSON_AddBoolToObject(logging, "toStderr", cfg->log_to_stderr);
        cJSON_AddStringToObject(logging, "dir", cfg->log_dir);
        cJSON_AddStringToObject(logging, "file", cfg->log_file);
        cJSON_AddNumberToObject(logging, "maxFileBytes", cfg->log_max_file_bytes);
        cJSON_AddNumberToObject(logging, "maxFiles", cfg->log_max_files);
        cJSON_AddItemToObject(root, "logging", logging);
    }

    cJSON *tracing = cJSON_CreateObject();
    if (tracing) {
        cJSON_AddBoolToObject(tracing, "enabled", cfg->llm_trace_enabled);
        cJSON_AddStringToObject(tracing, "dir", cfg->llm_trace_dir);
        cJSON_AddNumberToObject(tracing, "maxFileBytes", cfg->llm_trace_max_file_bytes);
        cJSON_AddNumberToObject(tracing, "maxFieldBytes", cfg->llm_trace_max_field_bytes);
        cJSON_AddItemToObject(root, "tracing", tracing);
    }

    cJSON *network = cJSON_CreateObject();
    if (network) {
        cJSON_AddStringToObject(network, "dnsServer", cfg->dns_server);
        cJSON_AddItemToObject(root, "network", network);
    }

    /* Internal tunables (kept in JSON so config_view can be the single access layer). */
    cJSON *internal = cJSON_CreateObject();
    if (internal) {
        cJSON_AddNumberToObject(internal, "busQueueLen", cfg->bus_queue_len);
        cJSON_AddNumberToObject(internal, "wsPort", cfg->ws_port);
        cJSON_AddNumberToObject(internal, "heartbeatIntervalMs", cfg->heartbeat_interval_ms);
        cJSON_AddNumberToObject(internal, "cronCheckIntervalMs", cfg->cron_check_interval_ms);
        cJSON_AddItemToObject(root, "internal", internal);
    }

    return root;
}

static void config_inject_default_subagents(cJSON *root)
{
#if !MIMI_ENABLE_SUBAGENT
    (void)root;
    return;
#else
    if (!root || !cJSON_IsObject(root)) return;

    cJSON *agents = cJSON_GetObjectItem(root, "agents");
    if (!agents || !cJSON_IsObject(agents)) {
        agents = cJSON_CreateObject();
        if (!agents) return;
        cJSON_AddItemToObject(root, "agents", agents);
    }

    /* Ensure agents.defaults exists and set runtime enable flag (default true). */
    cJSON *defaults = cJSON_GetObjectItem(agents, "defaults");
    if (!defaults || !cJSON_IsObject(defaults)) {
        defaults = cJSON_CreateObject();
        if (!defaults) return;
        cJSON_AddItemToObject(agents, "defaults", defaults);
    }
    if (!cJSON_GetObjectItem(defaults, "subagentsEnabled")) {
        cJSON_AddBoolToObject(defaults, "subagentsEnabled", true);
    }

    /* Only inject if agents.subagents is missing or empty. */
    cJSON *subagents = cJSON_GetObjectItem(agents, "subagents");
    if (subagents && cJSON_IsArray(subagents) && cJSON_GetArraySize(subagents) > 0) {
        return;
    }
    if (subagents && !cJSON_IsArray(subagents)) {
        /* If user has a non-array type here, don't clobber it. */
        return;
    }
    if (!subagents) {
        subagents = cJSON_CreateArray();
        if (!subagents) return;
        cJSON_AddItemToObject(agents, "subagents", subagents);
    }

    /* Default subagent profile (OpenClaw-style): one generic profile with tools allowlist. */
    cJSON *def = cJSON_CreateObject();
    if (def) {
        cJSON_AddBoolToObject(def, "isolatedContext", true);

        cJSON *tools = cJSON_CreateArray();
        if (tools) {
            cJSON_AddItemToArray(tools, cJSON_CreateString("read_file"));
            cJSON_AddItemToArray(tools, cJSON_CreateString("write_file"));
            cJSON_AddItemToArray(tools, cJSON_CreateString("list_dir"));
            cJSON_AddItemToArray(tools, cJSON_CreateString("exec"));
            cJSON_AddItemToObject(def, "tools", tools);
        }
        cJSON_AddNumberToObject(def, "maxIters", 20);
        cJSON_AddNumberToObject(def, "timeoutSec", 600);
        cJSON_AddItemToArray(subagents, def);
    }
#endif
}

/* Internal raw JSON root getter for config_view.c.
 * Not exposed publicly: keep JSON out of most modules. */
const cJSON *mimi_config_json_root_internal(void)
{
    return s_json_root;
}

const char *mimi_config_get_path(void)
{
    return s_config_path[0] ? s_config_path : NULL;
}

mimi_err_t mimi_config_save_current(void)
{
    if (!s_config_path[0]) {
        MIMI_LOGW(TAG, "No config path known; call mimi_config_save(path) explicitly");
        return MIMI_ERR_INVALID_STATE;
    }
    return mimi_config_save(s_config_path);
}

/* Internal helper to navigate/create JSON path (e.g., "a.b.c" -> creates nodes as needed) */
static cJSON* config_json_ensure_path(cJSON *root, const char *path)
{
    if (!root || !path || !path[0]) return NULL;
    
    char path_copy[256];
    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    
    cJSON *current = root;
    char *token = strtok(path_copy, ".");
    
    while (token && current) {
        cJSON *next = cJSON_GetObjectItem(current, token);
        if (!next) {
            next = cJSON_AddObjectToObject(current, token);
        }
        current = next;
        token = strtok(NULL, ".");
    }
    
    return current;
}

/* Internal helper to set string values in JSON root */
static mimi_err_t config_json_set_str(const char *json_path, const char *value)
{
    if (!json_path || !json_path[0]) return MIMI_ERR_INVALID_ARG;
    
    /* Ensure JSON root exists */
    if (!s_json_root) {
        s_json_root = config_build_json_full_from_config(&s_config, MIMI_CONFIG_SCHEMA_VERSION);
        if (!s_json_root) return MIMI_ERR_NO_MEM;
    }
    
    /* Split path into parent path and key */
    char path_copy[256];
    strncpy(path_copy, json_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    
    char *last_dot = strrchr(path_copy, '.');
    if (!last_dot) {
        /* Root level key */
        cJSON_DeleteItemFromObject(s_json_root, path_copy);
        if (value && value[0]) {
            cJSON_AddStringToObject(s_json_root, path_copy, value);
        }
        return MIMI_OK;
    }
    
    /* Split into parent path and key */
    *last_dot = '\0';
    const char *key = last_dot + 1;
    
    /* Navigate to parent node */
    cJSON *parent = config_json_ensure_path(s_json_root, path_copy);
    if (!parent) return MIMI_ERR_INVALID_ARG;
    
    /* Set the value - always delete first to avoid duplicates */
    cJSON_DeleteItemFromObject(parent, key);
    if (value && value[0]) {
        cJSON_AddStringToObject(parent, key, value);
    }
    
    return MIMI_OK;
}

/* Internal helper to set bool values in JSON root */
static mimi_err_t config_json_set_bool(const char *json_path, bool value)
{
    if (!json_path || !json_path[0]) return MIMI_ERR_INVALID_ARG;
    
    /* Ensure JSON root exists */
    if (!s_json_root) {
        s_json_root = config_build_json_full_from_config(&s_config, MIMI_CONFIG_SCHEMA_VERSION);
        if (!s_json_root) return MIMI_ERR_NO_MEM;
    }
    
    /* Split path into parent path and key */
    char path_copy[256];
    strncpy(path_copy, json_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';
    
    char *last_dot = strrchr(path_copy, '.');
    if (!last_dot) {
        /* Root level key */
        cJSON_DeleteItemFromObject(s_json_root, path_copy);
        cJSON_AddBoolToObject(s_json_root, path_copy, value);
        return MIMI_OK;
    }
    
    /* Split into parent path and key */
    *last_dot = '\0';
    const char *key = last_dot + 1;
    
    /* Navigate to parent node */
    cJSON *parent = config_json_ensure_path(s_json_root, path_copy);
    if (!parent) return MIMI_ERR_INVALID_ARG;
    
    /* Set the value - always delete first to avoid duplicates */
    cJSON_DeleteItemFromObject(parent, key);
    cJSON_AddBoolToObject(parent, key, value);
    
    return MIMI_OK;
}

/*
 * Generic config setter - path format examples:
 * - "channels.wechat.bot_token" -> channels.wechat.bot_token
 * - "proxy.host" -> proxy.host
 * - "log_level" (root level key)
 *
 * Updates both the JSON root (source of truth for persistence) and the
 * runtime config struct for known fields.
 */
mimi_err_t mimi_config_set(const char *path, mimi_config_type_t type, const void *value)
{
    if (!path || !path[0] || !value) return MIMI_ERR_INVALID_ARG;
    
    mimi_err_t err = MIMI_OK;
    
    switch (type) {
        case MIMI_CONFIG_TYPE_STRING:
            err = config_json_set_str(path, (const char *)value);
            break;
        case MIMI_CONFIG_TYPE_BOOL:
            err = config_json_set_bool(path, *(const bool *)value);
            break;
        default:
            return MIMI_ERR_INVALID_ARG;
    }
    
    return err;
}

/* Convenience wrapper for string values */
mimi_err_t mimi_config_set_string(const char *path, const char *value)
{
    return mimi_config_set(path, MIMI_CONFIG_TYPE_STRING, value);
}

/* Convenience wrapper for bool values */
mimi_err_t mimi_config_set_bool(const char *path, bool value)
{
    return mimi_config_set(path, MIMI_CONFIG_TYPE_BOOL, &value);
}



mimi_err_t mimi_config_load(const char *path)
{
    apply_defaults();
    s_loaded = true;

    /* Reset raw JSON root; will be repopulated below. */
    if (s_json_root) {
        cJSON_Delete(s_json_root);
        s_json_root = NULL;
    }

    /* Store config path for symmetry: allows mimi_config_save_current() */
    if (path && path[0] != '\0') {
        strncpy(s_config_path, path, sizeof(s_config_path) - 1);
        s_config_path[sizeof(s_config_path) - 1] = '\0';
    } else {
        s_config_path[0] = '\0';
    }

    if (!path || path[0] == '\0') {
        MIMI_LOGI(TAG, "No config path; using defaults");
        s_json_root = config_build_json_full_from_config(&s_config, MIMI_CONFIG_SCHEMA_VERSION);
        return MIMI_OK;
    }

    MIMI_LOGD(TAG, "Loading config from: %s", path);

    mimi_file_t *f = NULL;
    mimi_err_t ferr = mimi_fs_open(path, "rb", &f);
    if (ferr != MIMI_OK) {
        if (ferr == MIMI_ERR_NOT_FOUND) {
            MIMI_LOGI(TAG, "Config file not found: %s (using defaults)", path);
            s_json_root = config_build_json_full_from_config(&s_config, MIMI_CONFIG_SCHEMA_VERSION);
            return MIMI_OK;
        }
        MIMI_LOGE(TAG, "Cannot open config: %s", path);
        return MIMI_ERR_IO;
    }
    MIMI_LOGD(TAG, "Config file opened successfully");

    (void)mimi_fs_seek(f, 0, SEEK_END);
    long n = 0;
    (void)mimi_fs_tell(f, &n);
    (void)mimi_fs_seek(f, 0, SEEK_SET);
    if (n < 0 || n > 512 * 1024) {
        mimi_fs_close(f);
        return MIMI_ERR_IO;
    }

    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        mimi_fs_close(f);
        return MIMI_ERR_NO_MEM;
    }
    size_t r = 0;
    (void)mimi_fs_read(f, buf, (size_t)n, &r);
    mimi_fs_close(f);
    buf[r] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        MIMI_LOGW(TAG, "Invalid JSON in %s; using defaults", path);
        s_json_root = config_build_json_full_from_config(&s_config, MIMI_CONFIG_SCHEMA_VERSION);
        return MIMI_OK;
    }
    MIMI_LOGD(TAG, "Config JSON parsed successfully");

    /* Schema/versioned merge:
     * - Build a full "current schema" JSON from defaults
     * - Deep-merge user's file over it (user values win)
     * - If user's file is missing keys or schemaVersion is old/missing, write back merged result */
    cJSON *file_root = root;
    int file_ver = config_get_schema_version(file_root);
    cJSON *merged_root = config_build_json_full_from_config(&s_config, MIMI_CONFIG_SCHEMA_VERSION);
    bool should_write_back = false;
    if (merged_root && cJSON_IsObject(merged_root)) {
        bool missing_keys = json_any_missing_expected(merged_root, file_root);
        bool version_mismatch = (file_ver != MIMI_CONFIG_SCHEMA_VERSION);
        should_write_back = missing_keys || version_mismatch;
        json_merge_object_into(merged_root, file_root);
        /* Force bump schemaVersion after merge.
         * The merge policy is "user values win", so a user's old schemaVersion
         * key would otherwise overwrite the upgraded default schemaVersion. */
        cJSON *sv = cJSON_GetObjectItemCaseSensitive(merged_root, "schemaVersion");
        if (sv && cJSON_IsNumber(sv)) {
            sv->valuedouble = (double)MIMI_CONFIG_SCHEMA_VERSION;
            sv->valueint = MIMI_CONFIG_SCHEMA_VERSION;
        } else if (!sv) {
            cJSON_AddNumberToObject(merged_root, "schemaVersion", MIMI_CONFIG_SCHEMA_VERSION);
        }
        root = merged_root;
    } else {
        merged_root = NULL;
    }

    /* Persist merged raw JSON for extensible sections (providers/tools/channels/plugins). */
    s_json_root = cJSON_Duplicate(root, 1);

    /* agents.defaults (nanobot-style) */
    cJSON *agents = cJSON_GetObjectItem(root, "agents");
    cJSON *defaults = agents && cJSON_IsObject(agents) ? cJSON_GetObjectItem(agents, "defaults") : NULL;
    if (cJSON_IsObject(defaults)) {
        json_str_to_buf(cJSON_GetObjectItem(defaults, "workspace"), s_config.workspace, sizeof(s_config.workspace), true);
        json_str_to_buf(cJSON_GetObjectItem(defaults, "timezone"), s_config.timezone, sizeof(s_config.timezone), true);
        json_str_to_buf(cJSON_GetObjectItem(defaults, "provider"), s_config.provider, sizeof(s_config.provider), true);
        json_str_to_buf(cJSON_GetObjectItem(defaults, "model"), s_config.model, sizeof(s_config.model), true);
        json_str_to_buf(cJSON_GetObjectItem(defaults, "apiProtocol"), s_config.api_protocol, sizeof(s_config.api_protocol), true);

        cJSON *mt = cJSON_GetObjectItem(defaults, "maxTokens");
        if (mt && cJSON_IsNumber(mt) && mt->valuedouble > 0) s_config.max_tokens = (int)mt->valuedouble;

        cJSON *temp = cJSON_GetObjectItem(defaults, "temperature");
        if (temp && cJSON_IsNumber(temp)) s_config.temperature = temp->valuedouble;

        cJSON *iter = cJSON_GetObjectItem(defaults, "maxToolIterations");
        if (iter && cJSON_IsNumber(iter) && iter->valuedouble > 0) s_config.max_tool_iterations = (int)iter->valuedouble;

        cJSON *mw = cJSON_GetObjectItem(defaults, "memoryWindow");
        if (mw && cJSON_IsNumber(mw) && mw->valuedouble > 0) s_config.memory_window = (int)mw->valuedouble;

        json_str_to_buf(cJSON_GetObjectItem(defaults, "apiUrl"), s_config.api_url, sizeof(s_config.api_url), true);

        cJSON *working = cJSON_GetObjectItem(defaults, "sendWorkingStatus");
        if (working && cJSON_IsBool(working)) s_config.send_working_status = cJSON_IsTrue(working);
    }

    expand_tilde_inplace(s_config.workspace, sizeof(s_config.workspace));
    
    /* Normalize path separators to platform-specific format */
    char normalized_workspace[512];
    if (mimi_path_normalize(s_config.workspace, normalized_workspace, sizeof(normalized_workspace)) == 0) {
        safe_strcpy(s_config.workspace, sizeof(s_config.workspace), normalized_workspace);
    }

    /* agents.subagents: load static subagent configs (inproc/future fork/tcp) */
    s_config.subagent_count = 0;
    if (agents && cJSON_IsObject(agents)) {
        cJSON *subagents = cJSON_GetObjectItem(agents, "subagents");
        if (cJSON_IsArray(subagents)) {
            int count = cJSON_GetArraySize(subagents);
            for (int i = 0; i < count && s_config.subagent_count < (int)(sizeof(s_config.subagents) / sizeof(s_config.subagents[0])); i++) {
                cJSON *sa = cJSON_GetArrayItem(subagents, i);
                if (!cJSON_IsObject(sa)) continue;

                mimi_subagent_config_t *dst = &s_config.subagents[s_config.subagent_count];
                memset(dst, 0, sizeof(*dst));

                json_str_to_buf(cJSON_GetObjectItem(sa, "name"), dst->name, sizeof(dst->name), false);
                json_str_to_buf(cJSON_GetObjectItem(sa, "role"), dst->role, sizeof(dst->role), true);
                json_str_to_buf(cJSON_GetObjectItem(sa, "type"), dst->type, sizeof(dst->type), true);
                json_str_to_buf(cJSON_GetObjectItem(sa, "systemPromptFile"),
                                dst->system_prompt_file, sizeof(dst->system_prompt_file), true);

                /* tools: JSON array -> comma-separated list for now */
                cJSON *tools_arr = cJSON_GetObjectItem(sa, "tools");
                if (cJSON_IsArray(tools_arr)) {
                    size_t off = 0;
                    dst->tools[0] = '\0';
                    int tcount = cJSON_GetArraySize(tools_arr);
                    for (int ti = 0; ti < tcount; ti++) {
                        cJSON *t = cJSON_GetArrayItem(tools_arr, ti);
                        const char *name = cJSON_GetStringValue(t);
                        if (!name || !name[0]) continue;
                        size_t name_len = strnlen(name, sizeof(dst->tools) - 1);
                        if (off + name_len + 1 >= sizeof(dst->tools)) break;
                        if (off > 0) {
                            dst->tools[off++] = ',';
                        }
                        memcpy(dst->tools + off, name, name_len);
                        off += name_len;
                        dst->tools[off] = '\0';
                    }
                }

                cJSON *mi = cJSON_GetObjectItem(sa, "maxIters");
                if (mi && cJSON_IsNumber(mi) && mi->valuedouble > 0) {
                    dst->max_iters = (int)mi->valuedouble;
                }

                cJSON *to = cJSON_GetObjectItem(sa, "timeoutSec");
                if (to && cJSON_IsNumber(to) && to->valuedouble > 0) {
                    dst->timeout_sec = (int)to->valuedouble;
                }

                /* Defaults for missing fields */
                if (dst->type[0] == '\0') {
                    safe_strcpy(dst->type, sizeof(dst->type), "inproc");
                }
                if (dst->max_iters <= 0) {
                    dst->max_iters = s_config.max_tool_iterations;
                }

                s_config.subagent_count++;
            }
        }
    }

    /* providers.* — pick apiKey/apiBase by provider name */
    cJSON *providers = cJSON_GetObjectItem(root, "providers");
    if (cJSON_IsObject(providers)) {
        const char *prov_name = s_config.provider[0] ? s_config.provider : "anthropic";
        cJSON *prov = cJSON_GetObjectItem(providers, prov_name);
        if (!prov) prov = cJSON_GetObjectItem(providers, "openrouter");
        if (!prov) prov = cJSON_GetObjectItem(providers, "anthropic");
        if (cJSON_IsObject(prov)) {
            json_str_to_buf(cJSON_GetObjectItem(prov, "apiKey"), s_config.api_key, sizeof(s_config.api_key), false);
            json_str_to_buf(cJSON_GetObjectItem(prov, "apiBase"), s_config.api_base, sizeof(s_config.api_base), true);
        }
        if (s_config.api_key[0] == '\0') {
            prov = cJSON_GetObjectItem(providers, "openrouter");
            if (cJSON_IsObject(prov))
                json_str_to_buf(cJSON_GetObjectItem(prov, "apiKey"), s_config.api_key, sizeof(s_config.api_key), false);
        }
    }

    /* channels.telegram */
    cJSON *channels = cJSON_GetObjectItem(root, "channels");
    cJSON *tg = channels && cJSON_IsObject(channels) ? cJSON_GetObjectItem(channels, "telegram") : NULL;
    if (cJSON_IsObject(tg)) {
        cJSON *en = cJSON_GetObjectItem(tg, "enabled");
        if (en && (cJSON_IsBool(en) || cJSON_IsNumber(en))) {
            s_config.telegram_enabled = cJSON_IsTrue(en) || (cJSON_IsNumber(en) && en->valueint != 0);
        }
        json_str_to_buf(cJSON_GetObjectItem(tg, "token"), s_config.telegram_token, sizeof(s_config.telegram_token), false);
        cJSON *allow = cJSON_GetObjectItem(tg, "allowFrom");
        if (allow && cJSON_IsArray(allow))
            parse_allow_from(allow, s_config.telegram_allow_from, sizeof(s_config.telegram_allow_from));
    }

    /* channels.feishu */
    cJSON *feishu = channels && cJSON_IsObject(channels) ? cJSON_GetObjectItem(channels, "feishu") : NULL;
    MIMI_LOGD(TAG, "Loading feishu config, channels=%p, feishu=%p", (void*)channels, (void*)feishu);
    if (cJSON_IsObject(feishu)) {
        cJSON *en = cJSON_GetObjectItem(feishu, "enabled");
        cJSON *app_id = cJSON_GetObjectItem(feishu, "appId");
        cJSON *app_secret = cJSON_GetObjectItem(feishu, "appSecret");
        MIMI_LOGD(TAG, "Feishu fields: enabled=%p (type=%d, val=%d), appId=%p, appSecret=%p",
                  (void*)en, en ? en->type : -1, en ? cJSON_IsTrue(en) : -1, (void*)app_id, (void*)app_secret);
        if (en && (cJSON_IsBool(en) || cJSON_IsNumber(en))) {
            s_config.feishu_enabled = cJSON_IsTrue(en) || (cJSON_IsNumber(en) && en->valueint != 0);
            MIMI_LOGD(TAG, "Feishu enabled set to: %d", s_config.feishu_enabled);
        }
        if (app_id && cJSON_IsString(app_id)) {
            strncpy(s_config.feishu_app_id, app_id->valuestring, sizeof(s_config.feishu_app_id) - 1);
            MIMI_LOGD(TAG, "Feishu app_id set to: %s", s_config.feishu_app_id);
        }
        if (app_secret && cJSON_IsString(app_secret)) {
            strncpy(s_config.feishu_app_secret, app_secret->valuestring, sizeof(s_config.feishu_app_secret) - 1);
        }
    } else {
        MIMI_LOGW(TAG, "Feishu config not found or not an object");
    }

    /* channels.qq */
    cJSON *qq = channels && cJSON_IsObject(channels) ? cJSON_GetObjectItem(channels, "qq") : NULL;
    if (cJSON_IsObject(qq)) {
        cJSON *en = cJSON_GetObjectItem(qq, "enabled");
        if (en && (cJSON_IsBool(en) || cJSON_IsNumber(en))) {
            s_config.qq_enabled = cJSON_IsTrue(en) || (cJSON_IsNumber(en) && en->valueint != 0);
        }
        json_str_to_buf(cJSON_GetObjectItem(qq, "appId"), s_config.qq_app_id, sizeof(s_config.qq_app_id), false);
        json_str_to_buf(cJSON_GetObjectItem(qq, "secret"), s_config.qq_token, sizeof(s_config.qq_token), false);
    }

    /* channels.wechat */
    cJSON *wechat = channels && cJSON_IsObject(channels) ? cJSON_GetObjectItem(channels, "wechat") : NULL;
    if (cJSON_IsObject(wechat)) {
        cJSON *en = cJSON_GetObjectItem(wechat, "enabled");
        if (en && (cJSON_IsBool(en) || cJSON_IsNumber(en))) {
            s_config.wechat_enabled = cJSON_IsTrue(en) || (cJSON_IsNumber(en) && en->valueint != 0);
        }
        json_str_to_buf(cJSON_GetObjectItem(wechat, "bot_token"), s_config.wechat_bot_token, sizeof(s_config.wechat_bot_token), false);
        json_str_to_buf(cJSON_GetObjectItem(wechat, "bot_id"), s_config.wechat_bot_id, sizeof(s_config.wechat_bot_id), false);
        json_str_to_buf(cJSON_GetObjectItem(wechat, "user_id"), s_config.wechat_user_id, sizeof(s_config.wechat_user_id), false);
    }

    /* proxy (top-level or under a key) */
    cJSON *proxy = cJSON_GetObjectItem(root, "proxy");
    if (!proxy || !cJSON_IsObject(proxy))
        proxy = cJSON_GetObjectItem(root, "proxy_config");
    if (cJSON_IsObject(proxy)) {
        json_str_to_buf(cJSON_GetObjectItem(proxy, "host"), s_config.proxy_host, sizeof(s_config.proxy_host), true);
        json_str_to_buf(cJSON_GetObjectItem(proxy, "port"), s_config.proxy_port, sizeof(s_config.proxy_port), true);
        json_str_to_buf(cJSON_GetObjectItem(proxy, "type"), s_config.proxy_type, sizeof(s_config.proxy_type), true);
    }

    /* tools.search (Brave) */
    cJSON *tools = cJSON_GetObjectItem(root, "tools");
    cJSON *search = tools && cJSON_IsObject(tools) ? cJSON_GetObjectItem(tools, "search") : NULL;
    if (!search && cJSON_IsObject(tools)) search = cJSON_GetObjectItem(tools, "brave");
    if (cJSON_IsObject(search)) {
        json_str_to_buf(cJSON_GetObjectItem(search, "apiKey"), s_config.search_api_key, sizeof(s_config.search_api_key), false);
        cJSON *enabled = cJSON_GetObjectItem(search, "enabled");
        if (enabled && (cJSON_IsBool(enabled) || cJSON_IsNumber(enabled))) {
            s_config.search_enabled = cJSON_IsTrue(enabled) || (cJSON_IsNumber(enabled) && enabled->valueint != 0);
        }
    }
    
    /* logging */
    cJSON *logging = cJSON_GetObjectItem(root, "logging");
    if (cJSON_IsObject(logging)) {
        json_str_to_buf(cJSON_GetObjectItem(logging, "level"), s_config.log_level, sizeof(s_config.log_level), true);
        cJSON *to_file = cJSON_GetObjectItem(logging, "toFile");
        if (to_file && (cJSON_IsBool(to_file) || cJSON_IsNumber(to_file))) {
            s_config.log_to_file = cJSON_IsTrue(to_file) || (cJSON_IsNumber(to_file) && to_file->valueint != 0);
        }
        cJSON *to_stderr = cJSON_GetObjectItem(logging, "toStderr");
        if (to_stderr && (cJSON_IsBool(to_stderr) || cJSON_IsNumber(to_stderr))) {
            s_config.log_to_stderr = cJSON_IsTrue(to_stderr) || (cJSON_IsNumber(to_stderr) && to_stderr->valueint != 0);
        }
        json_str_to_buf(cJSON_GetObjectItem(logging, "dir"), s_config.log_dir, sizeof(s_config.log_dir), true);
        json_str_to_buf(cJSON_GetObjectItem(logging, "file"), s_config.log_file, sizeof(s_config.log_file), true);
        cJSON *lmfb = cJSON_GetObjectItem(logging, "maxFileBytes");
        if (lmfb && cJSON_IsNumber(lmfb) && lmfb->valuedouble >= 0) {
            s_config.log_max_file_bytes = (int)lmfb->valuedouble;
        }
        cJSON *lmf = cJSON_GetObjectItem(logging, "maxFiles");
        if (lmf && cJSON_IsNumber(lmf) && lmf->valuedouble >= 0) {
            s_config.log_max_files = (int)lmf->valuedouble;
        }
    }

    /* tracing */
    cJSON *tracing = cJSON_GetObjectItem(root, "tracing");
    if (cJSON_IsObject(tracing)) {
        cJSON *en = cJSON_GetObjectItem(tracing, "enabled");
        if (en && (cJSON_IsBool(en) || cJSON_IsNumber(en))) {
            s_config.llm_trace_enabled = cJSON_IsTrue(en) || (cJSON_IsNumber(en) && en->valueint != 0);
        }
        json_str_to_buf(cJSON_GetObjectItem(tracing, "dir"), s_config.llm_trace_dir, sizeof(s_config.llm_trace_dir), true);
        cJSON *mfb = cJSON_GetObjectItem(tracing, "maxFileBytes");
        if (mfb && cJSON_IsNumber(mfb) && mfb->valuedouble >= 0) {
            s_config.llm_trace_max_file_bytes = (int)mfb->valuedouble;
        }
        cJSON *mfd = cJSON_GetObjectItem(tracing, "maxFieldBytes");
        if (mfd && cJSON_IsNumber(mfd) && mfd->valuedouble >= 0) {
            s_config.llm_trace_max_field_bytes = (int)mfd->valuedouble;
        }
    }
    
    /* network */
    cJSON *network = cJSON_GetObjectItem(root, "network");
    if (cJSON_IsObject(network))
        json_str_to_buf(cJSON_GetObjectItem(network, "dnsServer"), s_config.dns_server, sizeof(s_config.dns_server), true);

    if (should_write_back) {
        if (file_ver > MIMI_CONFIG_SCHEMA_VERSION) {
            MIMI_LOGW(TAG, "Config schemaVersion=%d is newer than supported=%d; writing may drop unknown fields",
                      file_ver, MIMI_CONFIG_SCHEMA_VERSION);
        }
        char *json_str = cJSON_Print(root);
        if (json_str) {
            mimi_err_t werr = write_text_file_vfs(path, json_str, strlen(json_str));
            if (werr == MIMI_OK) {
                MIMI_LOGI(TAG, "Merged config written back to %s (schemaVersion %d -> %d)",
                          path, file_ver, MIMI_CONFIG_SCHEMA_VERSION);
            } else {
                MIMI_LOGW(TAG, "Failed to write merged config to %s: %s", path, mimi_err_to_name(werr));
            }
            free(json_str);
        }
    }

    if (merged_root) cJSON_Delete(merged_root);
    cJSON_Delete(file_root);
    MIMI_LOGD(TAG, "Loaded config from %s (workspace=%s provider=%s model=%s)",
              path, s_config.workspace, s_config.provider, s_config.model);
    return MIMI_OK;
}

mimi_err_t mimi_config_save(const char *path)
{
    if (!path || path[0] == '\0') return MIMI_ERR_INVALID_ARG;

    /* Prefer saving the raw merged JSON root so unknown/plugin fields aren't lost. */
    cJSON *root = s_json_root ? cJSON_Duplicate(s_json_root, 1)
                              : config_build_json_full_from_config(&s_config, MIMI_CONFIG_SCHEMA_VERSION);
    if (!root) return MIMI_ERR_NO_MEM;

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return MIMI_ERR_NO_MEM;

    mimi_err_t err = write_text_file_vfs(path, json_str, strlen(json_str));
    free(json_str);
    return err;
}

mimi_err_t mimi_config_save_starter(const char *path, bool with_default_subagents)
{
    if (!path || path[0] == '\0') return MIMI_ERR_INVALID_ARG;

    /* Prefer saving the raw merged JSON root so unknown/plugin fields aren't lost. */
    cJSON *root = s_json_root ? cJSON_Duplicate(s_json_root, 1)
                              : config_build_json_full_from_config(&s_config, MIMI_CONFIG_SCHEMA_VERSION);
    if (!root) return MIMI_ERR_NO_MEM;

    if (with_default_subagents) {
        config_inject_default_subagents(root);
    }

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return MIMI_ERR_NO_MEM;

    mimi_err_t err = write_text_file_vfs(path, json_str, strlen(json_str));
    free(json_str);
    return err;
}
