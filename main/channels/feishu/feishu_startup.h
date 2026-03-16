#pragma once

#include "mimi_err.h"
#include "channels/channel.h"

/* Kick off the non-blocking startup state machine. */
void feishu_sm_start(channel_t *ch);

