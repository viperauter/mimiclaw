#pragma once

#include <stdint.h>
#include "mimi_config.h"
#include "mimi_err.h"

/* Channel identifiers */
#define MIMI_CHAN_TELEGRAM   "telegram"
#define MIMI_CHAN_WEBSOCKET  "websocket"
#define MIMI_CHAN_CLI        "cli"
#define MIMI_CHAN_SYSTEM     "system"

/* Message types on the bus */
typedef enum {
    MIMI_MSG_TYPE_TEXT = 0,         /* Normal text message */
    MIMI_MSG_TYPE_CONTROL,          /* Control message (generic) */
    MIMI_MSG_TYPE_TOOL_RESULT,      /* Tool execution result */
    MIMI_MSG_TYPE_STATUS,           /* Generic status update (start/progress/done) */
} mimi_msg_type_t;

/* Status phases for MIMI_MSG_TYPE_STATUS messages. */
typedef enum {
    MIMI_STATUS_PHASE_START = 0,    /* Start of an operation */
    MIMI_STATUS_PHASE_PROGRESS,     /* In-progress update */
    MIMI_STATUS_PHASE_DONE,         /* Operation completed */
} mimi_status_phase_t;

/* Control message types */
typedef enum {
    MIMI_CONTROL_TYPE_CONFIRM = 0,  /* Confirmation request */
    MIMI_CONTROL_TYPE_CANCEL,       /* Cancel operation */
    MIMI_CONTROL_TYPE_STOP,         /* Stop operation */
    MIMI_CONTROL_TYPE_STATUS,       /* Status query */
} mimi_control_type_t;

typedef struct {
    char channel[MIMI_CHANNEL_NAME_LEN];  /* "telegram", "websocket", "cli" */
    char chat_id[MIMI_CHAT_ID_LEN];       /* Telegram/Feishu chat_id or WS client id */
    char *content;                  /* Heap-allocated message text (caller must free) */
    mimi_msg_type_t type;           /* Message type */
    
    /* Control message specific fields */
    mimi_control_type_t control_type;   /* Control type (for CONTROL messages) */
    char request_id[MIMI_CONTROL_REQUEST_ID_LEN];
    char target[MIMI_MAX_TOOL_NAME_LEN]; /* Target (e.g., tool name, operation ID) */
    char data[MIMI_MSG_CONTROL_DATA_LEN]; /* Additional data (e.g. tool params) */

    /* Status message specific fields (for MIMI_MSG_TYPE_STATUS). */
    mimi_status_phase_t status_phase;   /* Phase of the status update */
    char status_key[MIMI_MSG_STATUS_KEY_LEN];
} mimi_msg_t;

/**
 * Initialize the message bus (inbound + outbound FreeRTOS queues).
 */
mimi_err_t message_bus_init(void);

/**
 * Push a message to the inbound queue (towards Agent Loop).
 * The bus takes ownership of msg->content.
 */
mimi_err_t message_bus_push_inbound(const mimi_msg_t *msg);

/**
 * Pop a message from the inbound queue (blocking).
 * Caller must free msg->content when done.
 */
mimi_err_t message_bus_pop_inbound(mimi_msg_t *msg, uint32_t timeout_ms);

/**
 * Push a message to the outbound queue (towards channels).
 * The bus takes ownership of msg->content.
 */
mimi_err_t message_bus_push_outbound(const mimi_msg_t *msg);

/**
 * Pop a message from the outbound queue (blocking).
 * Caller must free msg->content when done.
 */
mimi_err_t message_bus_pop_outbound(mimi_msg_t *msg, uint32_t timeout_ms);
