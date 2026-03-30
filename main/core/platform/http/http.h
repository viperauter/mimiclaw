#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIMI_HTTP_CALL_ONESHOT = 0,
    MIMI_HTTP_CALL_STREAM  = 1,
} mimi_http_call_mode_t;

/* Forward declarations for request-level stream options */
typedef struct mimi_http_stream_handle mimi_http_stream_handle_t;
typedef struct mimi_http_stream_callbacks mimi_http_stream_callbacks_t;

typedef struct {
    const char *method;      /* "GET", "POST", ... */
    const char *url;         /* full URL */
    const char *headers;     /* optional additional headers, separated by \\r\\n */
    const uint8_t *body;     /* optional request body */
    size_t body_len;
    uint32_t timeout_ms;
    /* MIMI_HTTP_CALL_STREAM only: max idle time between incoming body chunks after
     * response headers (httpx `read` timeout). Timer is reset on each chunk/SSE data.
     * 0 means default 60000 ms (match httpx default read timeout). */

    uint32_t stream_read_idle_ms;

    /* Optional: if set, HTTP layer will capture these response header values
     * and return them in `mimi_http_response_t::captured_headers`.
     *
     * Lifetime note:
     * - For synchronous `mimi_http_exec()`, the pointers must remain valid for the call duration.
     * - For asynchronous `mimi_http_exec_async()`, pointers must remain valid until callback fires.
     */
    const char **capture_response_headers;
    size_t capture_response_headers_count;

    /* Optional unified async mode selector (default ONESHOT for compatibility). */
    mimi_http_call_mode_t mode;
    /* Used when mode == MIMI_HTTP_CALL_STREAM. */
    const mimi_http_stream_callbacks_t *stream_callbacks;
    mimi_http_stream_handle_t **out_stream_handle;
} mimi_http_request_t;

typedef struct {
    char *name;   /* heap-allocated */
    char *value;  /* heap-allocated, NULL if missing */
} mimi_http_header_kv_t;

typedef struct {
    int status;
    uint8_t *body;      /* heap-allocated, NUL-terminated for convenience */
    size_t body_len;
    char *content_type; /* heap-allocated Content-Type (optional) */

    /* Optional captured response headers (heap-allocated). */
    mimi_http_header_kv_t *captured_headers;
    size_t captured_headers_count;
} mimi_http_response_t;

/* HTTP callback for async operations */
typedef void (*mimi_http_callback_t)(mimi_err_t err, mimi_http_response_t *resp, void *user_data);

struct mimi_http_stream_callbacks {
    /* Called once HTTP response headers are ready (e.g. 200 + Content-Type). */
    void (*on_open)(const mimi_http_response_t *meta, void *user_data);
    /* Called for every parsed SSE event. `event_name` may be NULL when absent. */
    void (*on_sse_event)(const char *event_name, const char *data, void *user_data);
    /* Extended SSE event callback including `id:` and `retry:` (optional).
     * - `event_id` may be NULL when absent
     * - `retry_ms` is -1 when absent */
    void (*on_sse_event_ex)(const char *event_name,
                            const char *data,
                            const char *event_id,
                            int retry_ms,
                            void *user_data);
    /* Called for non-SSE body chunks (or fallback raw stream). */
    void (*on_data)(const uint8_t *data, size_t len, void *user_data);
    /* Called when stream is terminated (remote close / timeout / local stop). */
    void (*on_close)(mimi_err_t err, void *user_data);
};

mimi_err_t mimi_http_exec(const mimi_http_request_t *req, mimi_http_response_t *resp);
mimi_err_t mimi_http_exec_async(const mimi_http_request_t *req, mimi_http_response_t *resp,
                               mimi_http_callback_t callback, void *user_data);
void mimi_http_exec_async_cancel(mimi_http_stream_handle_t *stream_handle);
void mimi_http_stream_handle_release(mimi_http_stream_handle_t *stream_handle);
void mimi_http_response_free(mimi_http_response_t *resp);

/* Helper to read a captured response header value by name (case-insensitive).
 * Returns NULL if not captured or missing. */
const char *mimi_http_get_captured_header_value(const mimi_http_response_t *resp, const char *name);

mimi_err_t mimi_http_init(void);
void mimi_http_deinit(void);

#ifdef __cplusplus
}
#endif

