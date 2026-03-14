/**
 * Control Manager Implementation
 * 
 * Manages control requests and responses for the generic control channel.
 */

#include "control_manager.h"
#include "platform/log.h"
#include "platform/os/os.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "CTRL_MGR";

/* Control request pool */
static control_request_t s_requests[CONTROL_MAX_REQUESTS] = {0};
static mimi_mutex_t *s_mutex = NULL;
static bool s_initialized = false;

/* Generate a unique request ID using timestamp and random number */
void control_manager_generate_request_id(char *out_id, size_t id_len)
{
    if (!out_id || id_len == 0) {
        return;
    }
    
    uint64_t timestamp = mimi_time_ms();
    uint32_t random_val = (uint32_t)(timestamp ^ (timestamp >> 32));
    
    snprintf(out_id, id_len, "req_%016llx_%08x", 
             (unsigned long long)timestamp, random_val);
}

mimi_err_t control_manager_init(void)
{
    if (s_initialized) {
        return MIMI_OK;
    }
    
    memset(s_requests, 0, sizeof(s_requests));
    
    mimi_err_t err = mimi_mutex_create(&s_mutex);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to create mutex: %s", mimi_err_to_name(err));
        return err;
    }
    
    s_initialized = true;
    MIMI_LOGI(TAG, "Control manager initialized");
    
    return MIMI_OK;
}

void control_manager_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    
    if (s_mutex) {
        mimi_mutex_lock(s_mutex);
        
        /* Cancel all active requests */
        for (int i = 0; i < CONTROL_MAX_REQUESTS; i++) {
            if (s_requests[i].active) {
                s_requests[i].active = false;
                if (s_requests[i].callback) {
                    s_requests[i].callback(s_requests[i].request_id, "CANCELLED", 
                                          s_requests[i].context);
                }
            }
        }
        
        mimi_mutex_unlock(s_mutex);
        mimi_mutex_destroy(s_mutex);
        s_mutex = NULL;
    }
    
    s_initialized = false;
    MIMI_LOGI(TAG, "Control manager deinitialized");
}

mimi_err_t control_manager_send_request(
    const char *channel,
    const char *chat_id,
    mimi_control_type_t control_type,
    const char *target,
    const char *data,
    void *context,
    void (*callback)(const char *request_id, const char *response, void *context),
    char *out_request_id)
{
    if (!s_initialized) {
        return MIMI_ERR_INVALID_STATE;
    }
    
    if (!channel || !chat_id || !callback || !out_request_id) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    mimi_mutex_lock(s_mutex);
    
    /* Find an available slot */
    control_request_t *req = NULL;
    for (int i = 0; i < CONTROL_MAX_REQUESTS; i++) {
        if (!s_requests[i].active) {
            req = &s_requests[i];
            break;
        }
    }
    
    if (!req) {
        mimi_mutex_unlock(s_mutex);
        MIMI_LOGE(TAG, "No available control request slots");
        return MIMI_ERR_NO_MEM;
    }
    
    /* Fill in the request */
    memset(req, 0, sizeof(*req));
    control_manager_generate_request_id(req->request_id, sizeof(req->request_id));
    strncpy(req->channel, channel, sizeof(req->channel) - 1);
    strncpy(req->chat_id, chat_id, sizeof(req->chat_id) - 1);
    req->control_type = control_type;
    if (target) {
        strncpy(req->target, target, sizeof(req->target) - 1);
    }
    req->context = context;
    req->timeout = mimi_time_ms() + CONTROL_DEFAULT_TIMEOUT_MS;
    req->callback = callback;
    req->active = true;
    
    /* Copy request ID to output */
    strncpy(out_request_id, req->request_id, CONTROL_REQUEST_ID_LEN - 1);
    out_request_id[CONTROL_REQUEST_ID_LEN - 1] = '\0';
    
    mimi_mutex_unlock(s_mutex);
    
    /* Build and send control message */
    mimi_msg_t msg = {0};
    strncpy(msg.channel, channel, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
    msg.type = MIMI_MSG_TYPE_CONTROL;
    msg.control_type = control_type;
    strncpy(msg.request_id, req->request_id, sizeof(msg.request_id) - 1);
    strncpy(msg.target, req->target, sizeof(msg.target) - 1);
    if (data) {
        strncpy(msg.data, data, sizeof(msg.data) - 1);
    }
    
    /* Build content based on control type */
    char content[512];
    switch (control_type) {
        case MIMI_CONTROL_TYPE_CONFIRM:
            snprintf(content, sizeof(content), 
                     "Confirmation required for: %s\n%s", target, data ? data : "");
            break;
        case MIMI_CONTROL_TYPE_STOP:
            snprintf(content, sizeof(content), 
                     "Stop operation: %s", target);
            break;
        case MIMI_CONTROL_TYPE_STATUS:
            snprintf(content, sizeof(content), 
                     "Status query: %s", target);
            break;
        default:
            snprintf(content, sizeof(content), 
                     "Control request: %s", target);
            break;
    }
    msg.content = strdup(content);
    
    mimi_err_t err = message_bus_push_outbound(&msg);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to push control message: %s", mimi_err_to_name(err));
        /* Mark request as inactive since we couldn't send it */
        mimi_mutex_lock(s_mutex);
        req->active = false;
        mimi_mutex_unlock(s_mutex);
        free(msg.content);
        return err;
    }
    
    MIMI_LOGI(TAG, "Control request sent: type=%d, target=%s, id=%s", 
              control_type, target, req->request_id);
    
    return MIMI_OK;
}

