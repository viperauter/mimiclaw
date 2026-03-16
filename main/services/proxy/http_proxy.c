#include "http_proxy.h"
#include "config.h"
#include "config_view.h"

#include <string.h>
#include <stdlib.h>

#include "log.h"

static const char *TAG = "proxy";

/* -------------------------------------------------------------------------
 * Generic, platform-agnostic proxy configuration
 * ------------------------------------------------------------------------- */

static char     s_proxy_host[64] = {0};
static uint16_t s_proxy_port     = 0;
static char     s_proxy_type[8]  = "http";  /* "http" or "socks5" */

bool http_proxy_is_enabled(void)
{
    return s_proxy_host[0] != '\0' && s_proxy_port != 0;
}

mimi_err_t http_proxy_init(void)
{
    mimi_cfg_obj_t proxy = mimi_cfg_section("proxy");
    const char *host = mimi_cfg_get_str(proxy, "host", "");
    const char *port_s = mimi_cfg_get_str(proxy, "port", "");
    const char *type = mimi_cfg_get_str(proxy, "type", "http");

    if (host && host[0] && port_s && port_s[0]) {
        strncpy(s_proxy_host, host, sizeof(s_proxy_host) - 1);
        s_proxy_host[sizeof(s_proxy_host) - 1] = '\0';
        s_proxy_port = (uint16_t)atoi(port_s);
        if (type && type[0]) {
            strncpy(s_proxy_type, type, sizeof(s_proxy_type) - 1);
            s_proxy_type[sizeof(s_proxy_type) - 1] = '\0';
        }
    }

    if (http_proxy_is_enabled()) {
        MIMI_LOGI(TAG, "Proxy configured: %s:%u (%s)",
                  s_proxy_host, (unsigned)s_proxy_port, s_proxy_type);
    } else {
        MIMI_LOGD(TAG, "No proxy configured (direct connection)");
    }
    return MIMI_OK;
}

mimi_err_t http_proxy_set(const char *host, uint16_t port, const char *type)
{
    if (!host || !host[0] || port == 0 || !type || !type[0]) {
        return MIMI_ERR_INVALID_ARG;
    }

    strncpy(s_proxy_host, host, sizeof(s_proxy_host) - 1);
    s_proxy_port = port;
    strncpy(s_proxy_type, type, sizeof(s_proxy_type) - 1);

    MIMI_LOGI(TAG, "Proxy set (in-memory): %s:%u (%s)",
              s_proxy_host, (unsigned)s_proxy_port, s_proxy_type);
    return MIMI_OK;
}

mimi_err_t http_proxy_clear(void)
{
    s_proxy_host[0] = '\0';
    s_proxy_port = 0;
    strcpy(s_proxy_type, "http");
    MIMI_LOGI(TAG, "Proxy cleared");
    return MIMI_OK;
}

mimi_err_t http_proxy_get_config(http_proxy_config_t *out)
{
    if (!out) return MIMI_ERR_INVALID_ARG;
    if (!http_proxy_is_enabled()) return MIMI_ERR_INVALID_STATE;

    memset(out, 0, sizeof(*out));
    strncpy(out->host, s_proxy_host, sizeof(out->host) - 1);
    out->port = s_proxy_port;
    strncpy(out->type, s_proxy_type, sizeof(out->type) - 1);
    return MIMI_OK;
}

/* -------------------------------------------------------------------------
 * Proxied HTTPS connection stubs
 *
 * NOTE: For the generic platform build we currently only expose configuration.
 * Actual proxied TLS connections will be implemented later (likely using
 * the Mongoose HTTP client as the underlying transport).
 * ------------------------------------------------------------------------- */

struct proxy_conn {
    int unused;
};

proxy_conn_t *proxy_conn_open(const char *host, int port, int timeout_ms)
{
    (void)host;
    (void)port;
    (void)timeout_ms;
    if (!http_proxy_is_enabled()) {
        MIMI_LOGW(TAG, "proxy_conn_open called but proxy not configured");
    } else {
        MIMI_LOGW(TAG, "proxy_conn_open not implemented on generic platform");
    }
    return NULL;
}

int proxy_conn_write(proxy_conn_t *conn, const char *data, int len)
{
    (void)conn;
    (void)data;
    (void)len;
    MIMI_LOGW(TAG, "proxy_conn_write not implemented on generic platform");
    return -1;
}

int proxy_conn_read(proxy_conn_t *conn, char *buf, int len, int timeout_ms)
{
    (void)conn;
    (void)buf;
    (void)len;
    (void)timeout_ms;
    MIMI_LOGW(TAG, "proxy_conn_read not implemented on generic platform");
    return -1;
}

void proxy_conn_close(proxy_conn_t *conn)
{
    (void)conn;
}

