/*
 * Runtime config: load/save JSON (nanobot-style schema), built-in defaults.
 */

#include "config.h"
#include "log.h"
#include "cJSON.h"
#include "fs/fs.h"
#include "path_utils.h"

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

    /* Proxy */
    s_config.proxy_host[0] = '\0';
    s_config.proxy_port[0] = '\0';
    safe_strcpy(s_config.proxy_type, sizeof(s_config.proxy_type), "http");

    /* Tools */
    s_config.search_api_key[0] = '\0';

    /* Internal tunables */
    s_config.bus_queue_len = 16;
    s_config.ws_port = 18789;
    s_config.heartbeat_interval_ms = 30 * 60 * 1000;
    s_config.cron_check_interval_ms = 60 * 1000;

    /* Files/directories (treated as real paths; may be absolute or relative) */
    safe_strcpy(s_config.heartbeat_file, sizeof(s_config.heartbeat_file), "HEARTBEAT.md");
    safe_strcpy(s_config.cron_file, sizeof(s_config.cron_file), "cron.json");
    safe_strcpy(s_config.memory_file, sizeof(s_config.memory_file), "memory/MEMORY.md");
    safe_strcpy(s_config.soul_file, sizeof(s_config.soul_file), "config/SOUL.md");
    safe_strcpy(s_config.user_file, sizeof(s_config.user_file), "config/USER.md");
    safe_strcpy(s_config.skills_prefix, sizeof(s_config.skills_prefix), "skills/");
    safe_strcpy(s_config.session_dir, sizeof(s_config.session_dir), "sessions");

    /* WiFi (embedded) */
    s_config.wifi_ssid[0] = '\0';
    s_config.wifi_pass[0] = '\0';
    
    /* Network */
    safe_strcpy(s_config.dns_server, sizeof(s_config.dns_server), "114.114.114.114");
    
    /* Logging */
    safe_strcpy(s_config.log_level, sizeof(s_config.log_level), "info");
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

