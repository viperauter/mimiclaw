#pragma once

#include "tools/providers/mcp_provider_internal.h"
#include "mimi_err.h"
#include "cJSON.h"

char *mcp_tools_to_openai_json(const char *server_name, cJSON *tools_arr);
mcp_server_t *mcp_find_server_by_tool(mcp_server_t *servers, int server_count,
                                      const char *tool_name, const char **out_local_tool);
void mcp_rebuild_merged_tools_json(mcp_server_t *servers, int server_count, char **merged_cache);

typedef mimi_err_t (*mcp_start_fn_t)(mcp_server_t *s);
typedef char *(*mcp_rpc_request_fn_t)(mcp_server_t *s, const char *method, cJSON *params);
typedef mimi_err_t (*mcp_rpc_notify_fn_t)(mcp_server_t *s, const char *method, cJSON *params);
typedef long long (*mcp_now_ms_fn_t)(void);
typedef mimi_err_t (*mcp_init_fn_t)(mcp_server_t *s);

mimi_err_t mcp_core_initialize(mcp_server_t *s,
                               mcp_start_fn_t start_fn,
                               mcp_rpc_request_fn_t request_fn,
                               mcp_rpc_notify_fn_t notify_fn,
                               const char *protocol_version,
                               const char *client_name,
                               const char *client_version,
                               double process_id);
mimi_err_t mcp_core_ping_if_needed(mcp_server_t *s,
                                   mcp_now_ms_fn_t now_fn,
                                   mcp_rpc_request_fn_t request_fn,
                                   long long interval_ms);
mimi_err_t mcp_core_refresh_server_tools(mcp_server_t *s,
                                         mcp_init_fn_t init_fn,
                                         mcp_rpc_request_fn_t request_fn);
