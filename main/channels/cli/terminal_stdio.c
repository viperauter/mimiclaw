/**
 * STDIO CLI implementation using transport interface
 * 
 * Moved from main/cli/ to main/channels/cli/ as part of Channel architecture refactor
 */

#include "channels/cli/terminal_stdio.h"
#include "cli/cli_terminal.h"

#include "log.h"
#include "os/os.h"
#include "platform/runtime.h"
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
#endif

static const char *TAG = "cli_stdio";

/* STDIO transport context */
typedef struct {
    bool initialized;
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
} stdio_ctx_t;

static stdio_ctx_t s_stdio_ctx = {0};

#ifdef _WIN32
/* Convert wide character to UTF-8 and store in context buffer */
static void wide_to_utf8(stdio_ctx_t *ctx, wchar_t wc)
{
    if (wc < 0x80) {
        ctx->utf8_buf[0] = (char)wc;
        ctx->utf8_len = 1;
    } else if (wc < 0x800) {
        ctx->utf8_buf[0] = (char)(0xC0 | (wc >> 6));
        ctx->utf8_buf[1] = (char)(0x80 | (wc & 0x3F));
        ctx->utf8_len = 2;
    } else if (wc < 0x10000) {
        ctx->utf8_buf[0] = (char)(0xE0 | (wc >> 12));
        ctx->utf8_buf[1] = (char)(0x80 | ((wc >> 6) & 0x3F));
        ctx->utf8_buf[2] = (char)(0x80 | (wc & 0x3F));
        ctx->utf8_len = 3;
    } else if (wc < 0x110000) {
        ctx->utf8_buf[0] = (char)(0xF0 | (wc >> 18));
        ctx->utf8_buf[1] = (char)(0x80 | ((wc >> 12) & 0x3F));
        ctx->utf8_buf[2] = (char)(0x80 | ((wc >> 6) & 0x3F));
        ctx->utf8_buf[3] = (char)(0x80 | (wc & 0x3F));
        ctx->utf8_len = 4;
    } else {
        ctx->utf8_len = 0;
    }
    ctx->utf8_pos = 0;
}
#endif

/* STDIO transport implementation */

static int stdio_read(void *ctx, char *buf, int len)
{
    stdio_ctx_t *stdio_ctx = (stdio_ctx_t *)ctx;
    if (!buf || len <= 0) return 0;

#ifdef _WIN32
    int n = 0;

    /* First, return any remaining UTF-8 bytes from previous wide char */
    while (n < len && stdio_ctx->utf8_pos < stdio_ctx->utf8_len) {
        buf[n++] = stdio_ctx->utf8_buf[stdio_ctx->utf8_pos++];
    }
    if (n > 0) return n;

    /* Use ReadConsoleInputW for proper Unicode handling */
    DWORD numEvents;
    if (!GetNumberOfConsoleInputEvents(stdio_ctx->hstdin, &numEvents) || numEvents == 0) {
        return 0;
    }

    while (n < len && numEvents > 0) {
        INPUT_RECORD ir;
        DWORD numRead;
        if (!ReadConsoleInputW(stdio_ctx->hstdin, &ir, 1, &numRead) || numRead == 0) {
            break;
        }
        numEvents--;

        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
            wchar_t wc = ir.Event.KeyEvent.uChar.UnicodeChar;
            if (wc != 0) {
                wide_to_utf8(stdio_ctx, wc);
                while (n < len && stdio_ctx->utf8_pos < stdio_ctx->utf8_len) {
                    buf[n++] = stdio_ctx->utf8_buf[stdio_ctx->utf8_pos++];
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

static int stdio_write(void *ctx, const char *buf, int len)
{
    (void)ctx;
    if (!buf || len <= 0) return 0;

    /* Use fwrite for binary-safe output */
    int written = fwrite(buf, 1, len, stdout);
    fflush(stdout);
    return written;
}

static bool stdio_available(void *ctx)
{
    stdio_ctx_t *stdio_ctx = (stdio_ctx_t *)ctx;

#ifdef _WIN32
    /* Check if there are remaining UTF-8 bytes in buffer */
    if (stdio_ctx->utf8_pos < stdio_ctx->utf8_len) {
        return true;
    }
    /* Check for console input events */
    DWORD numEvents;
    return GetNumberOfConsoleInputEvents(stdio_ctx->hstdin, &numEvents) && numEvents > 0;
#else
    /* Check if data available without blocking */
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
#endif
}

static void stdio_close(void *ctx)
{
    stdio_ctx_t *stdio_ctx = (stdio_ctx_t *)ctx;
    if (!stdio_ctx || !stdio_ctx->initialized) return;

#ifdef _WIN32
    /* Restore console mode */
    SetConsoleMode(stdio_ctx->hstdin, stdio_ctx->old_mode);
    /* Restore code page */
    SetConsoleOutputCP(stdio_ctx->old_mode);
#else
    /* Restore terminal settings */
    tcsetattr(STDIN_FILENO, TCSANOW, &stdio_ctx->old_termios);
#endif

    stdio_ctx->initialized = false;
    MIMI_LOGI(TAG, "STDIO transport closed");
}

static mimi_err_t stdio_transport_init(stdio_ctx_t *ctx)
{
    if (ctx->initialized) return MIMI_OK;

#ifdef _WIN32
    /* Save and modify console mode */
    ctx->hstdin = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(ctx->hstdin, &ctx->old_mode);

    /* Enable UTF-8 output */
    SetConsoleOutputCP(CP_UTF8);

    /* Set console to raw mode for character-by-character input */
    DWORD new_mode = ctx->old_mode;
    new_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    new_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(ctx->hstdin, new_mode);
#else
    /* Save current terminal settings */
    tcgetattr(STDIN_FILENO, &ctx->old_termios);

    /* Set to raw mode */
    struct termios new_termios = ctx->old_termios;
    new_termios.c_lflag &= ~(ICANON | ECHO);
    new_termios.c_cc[VMIN] = 0;
    new_termios.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

    /* Set non-blocking */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
#endif

    ctx->initialized = true;
    return MIMI_OK;
}

/* CLI task */
static void stdio_cli_task(void *arg)
{
    (void)arg;

    MIMI_LOGI(TAG, "CLI ready. Type 'help' for available commands.");

    /* Initialize transport */
    if (stdio_transport_init(&s_stdio_ctx) != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to initialize STDIO transport");
        return;
    }

    /* Initialize CLI terminal framework */
    mimi_err_t e = app_terminal_init();
    if (e != MIMI_OK) {
        MIMI_LOGE(TAG, "Failed to initialize CLI terminal framework");
        return;
    }

    /* Create transport interface */
    cli_transport_t transport = {
        .read = stdio_read,
        .write = stdio_write,
        .available = stdio_available,
        .close = stdio_close,
        .ctx = &s_stdio_ctx
    };

    /* Create terminal configuration */
    app_terminal_config_t config = {
        .name = "stdio",
        .channel = "cli",
        .chat_id = "default",
        .transport = transport
    };

    /* Create terminal */
    app_terminal_t *term = app_terminal_create(&config);
    if (!term) {
        MIMI_LOGE(TAG, "Failed to create CLI terminal");
        return;
    }

    /* Main loop */
    while (!mimi_runtime_should_exit()) {
        app_terminal_poll_all();
        mimi_sleep_ms(10);
    }

    app_terminal_destroy(term);
}

mimi_err_t stdio_cli_start(void)
{
    return mimi_task_create_detached("stdio_cli", stdio_cli_task, NULL);
}
