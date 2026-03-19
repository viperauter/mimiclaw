#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "tools/tool_get_time.h"
#include "config.h"
#include "config_view.h"

#include "http/http.h"
#include "log.h"

#include "cJSON.h"

static const char *TAG = "tool_time_posix";
static const char *TOOL_SCHEMA =
    "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}";
static const char *TOOL_DESCRIPTION =
    "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.";

const char *tool_get_time_schema_json(void) { return TOOL_SCHEMA; }
const char *tool_get_time_description(void) { return TOOL_DESCRIPTION; }

static bool set_clock_from_epoch(long long epoch, char *out, size_t out_size)
{
    if (epoch <= 0) return false;

    time_t t = (time_t) epoch;

#ifdef _WIN32
    /* Windows doesn't have settimeofday/setenv */
    (void)t;
#else
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    mimi_cfg_obj_t defaults = mimi_cfg_get_obj(mimi_cfg_section("agents"), "defaults");
    const char *tz = mimi_cfg_get_str(defaults, "timezone", "UTC");
    setenv("TZ", (tz && tz[0] ? tz : "UTC"), 1);
    tzset();
#endif

    struct tm local;
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S %Z (%A)", &local);
    return true;
}

mimi_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size,
                                 const mimi_session_ctx_t *session_ctx)
{
    (void)input_json;
    (void)session_ctx;
    MIMI_LOGI(TAG, "Fetching current time via worldtimeapi.org...");

    mimi_http_request_t req = {
        .method = "GET",
        .url = "https://worldtimeapi.org/api/timezone/Etc/UTC",
        .headers = "Accept: application/json\r\n",
        .body = NULL,
        .body_len = 0,
        .timeout_ms = 10000,
    };
    mimi_http_response_t resp;
    mimi_err_t err = mimi_http_exec(&req, &resp);
    if (err != MIMI_OK) {
        snprintf(output, output_size, "Error: failed to fetch time (%s)", mimi_err_to_name(err));
        MIMI_LOGE(TAG, "%s", output);
        return err;
    }

    if (resp.status != 200 || !resp.body) {
        snprintf(output, output_size, "Error: time API returned %d", resp.status);
        MIMI_LOGE(TAG, "%s", output);
        mimi_http_response_free(&resp);
        return MIMI_ERR_FAIL;
    }

    cJSON *root = cJSON_Parse((char *) resp.body);
    mimi_http_response_free(&resp);
    if (!root) {
        snprintf(output, output_size, "Error: failed to parse time API response");
        MIMI_LOGE(TAG, "%s", output);
        return MIMI_ERR_FAIL;
    }

    cJSON *unixtime = cJSON_GetObjectItem(root, "unixtime");
    long long epoch = 0;
    if (unixtime && cJSON_IsNumber(unixtime)) {
        epoch = (long long) unixtime->valuedouble;
    }

    bool ok = set_clock_from_epoch(epoch, output, output_size);
    cJSON_Delete(root);

    if (!ok) {
        snprintf(output, output_size, "Error: invalid time value from API");
        MIMI_LOGE(TAG, "%s", output);
        return MIMI_ERR_FAIL;
    }

    MIMI_LOGI(TAG, "Time: %s", output);
    return MIMI_OK;
}

