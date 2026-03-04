#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "platform/mimi_err.h"

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
} mimi_http_response_t;

mimi_err_t mimi_http_exec(const mimi_http_request_t *req, mimi_http_response_t *resp);
void mimi_http_response_free(mimi_http_response_t *resp);

#ifdef __cplusplus
}
#endif