mimi_err_t control_manager_handle_response(const char *request_id, const char *response)
{
    if (!s_initialized || !request_id || !response) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    mimi_mutex_lock(s_mutex);
    
    /* Find the request */
    control_request_t *req = NULL;
    for (int i = 0; i < CONTROL_MAX_REQUESTS; i++) {
        if (s_requests[i].active && 
            strcmp(s_requests[i].request_id, request_id) == 0) {
            req = &s_requests[i];
            break;
        }
    }
    
    if (!req) {
        mimi_mutex_unlock(s_mutex);
        MIMI_LOGW(TAG, "Response for unknown request: %s", request_id);
        return MIMI_ERR_NOT_FOUND;
    }
    
    /* Store callback and context before releasing the slot */
    void (*callback)(const char *, const char *, void *) = req->callback;
    void *context = req->context;
    char req_id_copy[CONTROL_REQUEST_ID_LEN];
    strncpy(req_id_copy, req->request_id, sizeof(req_id_copy) - 1);
    req_id_copy[sizeof(req_id_copy) - 1] = '\0';
    
    /* Mark as inactive */
    req->active = false;
    
    mimi_mutex_unlock(s_mutex);
    
    /* Call the callback */
    if (callback) {
        callback(req_id_copy, response, context);
    }
    
    MIMI_LOGI(TAG, "Control response handled: id=%s, response=%s", 
              request_id, response);
    
    return MIMI_OK;
}

mimi_err_t control_manager_handle_response_by_chat_id(const char *chat_id, const char *response)
{
    if (!s_initialized || !chat_id || !response) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    mimi_mutex_lock(s_mutex);
    
    /* Find the most recent pending request for this chat ID */
    control_request_t *req = NULL;
    for (int i = 0; i < CONTROL_MAX_REQUESTS; i++) {
        if (s_requests[i].active && 
            strcmp(s_requests[i].chat_id, chat_id) == 0) {
            req = &s_requests[i];
            /* Continue searching to find the most recent one */
        }
    }
    
    if (!req) {
        mimi_mutex_unlock(s_mutex);
        MIMI_LOGW(TAG, "Response for unknown chat: %s", chat_id);
        return MIMI_ERR_NOT_FOUND;
    }
    
    /* Store callback and context before releasing the slot */
    void (*callback)(const char *, const char *, void *) = req->callback;
    void *context = req->context;
    char req_id_copy[CONTROL_REQUEST_ID_LEN];
    strncpy(req_id_copy, req->request_id, sizeof(req_id_copy) - 1);
    req_id_copy[sizeof(req_id_copy) - 1] = '\0';
    
    /* Mark as inactive */
    req->active = false;
    
    mimi_mutex_unlock(s_mutex);
    
    /* Call the callback */
    if (callback) {
        callback(req_id_copy, response, context);
    }
    
    MIMI_LOGI(TAG, "Control response handled by chat: chat=%s, response=%s", 
              chat_id, response);
    
    return MIMI_OK;
}

void control_manager_check_timeouts(void)
{
    if (!s_initialized) {
        return;
    }
    
    uint64_t current_time = mimi_time_ms();
    
    mimi_mutex_lock(s_mutex);
    
    for (int i = 0; i < CONTROL_MAX_REQUESTS; i++) {
        if (s_requests[i].active && current_time >= s_requests[i].timeout) {
            control_request_t *req = &s_requests[i];
            
            MIMI_LOGW(TAG, "Control request timeout: id=%s", req->request_id);
            
            /* Store callback and context */
            void (*callback)(const char *, const char *, void *) = req->callback;
            void *context = req->context;
            char req_id_copy[CONTROL_REQUEST_ID_LEN];
            strncpy(req_id_copy, req->request_id, sizeof(req_id_copy) - 1);
            req_id_copy[sizeof(req_id_copy) - 1] = '\0';
            
            /* Mark as inactive */
            req->active = false;
            
            mimi_mutex_unlock(s_mutex);
            
            /* Call callback with timeout */
            if (callback) {
                callback(req_id_copy, "TIMEOUT", context);
            }
            
            mimi_mutex_lock(s_mutex);
        }
    }
    
    mimi_mutex_unlock(s_mutex);
}

mimi_err_t control_manager_cancel_request(const char *request_id)
{
    if (!s_initialized || !request_id) {
        return MIMI_ERR_INVALID_ARG;
    }
    
    mimi_mutex_lock(s_mutex);
    
    for (int i = 0; i < CONTROL_MAX_REQUESTS; i++) {
        if (s_requests[i].active && 
            strcmp(s_requests[i].request_id, request_id) == 0) {
            control_request_t *req = &s_requests[i];
            
            /* Store callback and context */
            void (*callback)(const char *, const char *, void *) = req->callback;
            void *context = req->context;
            char req_id_copy[CONTROL_REQUEST_ID_LEN];
            strncpy(req_id_copy, req->request_id, sizeof(req_id_copy) - 1);
            req_id_copy[sizeof(req_id_copy) - 1] = '\0';
            
            /* Mark as inactive */
            req->active = false;
            
            mimi_mutex_unlock(s_mutex);
            
            /* Call callback with cancelled */
            if (callback) {
                callback(req_id_copy, "CANCELLED", context);
            }
            
            MIMI_LOGI(TAG, "Control request cancelled: id=%s", request_id);
            return MIMI_OK;
        }
    }
    
    mimi_mutex_unlock(s_mutex);
    
    return MIMI_ERR_NOT_FOUND;
}

int control_manager_get_active_count(void)
{
    if (!s_initialized) {
        return 0;
    }
    
    int count = 0;
    
    mimi_mutex_lock(s_mutex);
    
    for (int i = 0; i < CONTROL_MAX_REQUESTS; i++) {
        if (s_requests[i].active) {
            count++;
        }
    }
    
    mimi_mutex_unlock(s_mutex);
    
    return count;
}
