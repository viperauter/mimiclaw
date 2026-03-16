#pragma once

#include <stddef.h>

#include "gateway/gateway.h"

/* Feishu WS gateway message handler. */
void feishu_on_ws_message(gateway_t *gw,
                          const char *session_id,
                          const char *content,
                          size_t content_len,
                          void *user_data);

