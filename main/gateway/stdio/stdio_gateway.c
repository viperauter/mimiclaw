/**
 * STDIO Gateway Implementation
 *
 * Provides standard input/output transport for CLI channel
 * Adapted from channels/cli/terminal_stdio.c
 */

#include "gateway/stdio/stdio_gateway.h"
#include "router/router.h"
#include "cli/cli_terminal.h"
#include "log.h"
#include "os/os.h"
#include "runtime.h"
#include "mimi_time.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

static const char *TAG = "gw_stdio";
static volatile bool s_running = false;

/* STDIO Gateway private data */
typedef struct {
    bool initialized;
    bool started;
    app_terminal_t *terminal;

#ifdef _WIN32
    DWORD old_mode;
    HANDLE hstdin;
    /* UTF-8 buffer for wide character conversion */
    char utf8_buf[8];
    int utf8_len;
    int utf8_pos;
#else
    struct termios old_termios;
#endif
} stdio_gateway_priv_t;

static stdio_gateway_priv_t s_priv = {0};

/* STDIO gateway task function - runs in its own thread */
static void stdio_gateway_task(void *arg)
{
    (void)arg;
    MIMI_LOGI(TAG, "STDIO gateway task started");

    s_running = true;

    while (s_running) {
        app_terminal_poll_all();
        mimi_sleep_ms(100); /* Same interval as main event loop */
    }

    MIMI_LOGI(TAG, "STDIO gateway task stopped");
}

#ifdef _WIN32
/* Convert wide character to UTF-8 */
static void wide_to_utf8(stdio_gateway_priv_t *ctx, wchar_t wc)
{
    if (wc < 0x80) {
        ctx->utf8_buf[0] = (char)wc;
        ctx->utf8_len = 1;
    } else if (wc < 0x800) {
        ctx->utf8_buf[0] = (char)(0xC0 | (wc >> 6));
        ctx->utf8_buf[1] = (char)(0x80 | (wc & 0x3F));
        ctx->utf8_len = 2;
    } else {
        /* On Windows, wchar_t is 16-bit (UTF-16), max value is 0xFFFF */
        /* For values >= 0x10000, they are represented as surrogate pairs */
        ctx->utf8_buf[0] = (char)(0xE0 | (wc >> 12));
        ctx->utf8_buf[1] = (char)(0x80 | ((wc >> 6) & 0x3F));
        ctx->utf8_buf[2] = (char)(0x80 | (wc & 0x3F));
        ctx->utf8_len = 3;
    }
    ctx->utf8_pos = 0;
}
#endif

/* Transport read function */
static int stdio_transport_read(void *ctx, char *buf, int len)
{
    (void)ctx;
    if (!buf || len <= 0) return 0;

#ifdef _WIN32
    stdio_gateway_priv_t *priv = (stdio_gateway_priv_t *)ctx;
    int n = 0;

    /* Return remaining UTF-8 bytes from previous wide char */
    while (n < len && priv->utf8_pos < priv->utf8_len) {
        buf[n++] = priv->utf8_buf[priv->utf8_pos++];
    }
    if (n > 0) return n;

    /* Use ReadConsoleInputW for Unicode handling */
    DWORD numEvents;
    if (!GetNumberOfConsoleInputEvents(priv->hstdin, &numEvents) || numEvents == 0) {
        return 0;
    }

    while (n < len && numEvents > 0) {
        INPUT_RECORD ir;
        DWORD numRead;
        if (!ReadConsoleInputW(priv->hstdin, &ir, 1, &numRead) || numRead == 0) {
            break;
        }
        numEvents--;

        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
            wchar_t wc = ir.Event.KeyEvent.uChar.UnicodeChar;
            if (wc != 0) {
                wide_to_utf8(priv, wc);
                while (n < len && priv->utf8_pos < priv->utf8_len) {
                    buf[n++] = priv->utf8_buf[priv->utf8_pos++];
                }
            }
        }
    }
    return n;
#else
    /* POSIX: use read() in non-blocking mode */
    int n = read(STDIN_FILENO, buf, len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
    return n;
#endif
}

/* Transport write function */
static int stdio_transport_write(void *ctx, const char *buf, int len)
{
    (void)ctx;
    if (!buf || len <= 0) return 0;

    int written = fwrite(buf, 1, len, stdout);
    fflush(stdout);
    return written;
}

/* Transport available check */
static bool stdio_transport_available(void *ctx)
{
#ifdef _WIN32
    stdio_gateway_priv_t *priv = (stdio_gateway_priv_t *)ctx;
    if (priv->utf8_pos < priv->utf8_len) {
        return true;
    }
    DWORD numEvents;
    return GetNumberOfConsoleInputEvents(priv->hstdin, &numEvents) && numEvents > 0;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
#endif
}

/* Transport close function */
static void stdio_transport_close(void *ctx)
{
    stdio_gateway_priv_t *priv = (stdio_gateway_priv_t *)ctx;
    if (!priv || !priv->initialized) return;

#ifdef _WIN32
    SetConsoleMode(priv->hstdin, priv->old_mode);
    SetConsoleOutputCP(priv->old_mode);
#else
    tcsetattr(STDIN_FILENO, TCSANOW, &priv->old_termios);
#endif

    priv->initialized = false;
    MIMI_LOGI(TAG, "STDIO transport closed");
}

