#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "mimi_err.h"

typedef struct {
    char     host[64];
    uint16_t port;
    char     type[8];   /* "http" or "socks5" */
} http_proxy_config_t;

/**
 * Initialize proxy module.
 * Loads runtime defaults from config.json if present.
 */
mimi_err_t http_proxy_init(void);

/**
 * Returns true if a proxy host:port is configured.
 */
bool http_proxy_is_enabled(void);

/**
 * Save proxy host, port, and type to in-memory config.
 * Current generic implementation does NOT persist this to NVS/FS.
 */
mimi_err_t http_proxy_set(const char *host, uint16_t port, const char *type);

/**
 * Clear proxy configuration (reset to "no proxy").
 */
mimi_err_t http_proxy_clear(void);

/**
 * Get current proxy configuration.
 * Returns MIMI_ERR_INVALID_STATE if no proxy is configured.
 */
mimi_err_t http_proxy_get_config(http_proxy_config_t *out);

/* ── Proxied HTTPS connection ─────────────────────────────────── */

typedef struct proxy_conn proxy_conn_t;

/**
 * Open an HTTPS connection through the configured proxy.
 * 1) TCP connect to proxy
 * 2) Proxy handshake (HTTP CONNECT / SOCKS5)
 * 3) TLS handshake over the tunnel
 *
 * Returns NULL on failure.
 *
 * NOTE: On non-ESP platforms this may be unimplemented initially and will
 * return NULL with an error log.
 */
proxy_conn_t *proxy_conn_open(const char *host, int port, int timeout_ms);

/** Write raw bytes through the TLS tunnel. Returns bytes written or -1. */
int proxy_conn_write(proxy_conn_t *conn, const char *data, int len);

/** Read raw bytes from the TLS tunnel. Returns bytes read or -1. */
int proxy_conn_read(proxy_conn_t *conn, char *buf, int len, int timeout_ms);

/** Close and free the connection. */
void proxy_conn_close(proxy_conn_t *conn);
