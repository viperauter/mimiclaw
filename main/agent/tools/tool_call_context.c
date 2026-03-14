/**
 * Tool Call Context Manager Implementation
 *
 * Manages tool call contexts and their lifecycle.
 */

#include "tool_call_context.h"
#include "platform/log.h"
#include "platform/os/os.h"
#include "platform/kv.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "TOOL_CTX";
/* TODO: Implement persistent storage for always allowed tools using KV store */
/* static const char *KV_KEY_ALWAYS_ALLOWED = "tools_always_allowed"; */

/* Tool context pool */
static tool_call_context_t s_contexts[TOOL_CALL_MAX_CONTEXTS];
static mimi_mutex_t *s_mutex = NULL;
static bool s_initialized = false;

/* Always allowed tools list (simple array for now) */
#define MAX_ALWAYS_ALLOWED_TOOLS 32
static char s_always_allowed[MAX_ALWAYS_ALLOWED_TOOLS][64];
static int s_always_allowed_count = 0;

mimi_err_t tool_call_context_manager_init(void)
{
    if (s_initialized) {
        return MIMI_OK;
    }
    
    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_always_allowed, 0, sizeof(s_always_allowed));
    
    mimi_err_t err = mimi_mutex_create(&s_mutex);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to create mutex: %s", mimi_err_to_name(err));
        return err;
    }
    
    /* Load always allowed tools from persistent storage */
    tool_call_context_load_always_allowed();
    
    s_initialized = true;
    MIMI_LOGI(TAG, "Tool call context manager initialized");
    
    return MIMI_OK;
}

void tool_call_context_manager_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    
    if (s_mutex) {
        mimi_mutex_lock(s_mutex);
        
        /* Clean up all active contexts */
        for (int i = 0; i < TOOL_CALL_MAX_CONTEXTS; i++) {
            if (s_contexts[i].active) {
                s_contexts[i].active = false;
                s_contexts[i].ref_count = 0;
            }
        }
        
        mimi_mutex_unlock(s_mutex);
        mimi_mutex_destroy(s_mutex);
        s_mutex = NULL;
    }
    
    s_initialized = false;
    MIMI_LOGI(TAG, "Tool call context manager deinitialized");
}

tool_call_context_t *tool_call_context_create(
    void *agent_ctx,
    const char *tool_name,
    const char *tool_call_id,
    const char *tool_input,
    bool requires_confirmation)
{
    if (!s_initialized || !tool_name || !tool_call_id) {
        return NULL;
    }
    
    mimi_mutex_lock(s_mutex);
    
    /* Find an available slot */
    tool_call_context_t *ctx = NULL;
    for (int i = 0; i < TOOL_CALL_MAX_CONTEXTS; i++) {
        if (!s_contexts[i].active) {
            ctx = &s_contexts[i];
            break;
        }
    }
    
    if (!ctx) {
        mimi_mutex_unlock(s_mutex);
        MIMI_LOGE(TAG, "No available tool context slots");
        return NULL;
    }
    
    /* Initialize the context */
    memset(ctx, 0, sizeof(*ctx));
    
    strncpy(ctx->tool_name, tool_name, sizeof(ctx->tool_name) - 1);
    strncpy(ctx->tool_call_id, tool_call_id, sizeof(ctx->tool_call_id) - 1);
    if (tool_input) {
        strncpy(ctx->tool_input, tool_input, sizeof(ctx->tool_input) - 1);
    }
    
    ctx->agent_ctx = agent_ctx;
    
    /* Initialize session context from agent context */
    if (agent_ctx) {
        /* Cast to agent_request_ctx_t to access channel and chat_id */
        typedef struct {
            char channel[64];
            char chat_id[128];
        } agent_ctx_t;
        agent_ctx_t *ctx_ptr = (agent_ctx_t *)agent_ctx;
        strncpy(ctx->session_ctx.channel, ctx_ptr->channel, sizeof(ctx->session_ctx.channel) - 1);
        strncpy(ctx->session_ctx.chat_id, ctx_ptr->chat_id, sizeof(ctx->session_ctx.chat_id) - 1);
    }
    
    ctx->requires_confirmation = requires_confirmation;
    ctx->waiting_for_confirmation = requires_confirmation;
    if (requires_confirmation) {
        ctx->confirmation_timeout = mimi_time_ms() + 30000; /* 30 seconds */
    }
    ctx->confirmation_result = CONFIRMATION_PENDING;
    ctx->active = true;
    ctx->ref_count = 1;
    
    mimi_mutex_unlock(s_mutex);
    
    MIMI_LOGI(TAG, "Tool context created: %s (id=%s)", tool_name, tool_call_id);
    
    return ctx;
}

void tool_call_context_destroy(tool_call_context_t *tool_ctx)
{
    if (!tool_ctx) {
        return;
    }
    
    mimi_mutex_lock(s_mutex);
    
    if (!tool_ctx->active) {
        mimi_mutex_unlock(s_mutex);
        return;
    }
    
    tool_ctx->ref_count--;
    
    if (tool_ctx->ref_count <= 0) {
        tool_ctx->active = false;
        MIMI_LOGI(TAG, "Tool context destroyed: %s", tool_ctx->tool_name);
    }
    
    mimi_mutex_unlock(s_mutex);
}

void tool_call_context_retain(tool_call_context_t *tool_ctx)
{
    if (!tool_ctx) {
        return;
    }
    
    mimi_mutex_lock(s_mutex);
    
    if (tool_ctx->active) {
        tool_ctx->ref_count++;
    }
    
    mimi_mutex_unlock(s_mutex);
}

