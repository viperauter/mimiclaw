#pragma once

#include "mimi_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Append a structured trace event (JSONL).
 *
 * All events include: ts_ms, trace_id, event.
 * Additional fields are optional; large fields may be truncated based on config.
 *
 * This function is a no-op when tracing is disabled in config.
 */
mimi_err_t llm_trace_event_kv(const char *trace_id,
                              const char *event,
                              const char *k1, const char *v1,
                              const char *k2, const char *v2,
                              const char *k3, const char *v3,
                              const char *k4, const char *v4);

/**
 * Append a structured trace event with a free-form JSON string payload.
 * The payload is stored under "json" (string) and may be truncated.
 */
mimi_err_t llm_trace_event_json(const char *trace_id,
                                const char *event,
                                const char *json_str);

/**
 * Helper: generate a unique trace id string.
 * Output is always NUL-terminated.
 */
void llm_trace_make_id(char *out, size_t out_size);

/**
 * Bind a trace_id to a session context (channel/chat_id) for routing trace output.
 *
 * When tracing.mode is set to "perSession", events for a bound trace_id will be
 * written under: <tracing.dir>/<channel>/<chat_id>/chat_trace.jsonl
 *
 * This is best-effort. If no binding exists (or mode is "single"), traces fall
 * back to the global rolling file.
 */
void llm_trace_bind_session(const char *trace_id, const char *channel, const char *chat_id);

#ifdef __cplusplus
}
#endif