mimi_err_t mimi_config_load(const char *path)
{
    apply_defaults();
    s_loaded = true;

    if (!path || path[0] == '\0') {
        MIMI_LOGI(TAG, "No config path; using defaults");
        return MIMI_OK;
    }

    MIMI_LOGI(TAG, "Loading config from: %s", path);

    mimi_file_t *f = NULL;
    mimi_err_t ferr = mimi_fs_open(path, "rb", &f);
    if (ferr != MIMI_OK) {
        if (ferr == MIMI_ERR_NOT_FOUND) {
            MIMI_LOGI(TAG, "Config file not found: %s (using defaults)", path);
            return MIMI_OK;
        }
        MIMI_LOGE(TAG, "Cannot open config: %s", path);
        return MIMI_ERR_IO;
    }
    MIMI_LOGI(TAG, "Config file opened successfully");

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
        return MIMI_OK;
    }
    MIMI_LOGI(TAG, "Config JSON parsed successfully");

    /* agents.defaults (nanobot-style) */
    cJSON *agents = cJSON_GetObjectItem(root, "agents");
    cJSON *defaults = agents && cJSON_IsObject(agents) ? cJSON_GetObjectItem(agents, "defaults") : NULL;
    if (cJSON_IsObject(defaults)) {
        json_str_to_buf(cJSON_GetObjectItem(defaults, "workspace"), s_config.workspace, sizeof(s_config.workspace), true);
        json_str_to_buf(cJSON_GetObjectItem(defaults, "timezone"), s_config.timezone, sizeof(s_config.timezone), true);
        json_str_to_buf(cJSON_GetObjectItem(defaults, "provider"), s_config.provider, sizeof(s_config.provider), true);
        json_str_to_buf(cJSON_GetObjectItem(defaults, "model"), s_config.model, sizeof(s_config.model), true);

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
    MIMI_LOGI(TAG, "Loading feishu config, channels=%p, feishu=%p", (void*)channels, (void*)feishu);
    if (cJSON_IsObject(feishu)) {
        cJSON *en = cJSON_GetObjectItem(feishu, "enabled");
        cJSON *app_id = cJSON_GetObjectItem(feishu, "appId");
        cJSON *app_secret = cJSON_GetObjectItem(feishu, "appSecret");
        MIMI_LOGI(TAG, "Feishu fields: enabled=%p (type=%d, val=%d), appId=%p, appSecret=%p",
                  (void*)en, en ? en->type : -1, en ? cJSON_IsTrue(en) : -1, (void*)app_id, (void*)app_secret);
        if (en && (cJSON_IsBool(en) || cJSON_IsNumber(en))) {
            s_config.feishu_enabled = cJSON_IsTrue(en) || (cJSON_IsNumber(en) && en->valueint != 0);
            MIMI_LOGI(TAG, "Feishu enabled set to: %d", s_config.feishu_enabled);
        }
        if (app_id && cJSON_IsString(app_id)) {
            strncpy(s_config.feishu_app_id, app_id->valuestring, sizeof(s_config.feishu_app_id) - 1);
            MIMI_LOGI(TAG, "Feishu app_id set to: %s", s_config.feishu_app_id);
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
    if (cJSON_IsObject(search))
        json_str_to_buf(cJSON_GetObjectItem(search, "apiKey"), s_config.search_api_key, sizeof(s_config.search_api_key), false);
    
    /* logging */
    cJSON *logging = cJSON_GetObjectItem(root, "logging");
    if (cJSON_IsObject(logging))
        json_str_to_buf(cJSON_GetObjectItem(logging, "level"), s_config.log_level, sizeof(s_config.log_level), true);
    
    /* network */
    cJSON *network = cJSON_GetObjectItem(root, "network");
    if (cJSON_IsObject(network))
        json_str_to_buf(cJSON_GetObjectItem(network, "dnsServer"), s_config.dns_server, sizeof(s_config.dns_server), true);

    cJSON_Delete(root);
    MIMI_LOGI(TAG, "Loaded config from %s (workspace=%s provider=%s model=%s)",
              path, s_config.workspace, s_config.provider, s_config.model);
    return MIMI_OK;
}

const mimi_config_t *mimi_config_get(void)
{
    if (!s_loaded)
        apply_defaults();
    return &s_config;
}

mimi_err_t mimi_config_save(const char *path)
{
    if (!path || path[0] == '\0') return MIMI_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    if (!root) return MIMI_ERR_NO_MEM;

    cJSON *providers = cJSON_CreateObject();
    if (providers) {
        cJSON *prov = cJSON_CreateObject();
        if (prov) {
            cJSON_AddStringToObject(prov, "apiKey", s_config.api_key);
            if (s_config.api_base[0]) cJSON_AddStringToObject(prov, "apiBase", s_config.api_base);
            cJSON_AddItemToObject(providers, s_config.provider[0] ? s_config.provider : "anthropic", prov);
        }
        cJSON_AddItemToObject(root, "providers", providers);
    }

    cJSON *agents = cJSON_CreateObject();
    cJSON *defaults = cJSON_CreateObject();
    if (agents && defaults) {
        cJSON_AddStringToObject(defaults, "workspace", s_config.workspace);
        cJSON_AddStringToObject(defaults, "timezone", s_config.timezone);
        cJSON_AddStringToObject(defaults, "model", s_config.model);
        cJSON_AddStringToObject(defaults, "provider", s_config.provider);
        cJSON_AddNumberToObject(defaults, "maxTokens", s_config.max_tokens);
        cJSON_AddNumberToObject(defaults, "temperature", s_config.temperature);
        cJSON_AddNumberToObject(defaults, "maxToolIterations", s_config.max_tool_iterations);
        cJSON_AddNumberToObject(defaults, "memoryWindow", s_config.memory_window);
        if (s_config.api_url[0]) cJSON_AddStringToObject(defaults, "apiUrl", s_config.api_url);
        cJSON_AddBoolToObject(defaults, "sendWorkingStatus", s_config.send_working_status);
        cJSON_AddItemToObject(agents, "defaults", defaults);
        cJSON_AddItemToObject(root, "agents", agents);
    }

    cJSON *channels = cJSON_CreateObject();
    if (channels) {
        /* telegram */
        cJSON *tg = cJSON_CreateObject();
        if (tg) {
            cJSON_AddBoolToObject(tg, "enabled", s_config.telegram_enabled);
            cJSON_AddStringToObject(tg, "token", s_config.telegram_token);
            if (s_config.telegram_allow_from[0]) {
                cJSON *arr = cJSON_CreateArray();
                if (arr) {
                    cJSON_AddItemToArray(arr, cJSON_CreateString(s_config.telegram_allow_from));
                    cJSON_AddItemToObject(tg, "allowFrom", arr);
                }
            }
            cJSON_AddItemToObject(channels, "telegram", tg);
        }

        /* feishu */
        cJSON *feishu = cJSON_CreateObject();
        if (feishu) {
            cJSON_AddBoolToObject(feishu, "enabled", s_config.feishu_enabled);
            cJSON_AddStringToObject(feishu, "appId", s_config.feishu_app_id);
            cJSON_AddStringToObject(feishu, "appSecret", s_config.feishu_app_secret);
            cJSON_AddItemToObject(channels, "feishu", feishu);
        }

        /* qq */
        cJSON *qq = cJSON_CreateObject();
        if (qq) {
            cJSON_AddBoolToObject(qq, "enabled", s_config.qq_enabled);
            cJSON_AddStringToObject(qq, "appId", s_config.qq_app_id);
            cJSON_AddStringToObject(qq, "secret", s_config.qq_token);
            cJSON_AddItemToObject(channels, "qq", qq);
        }

        cJSON_AddItemToObject(root, "channels", channels);
    }

    cJSON *proxy = cJSON_CreateObject();
    if (proxy) {
        cJSON_AddStringToObject(proxy, "host", s_config.proxy_host);
        cJSON_AddStringToObject(proxy, "port", s_config.proxy_port);
        cJSON_AddStringToObject(proxy, "type", s_config.proxy_type);
        cJSON_AddItemToObject(root, "proxy", proxy);
    }

    cJSON *tools = cJSON_CreateObject();
    cJSON *search = cJSON_CreateObject();
    if (tools && search) {
        cJSON_AddStringToObject(search, "apiKey", s_config.search_api_key);
        cJSON_AddItemToObject(tools, "search", search);
        cJSON_AddItemToObject(root, "tools", tools);
    }
    
    /* logging */
    cJSON *logging = cJSON_CreateObject();
    if (logging) {
        cJSON_AddStringToObject(logging, "level", s_config.log_level);
        cJSON_AddItemToObject(root, "logging", logging);
    }
    
    /* network */
    cJSON *network = cJSON_CreateObject();
    if (network) {
        cJSON_AddStringToObject(network, "dnsServer", s_config.dns_server);
        cJSON_AddItemToObject(root, "network", network);
    }

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return MIMI_ERR_NO_MEM;

    /* Use standard POSIX file operations for absolute paths outside VFS */
#ifdef _WIN32
    /* Convert UTF-8 path to wide string for Windows */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) {
        free(json_str);
        return MIMI_ERR_IO;
    }
    wchar_t *wpath = (wchar_t *)malloc(wlen * sizeof(wchar_t));
    if (!wpath) {
        free(json_str);
        return MIMI_ERR_NO_MEM;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
    
    FILE *fp = _wfopen(wpath, L"w");
    free(wpath);
#else
    FILE *fp = fopen(path, "w");
#endif
    
    if (!fp) {
        free(json_str);
        return MIMI_ERR_IO;
    }
    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, fp);
    fclose(fp);
    free(json_str);
    return (written == len) ? MIMI_OK : MIMI_ERR_IO;
}
