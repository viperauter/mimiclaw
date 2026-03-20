/* FFF (Fake Function Framework) Mocks - Minimal Version */
#ifndef FFF_MOCKS_H
#define FFF_MOCKS_H

#include <stdbool.h>
#include "fff.h"

/* Basic type definitions */
#include "mimi_err.h"

/* Forward declaration of opaque types */
struct mimi_cfg_obj { const void *node; };
typedef struct mimi_cfg_obj mimi_cfg_obj_t;

/* Opaque pointer types (for fake function signatures) */
typedef void* mimi_mutex_t;
typedef void* mimi_cond_t;
typedef void* mimi_task_handle_t;
typedef void* mimi_timer_handle_t;

/* Use real LLM types so we don't conflict with `llm_proxy.h` typedefs. */
#include "llm/llm_proxy.h"

/* Task function type */
typedef void (*mimi_task_fn_t)(void *arg);

/* =========================================================================
 * FFF fake function declarations only (no definitions)
 * ========================================================================= */

/* Tool Provider */
DECLARE_FAKE_VALUE_FUNC0(mimi_err_t, tool_provider_registry_init);
DECLARE_FAKE_VOID_FUNC0(tool_provider_registry_deinit);
DECLARE_FAKE_VALUE_FUNC1(mimi_err_t, tool_provider_register, const void *);
DECLARE_FAKE_VALUE_FUNC0(const char *, tool_provider_get_all_tools_json);
DECLARE_FAKE_VALUE_FUNC0(const void *, mcp_stdio_provider_get);

/* Config */
DECLARE_FAKE_VALUE_FUNC1(mimi_cfg_obj_t, mimi_cfg_section, const char *);
DECLARE_FAKE_VALUE_FUNC2(mimi_cfg_obj_t, mimi_cfg_get_obj, mimi_cfg_obj_t, const char *);
DECLARE_FAKE_VALUE_FUNC3(const char *, mimi_cfg_get_str, mimi_cfg_obj_t, const char *, const char *);

/* LLM Proxy */
DECLARE_FAKE_VALUE_FUNC0(mimi_err_t, llm_proxy_init);
DECLARE_FAKE_VALUE_FUNC2(mimi_err_t, llm_chat_tools_req, const llm_chat_req_t *, llm_response_t *);
DECLARE_FAKE_VALUE_FUNC4(mimi_err_t, llm_chat_tools_async_req, const llm_chat_req_t *, llm_response_t *, llm_callback_t, void *);
DECLARE_FAKE_VOID_FUNC1(llm_response_free, llm_response_t *);
DECLARE_FAKE_VALUE_FUNC0(const char *, llm_get_last_error);

/* Additional stub declarations */
mimi_err_t tool_provider_execute(const char *name, const char *in, char *out, size_t out_sz, const void *ctx);
bool tool_provider_requires_confirmation(const char *name);
const char *tool_web_search_schema_json(void);

/* =========================================================================
 * Helper Macros
 * ========================================================================= */

/* Reset all fake functions */
#define FFF_RESET_ALL() \
    do { \
        /* Tool Provider */ \
        RESET_FAKE(tool_provider_registry_init); \
        RESET_FAKE(tool_provider_registry_deinit); \
        RESET_FAKE(tool_provider_register); \
        RESET_FAKE(tool_provider_get_all_tools_json); \
        RESET_FAKE(mcp_stdio_provider_get); \
        /* Config */ \
        RESET_FAKE(mimi_cfg_section); \
        RESET_FAKE(mimi_cfg_get_obj); \
        RESET_FAKE(mimi_cfg_get_str); \
        /* LLM Proxy */ \
        RESET_FAKE(llm_proxy_init); \
        RESET_FAKE(llm_chat_tools_req); \
        RESET_FAKE(llm_chat_tools_async_req); \
        RESET_FAKE(llm_response_free); \
        RESET_FAKE(llm_get_last_error); \
    } while(0)

#endif /* FFF_MOCKS_H */
