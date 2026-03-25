#pragma once

#include "tools/tool_provider.h"

const mimi_tool_provider_t *mcp_provider_get(void);

/* Request background discovery of configured MCP servers and refresh tool
 * registry JSON. Safe to call multiple times; only one discovery task runs
 * at a time. */
void mcp_provider_request_refresh(int max_attempts, int delay_ms);
