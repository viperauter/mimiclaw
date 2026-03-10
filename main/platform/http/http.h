#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "mimi_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *method;      /* "GET", "POST", ... */
    const char *url;         /* full URL */
    const char *headers;     /* optional additional headers, separated by \\r\\n */
    const uint8_t *body;     /* optional request body */
    size_t body_len;
    uint32_t timeout_ms;
} mimi_http_request_t;

typedef struct {
    int status;
    uint8_t *body;      /* heap-allocated, NUL-terminated for convenience */
    size_t body_len;
    char *content_type; /* heap-allocated Content-Type (optional) */
} mimi_http_response_t;

/* HTTP callback for async operations */
typedef void (*mimi_http_callback_t)(mimi_err_t err, mimi_http_response_t *resp, void *user_data);

mimi_err_t mimi_http_exec(const mimi_http_request_t *req, mimi_http_response_t *resp);
mimi_err_t mimi_http_exec_async(const mimi_http_request_t *req, mimi_http_response_t *resp,
                               mimi_http_callback_t callback, void *user_data);
void mimi_http_response_free(mimi_http_response_t *resp);
mimi_err_t mimi_http_init(void);
void mimi_http_deinit(void);

#ifdef __cplusplus
}
#endif

