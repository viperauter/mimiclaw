// Minimal standalone Mongoose WebSocket client for debugging Feishu WS
//
// Build example (from repo root, adjust include/lib paths as needed):
//   gcc -Ithird_party/mongoose -Imain/platform -o ws_mg_test \
//       tools/ws_mg_test.c third_party/mongoose/mongoose.c
//
// Run example:
//   ./ws_mg_test "wss://msg-frontier.feishu.cn/ws/v2?..." 
//
// Optional: override DNS via env:
//   MIMI_DNS_SERVER=114.114.114.114 ./ws_mg_test "wss://..."

#include "mongoose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *s_url = NULL;
static int s_connected = 0;

static void ws_fn(struct mg_connection *c, int ev, void *ev_data) {
  (void) ev_data;

  if (ev == MG_EV_OPEN) {
    /* Enable hexdump so we can see raw HTTP handshake */
    c->is_hexdumping = 1;
    MG_INFO(("EV_OPEN, fd=%d", (int) c->fd));
  } else if (ev == MG_EV_CONNECT) {
    int status = 0;
    if (ev_data != NULL) status = *(int *) ev_data;
    MG_INFO(("EV_CONNECT status=%d", status));
  } else if (ev == MG_EV_ERROR) {
    MG_ERROR(("EV_ERROR: %s", (char *) ev_data));
  } else if (ev == MG_EV_TLS_HS) {
    MG_INFO(("EV_TLS_HS done"));
  } else if (ev == MG_EV_WS_OPEN) {
    MG_INFO(("EV_WS_OPEN: WebSocket handshake ok"));
    s_connected = 1;
  } else if (ev == MG_EV_WS_MSG) {
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
    MG_INFO(("EV_WS_MSG: binary=%d len=%d",
             wm->flags & WEBSOCKET_OP_BINARY ? 1 : 0, (int) wm->data.len));
  } else if (ev == MG_EV_CLOSE) {
    MG_INFO(("EV_CLOSE"));
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s wss_url\n", argv[0]);
    return 1;
  }
  s_url = argv[1];

  struct mg_mgr mgr;
  mg_mgr_init(&mgr);

  // Optional: override DNS via env, same env var as runtime
  const char *dns = getenv("MIMI_DNS_SERVER");
  if (dns && dns[0]) {
    static char dns_url[64];
    snprintf(dns_url, sizeof(dns_url), "udp://%s:53", dns);
    mgr.dns4.url = dns_url;
    MG_INFO(("Using custom DNS server: %s", dns_url));
  }

  // Use DEBUG log level to see full handshake
  mg_log_set(MG_LL_DEBUG);

  MG_INFO(("Connecting to %s", s_url));
  struct mg_connection *c = mg_ws_connect(&mgr, s_url, ws_fn, NULL, NULL);
  if (!c) {
    MG_ERROR(("mg_ws_connect failed"));
    mg_mgr_free(&mgr);
    return 2;
  }

  // If URL is wss://, initialise TLS (SNI) explicitly, same方式 as项目里的 HTTP/WS 客户端
  if (mg_url_is_ssl(s_url)) {
    struct mg_str host = mg_url_host(s_url);
    char host_name[128] = {0};
    if (host.len > 0 && host.len < (int) sizeof(host_name)) {
      memcpy(host_name, host.buf, host.len);
      host_name[host.len] = '\0';

      struct mg_tls_opts opts;
      memset(&opts, 0, sizeof(opts));
      opts.name = mg_str(host_name);   // SNI: msg-frontier.feishu.cn
      opts.skip_verification = 1;      // 开发环境先跳过证书校验
      mg_tls_init(c, &opts);

      MG_INFO(("Initialized TLS for WS with host=%s", host_name));
    }
  }

  // Poll for up to 60 seconds
  uint64_t start = mg_millis();
  while ((mg_millis() - start) < 60000 && c != NULL) {
    mg_mgr_poll(&mgr, 100);
  }

  MG_INFO(("Done, connected=%d", s_connected));
  mg_mgr_free(&mgr);
  return 0;
}