tool_call_context_t *tool_call_context_find_pending(
    const char *channel,
    const char *chat_id)
{
    if (!s_initialized || !channel || !chat_id) {
        return NULL;
    }
    
    tool_call_context_t *found = NULL;
    
    mimi_mutex_lock(s_mutex);
    
    for (int i = 0; i < TOOL_CALL_MAX_CONTEXTS; i++) {
        if (s_contexts[i].active && 
            s_contexts[i].waiting_for_confirmation &&
            s_contexts[i].agent_ctx) {
            /* Compare channel and chat_id through agent_ctx */
            /* Note: This assumes agent_ctx has channel and chat_id fields */
            /* In actual implementation, you may need to adjust this */
            found = &s_contexts[i];
            break;
        }
    }
    
    if (found) {
        found->ref_count++;
    }
    
    mimi_mutex_unlock(s_mutex);
    
    return found;
}

tool_call_context_t *tool_call_context_find_by_id(const char *tool_call_id)
{
    if (!s_initialized || !tool_call_id) {
        return NULL;
    }
    
    tool_call_context_t *found = NULL;
    
    mimi_mutex_lock(s_mutex);
    
    for (int i = 0; i < TOOL_CALL_MAX_CONTEXTS; i++) {
        if (s_contexts[i].active && 
            strcmp(s_contexts[i].tool_call_id, tool_call_id) == 0) {
            found = &s_contexts[i];
            found->ref_count++;
            break;
        }
    }
    
    mimi_mutex_unlock(s_mutex);
    
    return found;
}

void tool_call_context_check_timeouts(void)
{
    if (!s_initialized) {
        return;
    }
    
    uint64_t current_time = mimi_time_ms();
    
    mimi_mutex_lock(s_mutex);
    
    for (int i = 0; i < TOOL_CALL_MAX_CONTEXTS; i++) {
        if (s_contexts[i].active && 
            s_contexts[i].waiting_for_confirmation &&
            current_time >= s_contexts[i].confirmation_timeout) {
            
            s_contexts[i].confirmation_result = CONFIRMATION_TIMEOUT;
            s_contexts[i].waiting_for_confirmation = false;
            
            MIMI_LOGW(TAG, "Tool confirmation timeout: %s", s_contexts[i].tool_name);
        }
    }
    
    mimi_mutex_unlock(s_mutex);
}

int tool_call_context_get_pending_count(void)
{
    if (!s_initialized) {
        return 0;
    }
    
    int count = 0;
    
    mimi_mutex_lock(s_mutex);
    
    for (int i = 0; i < TOOL_CALL_MAX_CONTEXTS; i++) {
        if (s_contexts[i].active && s_contexts[i].waiting_for_confirmation) {
            count++;
        }
    }
    
    mimi_mutex_unlock(s_mutex);
    
    return count;
}

int tool_call_context_get_active_count(void)
{
    if (!s_initialized) {
        return 0;
    }
    
    int count = 0;
    
    mimi_mutex_lock(s_mutex);
    
    for (int i = 0; i < TOOL_CALL_MAX_CONTEXTS; i++) {
        if (s_contexts[i].active) {
            count++;
        }
    }
    
    mimi_mutex_unlock(s_mutex);
    
    return count;
}

mimi_err_t tool_call_context_mark_always_allowed(const char *tool_name)
{
    if (!tool_name) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    mimi_mutex_lock(s_mutex);
    
    /* Check if already in list */
    for (int i = 0; i < s_always_allowed_count; i++) {
        if (strcmp(s_always_allowed[i], tool_name) == 0) {
            mimi_mutex_unlock(s_mutex);
            return MIMI_OK; /* Already in list */
        }
    }
    
    /* Add to list */
    if (s_always_allowed_count < MAX_ALWAYS_ALLOWED_TOOLS) {
        strncpy(s_always_allowed[s_always_allowed_count], tool_name, 63);
        s_always_allowed[s_always_allowed_count][63] = '\0';
        s_always_allowed_count++;
        
        mimi_mutex_unlock(s_mutex);
        
        /* Save to persistent storage */
        tool_call_context_save_always_allowed();
        
        MIMI_LOGI(TAG, "Tool marked as always allowed: %s", tool_name);
        return MIMI_OK;
    }
    
    mimi_mutex_unlock(s_mutex);
    
    return MIMI_ERR_NO_MEM;
}

bool tool_call_context_is_always_allowed(const char *tool_name)
{
    if (!s_initialized || !tool_name) {
        return false;
    }
    
    bool result = false;
    
    mimi_mutex_lock(s_mutex);
    
    for (int i = 0; i < s_always_allowed_count; i++) {
        if (strcmp(s_always_allowed[i], tool_name) == 0) {
            result = true;
            break;
        }
    }
    
    mimi_mutex_unlock(s_mutex);
    
    return result;
}

mimi_err_t tool_call_context_load_always_allowed(void)
{
    /* TODO: Implement persistent storage loading */
    /* For now, just clear the list */
    s_always_allowed_count = 0;
    memset(s_always_allowed, 0, sizeof(s_always_allowed));
    
    MIMI_LOGI(TAG, "Always allowed tools loaded: %d", s_always_allowed_count);
    
    return MIMI_OK;
}

mimi_err_t tool_call_context_save_always_allowed(void)
{
    /* TODO: Implement persistent storage saving */
    MIMI_LOGI(TAG, "Always allowed tools saved: %d", s_always_allowed_count);
    
    return MIMI_OK;
}