/* Initialize terminal settings */
static mimi_err_t stdio_terminal_init(stdio_gateway_priv_t *priv)
{
    if (priv->initialized) return MIMI_OK;

#ifdef _WIN32
    priv->hstdin = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(priv->hstdin, &priv->old_mode);
    SetConsoleOutputCP(CP_UTF8);

    DWORD new_mode = priv->old_mode;
    new_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    new_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(priv->hstdin, new_mode);
#else
    tcgetattr(STDIN_FILENO, &priv->old_termios);

    struct termios new_termios = priv->old_termios;
    new_termios.c_lflag &= ~(ICANON | ECHO);
    new_termios.c_cc[VMIN] = 0;
    new_termios.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
#endif

    priv->initialized = true;
    return MIMI_OK;
}

/* Gateway implementation functions */

static mimi_err_t stdio_gateway_init_impl(gateway_t *gw, const gateway_config_t *cfg)
{
    (void)cfg;

    if (s_priv.initialized) {
        MIMI_LOGW(TAG, "STDIO Gateway already initialized");
        return MIMI_OK;
    }

    /* Initialize terminal settings */
    mimi_err_t err = stdio_terminal_init(&s_priv);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to initialize terminal");
        return err;
    }

    /* Initialize CLI terminal framework */
    err = app_terminal_init();
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to init CLI terminal framework");
        return err;
    }

    gw->priv_data = &s_priv;
    MIMI_LOGI(TAG, "STDIO Gateway initialized");
    return MIMI_OK;
}

static mimi_err_t stdio_gateway_start_impl(gateway_t *gw)
{
    (void)gw;

    if (!s_priv.initialized) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (s_priv.started) {
        return MIMI_OK;
    }

    /* Create transport interface */
    cli_transport_t transport = {
        .read = stdio_transport_read,
        .write = stdio_transport_write,
        .available = stdio_transport_available,
        .close = stdio_transport_close,
        .ctx = &s_priv
    };

    /* Create terminal */
    app_terminal_config_t config = {
        .name = "stdio",
        .channel = "cli",
        .chat_id = "default",
        .transport = transport
    };

    s_priv.terminal = app_terminal_create(&config);
    if (!s_priv.terminal) {
        MIMI_LOGE(TAG, "Failed to create terminal");
        return MIMI_ERR_NO_MEM;
    }

    s_priv.started = true;
    
    /* Create detached task for STDIO polling */
    mimi_err_t err = mimi_task_create_detached("stdio_gw", stdio_gateway_task, NULL);
    if (err != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to create STDIO gateway task: %d", err);
        s_priv.started = false;
        app_terminal_destroy(s_priv.terminal);
        s_priv.terminal = NULL;
        return err;
    }
    
    MIMI_LOGI(TAG, "STDIO Gateway started");
    return MIMI_OK;
}

static mimi_err_t stdio_gateway_stop_impl(gateway_t *gw)
{
    (void)gw;

    if (!s_priv.started) {
        return MIMI_OK;
    }

    /* Stop the task first */
    s_running = false;

    /* Wait for the task to stop */
    mimi_sleep_ms(50);

    if (s_priv.terminal) {
        app_terminal_destroy(s_priv.terminal);
        s_priv.terminal = NULL;
    }

    stdio_transport_close(&s_priv);
    s_priv.started = false;

    MIMI_LOGI(TAG, "STDIO Gateway stopped");
    return MIMI_OK;
}

static void stdio_gateway_destroy_impl(gateway_t *gw)
{
    (void)gw;
    stdio_gateway_stop_impl(gw);
    memset(&s_priv, 0, sizeof(s_priv));
    MIMI_LOGI(TAG, "STDIO Gateway destroyed");
}

static mimi_err_t stdio_gateway_send_impl(gateway_t *gw, const char *session_id,
                                           const char *content)
{
    (void)gw;
    (void)session_id;

    if (!s_priv.started || !s_priv.terminal) {
        return MIMI_ERR_INVALID_STATE;
    }

    if (!content) {
        return MIMI_ERR_INVALID_ARG;
    }

    app_terminal_output_ln(s_priv.terminal, content);
    return MIMI_OK;
}

/* Global STDIO Gateway instance */
gateway_t g_stdio_gateway = {
    .name = "stdio",
    .type = GATEWAY_TYPE_STDIO,
    .init = stdio_gateway_init_impl,
    .start = stdio_gateway_start_impl,
    .stop = stdio_gateway_stop_impl,
    .destroy = stdio_gateway_destroy_impl,
    .send = stdio_gateway_send_impl,
    .set_on_message = NULL,  /* Not used - CLI uses editor callbacks */
    .set_on_connect = NULL,
    .set_on_disconnect = NULL,
    .is_initialized = false,
    .is_started = false,
    .priv_data = NULL,
    .on_message_cb = NULL,
    .on_connect_cb = NULL,
    .on_disconnect_cb = NULL,
    .callback_user_data = NULL
};

mimi_err_t stdio_gateway_module_init(void)
{
    /* Nothing special needed - the global instance is ready */
    return MIMI_OK;
}

gateway_t* stdio_gateway_get(void)
{
    return &g_stdio_gateway;
}
