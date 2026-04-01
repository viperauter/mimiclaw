/**
 * Cross-platform CLI library with multi-terminal support
 * Supports: stdin, WebSocket, UART, etc.
 */

#include "editor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif

/* UTF-8 character handling utilities */

/* Check if a byte is a UTF-8 continuation byte */
static bool is_utf8_cont_byte(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

/* Get the length of a UTF-8 character starting at the given position */
static int get_utf8_char_len(const char *str, int pos)
{
    unsigned char c = (unsigned char)str[pos];
    if (c < 0x80) return 1;    /* ASCII */
    if (c < 0xE0) return 2;    /* 2-byte UTF-8 */
    if (c < 0xF0) return 3;    /* 3-byte UTF-8 */
    if (c < 0xF8) return 4;    /* 4-byte UTF-8 */
    return 1;                  /* Invalid, treat as 1 byte */
}

/* Find the start of the previous UTF-8 character before pos */
static int find_prev_utf8_char(const char *str, int pos)
{
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && is_utf8_cont_byte((unsigned char)str[pos])) {
        pos--;
    }
    return pos;
}

/* Find the end of the current UTF-8 character at pos */
static int find_next_utf8_char(const char *str, int pos, int len)
{
    if (pos >= len) return len;
    int char_len = get_utf8_char_len(str, pos);
    return pos + char_len;
}

#define CLI_MAX_LINE_LEN 1024
#define CLI_MAX_HISTORY 100
#define CLI_MAX_COMPLETIONS 32
#define CLI_MAX_TERMINALS 8

/* Terminal input buffer */
typedef struct {
    uint8_t data[256];
    size_t head;
    size_t tail;
} cli_input_buf_t;

/* Terminal instance */
struct cli_terminal {
    cli_terminal_type_t type;
    void *user_data;
    
    /* Callbacks */
    cli_char_input_cb_t input_cb;
    cli_output_cb_t output_cb;
    
    /* Line editing state */
    char line[CLI_MAX_LINE_LEN];
    int cursor;
    int len;
    
    /* History */
    char *history[CLI_MAX_HISTORY];
    int hist_count;
    int hist_index;
    
    /* Input buffer for escape sequences */
    cli_input_buf_t input_buf;
    
    /* Active flag */
    bool active;
};

/* Global CLI context */
static struct {
    cli_terminal_t terminals[CLI_MAX_TERMINALS];
    int terminal_count;
    
    /* Current active terminal for callbacks */
    cli_terminal_t *current_term;
    
    /* Callbacks */
    cli_execute_cb_t execute_cb;
    cli_complete_cb_t complete_cb;
    cli_get_prompt_cb_t prompt_cb;
} g_cli;

/* Platform-specific: Get single character without blocking */
static int cli_platform_get_char(void)
{
#ifdef _WIN32
    if (_kbhit()) {
        return _getch();
    }
    return -1;
#else
    struct termios old_tio, new_tio;
    int ch = -1;
    
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= ~(ICANON | ECHO);
    new_tio.c_cc[VMIN] = 0;
    new_tio.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    
    if (read(STDIN_FILENO, &ch, 1) != 1) {
        ch = -1;
    }
    
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    return ch;
#endif
}

/* Initialize CLI system */
void cli_init(cli_execute_cb_t exec_cb, cli_complete_cb_t complete_cb, 
              cli_get_prompt_cb_t prompt_cb)
{
    memset(&g_cli, 0, sizeof(g_cli));
    g_cli.execute_cb = exec_cb;
    g_cli.complete_cb = complete_cb;
    g_cli.prompt_cb = prompt_cb;
}

/* Create a new terminal */
cli_terminal_t* cli_terminal_create(cli_terminal_type_t type, void *user_data,
                                    cli_char_input_cb_t input_cb,
                                    cli_output_cb_t output_cb)
{
    if (g_cli.terminal_count >= CLI_MAX_TERMINALS) {
        return NULL;
    }
    
    cli_terminal_t *term = &g_cli.terminals[g_cli.terminal_count++];
    memset(term, 0, sizeof(*term));
    
    term->type = type;
    term->user_data = user_data;
    term->input_cb = input_cb;
    term->output_cb = output_cb;
    term->active = true;
    
    return term;
}

/* Destroy terminal */
void cli_terminal_destroy(cli_terminal_t *term)
{
    if (!term) return;
    
    /* Free history */
    for (int i = 0; i < term->hist_count; i++) {
        free(term->history[i]);
    }
    
    term->active = false;
}

/* Output to terminal */
static void cli_term_output(cli_terminal_t *term, const char *str)
{
    if (term->output_cb) {
        term->output_cb(term->user_data, str);
    }
}

/* Output single char to terminal */
static void cli_term_putc(cli_terminal_t *term, char c)
{
    char buf[2] = {c, '\0'};
    cli_term_output(term, buf);
}

/* Clear current line */
static void cli_clear_line(cli_terminal_t *term)
{
    /* Move cursor to beginning and clear line */
    cli_term_output(term, "\r\033[K");
}

/* Get current prompt */
static const char* cli_get_prompt_for_term(cli_terminal_t *term)
{
    if (g_cli.prompt_cb) {
        return g_cli.prompt_cb(term->user_data);
    }
    return NULL;
}

/* Redraw current line with prompt */
static void cli_redraw_line(cli_terminal_t *term, const char *prompt)
{
    cli_clear_line(term);
    if (prompt) {
        cli_term_output(term, prompt);
    }
    cli_term_output(term, term->line);
    
    /* Move cursor to correct position */
    int tail_len = term->len - term->cursor;
    for (int i = 0; i < tail_len; i++) {
        cli_term_output(term, "\033[D");
    }
}

/* Redraw current line using current prompt */
static void cli_redraw_current_line(cli_terminal_t *term)
{
    const char *prompt = cli_get_prompt_for_term(term);
    cli_redraw_line(term, prompt);
}

/* Add character at cursor position */
static void cli_insert_char(cli_terminal_t *term, char c)
{
    if (term->len >= CLI_MAX_LINE_LEN - 1) return;
    
    /* Make room */
    memmove(&term->line[term->cursor + 1], &term->line[term->cursor], 
            term->len - term->cursor);
    
    term->line[term->cursor] = c;
    term->cursor++;
    term->len++;
    term->line[term->len] = '\0';
    
    /* Redraw */
    cli_term_putc(term, c);
    cli_term_output(term, &term->line[term->cursor]);
    
    /* Move cursor back */
    for (int i = term->cursor; i < term->len; i++) {
        cli_term_output(term, "\033[D");
    }
}

/* Delete character at cursor */
static void cli_delete_char(cli_terminal_t *term)
{
    if (term->cursor >= term->len) return;
    
    /* Find the end of the current UTF-8 character */
    int next_char_start = find_next_utf8_char(term->line, term->cursor, term->len);
    
    /* Calculate the length of the character to delete */
    int char_len = next_char_start - term->cursor;
    
    /* Delete the entire UTF-8 character */
    memmove(&term->line[term->cursor], &term->line[next_char_start],
            term->len - next_char_start);
    term->len -= char_len;
    term->line[term->len] = '\0';
    
    cli_redraw_current_line(term);
}

/* Backspace */
static void cli_backspace(cli_terminal_t *term)
{
    if (term->cursor <= 0) return;
    
    /* Find the start of the previous UTF-8 character */
    int prev_char_start = find_prev_utf8_char(term->line, term->cursor);
    
    /* Calculate the length of the character to delete */
    int char_len = term->cursor - prev_char_start;
    
    /* Delete the entire UTF-8 character */
    memmove(&term->line[prev_char_start], &term->line[term->cursor],
            term->len - term->cursor);
    term->len -= char_len;
    term->line[term->len] = '\0';
    
    /* Update cursor position */
    term->cursor = prev_char_start;
    
    cli_redraw_current_line(term);
}

/* Add to history */
static void cli_add_history(cli_terminal_t *term, const char *line)
{
    if (!line || !*line) return;
    
    /* Don't add duplicate of last entry */
    if (term->hist_count > 0 && strcmp(term->history[term->hist_count - 1], line) == 0) {
        return;
    }
    
    /* Make room if needed */
    if (term->hist_count >= CLI_MAX_HISTORY) {
        free(term->history[0]);
        memmove(&term->history[0], &term->history[1], 
                (CLI_MAX_HISTORY - 1) * sizeof(char*));
        term->hist_count--;
    }
    
    term->history[term->hist_count++] = strdup(line);
}

/* Get history entry */
static const char* cli_get_history(cli_terminal_t *term, int index)
{
    if (index < 0 || index >= term->hist_count) return NULL;
    return term->history[index];
}

/* Execute Tab completion */
static void cli_do_completion(cli_terminal_t *term)
{
    if (!g_cli.complete_cb) return;
    
    /* If cursor is at a space or at end of line after a space, do nothing */
    if (term->cursor > 0 && isspace((unsigned char)term->line[term->cursor - 1])) {
        /* Cursor is after a space, don't try to complete */
        return;
    }
    
    /* Get word at cursor */
    int word_start = term->cursor;
    while (word_start > 0 && !isspace((unsigned char)term->line[word_start - 1])) {
        word_start--;
    }
    
    /* Check if this is a command (starts with "/") */
    bool is_command = false;
    char prefix[64] = "";
    
    if (word_start == 0 && term->line[0] == '/') {
        is_command = true;
        /* Extract prefix after "/" */
        int prefix_start = 1;
        int prefix_len = term->cursor - prefix_start;
        if (prefix_len > 0 && prefix_len < (int)sizeof(prefix) - 1) {
            memcpy(prefix, &term->line[prefix_start], prefix_len);
            prefix[prefix_len] = '\0';
        }
    } else {
        /* Regular word completion */
        int word_len = term->cursor - word_start;
        if (word_len > 0 && word_len < (int)sizeof(prefix) - 1) {
            memcpy(prefix, &term->line[word_start], word_len);
            prefix[word_len] = '\0';
        }
    }
    
    /* Get completions */
    char *completions[CLI_MAX_COMPLETIONS];
    int count = g_cli.complete_cb(prefix, completions, CLI_MAX_COMPLETIONS,
                                   term->user_data);
    
    if (count == 0) return;
    
    if (count == 1) {
        /* Single match: insert it */
        const char *match = completions[0];
        
        /* Calculate the new line content */
        char new_line[CLI_MAX_LINE_LEN];
        int new_len = 0;
        
        /* Keep everything before word_start */
        if (word_start > 0) {
            memcpy(new_line, term->line, word_start);
            new_len = word_start;
        }
        
        /* Add / prefix if it's a command */
        if (is_command) {
            new_line[new_len++] = '/';
        }
        
        /* Add the completion match */
        int match_len = strlen(match);
        memcpy(&new_line[new_len], match, match_len);
        new_len += match_len;
        
        /* Add space */
        new_line[new_len++] = ' ';
        
        /* Keep everything after cursor (rest of line) */
        int tail_len = term->len - term->cursor;
        if (tail_len > 0 && new_len + tail_len < CLI_MAX_LINE_LEN - 1) {
            memcpy(&new_line[new_len], &term->line[term->cursor], tail_len);
            new_len += tail_len;
        }
        
        new_line[new_len] = '\0';
        
        /* Update line */
        strcpy(term->line, new_line);
        term->len = new_len;
        term->cursor = new_len - tail_len;

        /* Redraw once */
        cli_redraw_current_line(term);
    } else {
        /* Multiple matches: first try to extend by longest common prefix */
        size_t common_len = strlen(completions[0]);
        for (int i = 1; i < count; i++) {
            size_t j = 0;
            size_t cur_len = strlen(completions[i]);
            size_t max_cmp = (common_len < cur_len) ? common_len : cur_len;
            while (j < max_cmp && completions[0][j] == completions[i][j]) {
                j++;
            }
            common_len = j;
            if (common_len == 0) {
                break;
            }
        }

        size_t prefix_len = strlen(prefix);
        if (common_len > prefix_len) {
            const char *match = completions[0];
            char new_line[CLI_MAX_LINE_LEN];
            int new_len = 0;

            /* Keep everything before word_start */
            if (word_start > 0) {
                memcpy(new_line, term->line, word_start);
                new_len = word_start;
            }

            /* Add '/' prefix if it's a command */
            if (is_command) {
                new_line[new_len++] = '/';
            }

            /* Add only the common prefix (no trailing space for multi-match) */
            if ((size_t)new_len + common_len < CLI_MAX_LINE_LEN - 1) {
                memcpy(&new_line[new_len], match, common_len);
                new_len += (int)common_len;
            } else {
                /* Not enough room, keep current line */
                goto completion_cleanup;
            }

            /* Keep everything after cursor (rest of line) */
            int tail_len = term->len - term->cursor;
            if (tail_len > 0 && new_len + tail_len < CLI_MAX_LINE_LEN - 1) {
                memcpy(&new_line[new_len], &term->line[term->cursor], tail_len);
                new_len += tail_len;
            }

            new_line[new_len] = '\0';

            /* Update line and cursor (cursor after completed common prefix) */
            strcpy(term->line, new_line);
            term->len = new_len;
            term->cursor = new_len - tail_len;
            cli_redraw_current_line(term);
            goto completion_cleanup;
        }

        /* Multiple matches without further common prefix: display them */
        cli_term_output(term, "\n");
        for (int i = 0; i < count; i++) {
            if (is_command) {
                cli_term_output(term, "/");
            }
            cli_term_output(term, completions[i]);
            cli_term_output(term, "  ");
        }
        cli_term_output(term, "\n");
        
        /* Redraw prompt and current line */
        cli_redraw_current_line(term);
    }

completion_cleanup:
    /* Free completions */
    for (int i = 0; i < count; i++) {
        free(completions[i]);
    }
}

/* Execute current line */
static void cli_execute_line(cli_terminal_t *term)
{
    if (term->len == 0) {
        cli_term_output(term, "\n");
        cli_terminal_print_prompt(term);
        return;
    }
    
    /* Add to history */
    cli_add_history(term, term->line);
    term->hist_index = term->hist_count;
    
    /* Execute */
    cli_term_output(term, "\n");
    
    if (g_cli.execute_cb) {
        g_cli.current_term = term;
        g_cli.execute_cb(term->line, term->user_data);
        g_cli.current_term = NULL;
    }
    
    /* Clear line */
    term->cursor = 0;
    term->len = 0;
    term->line[0] = '\0';
}

/* Process input character */
static void cli_process_char(cli_terminal_t *term, char c)
{
    static enum {
        STATE_NORMAL,
        STATE_ESC,
        STATE_ESC_BRACKET
    } state = STATE_NORMAL;
    
    switch (state) {
        case STATE_ESC:
            if (c == '[') {
                state = STATE_ESC_BRACKET;
                return;
            }
            state = STATE_NORMAL;
            break;
            
        case STATE_ESC_BRACKET:
            state = STATE_NORMAL;
            switch (c) {
                case 'A': /* Up */
                    if (term->hist_index > 0) {
                        term->hist_index--;
                        const char *hist = cli_get_history(term, term->hist_index);
                        if (hist) {
                            strncpy(term->line, hist, CLI_MAX_LINE_LEN - 1);
                            term->line[CLI_MAX_LINE_LEN - 1] = '\0';
                            term->len = strlen(term->line);
                            term->cursor = term->len;
                            cli_redraw_current_line(term);
                        }
                    }
                    return;
                    
                case 'B': /* Down */
                    if (term->hist_index < term->hist_count) {
                        term->hist_index++;
                        const char *hist = cli_get_history(term, term->hist_index);
                        if (hist) {
                            strncpy(term->line, hist, CLI_MAX_LINE_LEN - 1);
                            term->line[CLI_MAX_LINE_LEN - 1] = '\0';
                            term->len = strlen(term->line);
                            term->cursor = term->len;
                        } else {
                            term->line[0] = '\0';
                            term->len = 0;
                            term->cursor = 0;
                        }
                        cli_redraw_current_line(term);
                    }
                    return;
                    
                case 'C': /* Right */
                    if (term->cursor < term->len) {
                        /* Move to the end of the current UTF-8 character */
                        int old_pos = term->cursor;
                        term->cursor = find_next_utf8_char(term->line, term->cursor, term->len);
                        /* For UTF-8 characters (non-ASCII), move cursor twice to account for wide characters */
                        if ((unsigned char)term->line[old_pos] >= 0x80) {
                            cli_term_output(term, "\033[C\033[C");
                        } else {
                            cli_term_output(term, "\033[C");
                        }
                    }
                    return;
                    
                case 'D': /* Left */
                    if (term->cursor > 0) {
                        /* Move to the start of the previous UTF-8 character */
                        int prev_pos = find_prev_utf8_char(term->line, term->cursor);
                        /* For UTF-8 characters (non-ASCII), move cursor twice to account for wide characters */
                        if ((unsigned char)term->line[prev_pos] >= 0x80) {
                            cli_term_output(term, "\033[D\033[D");
                        } else {
                            cli_term_output(term, "\033[D");
                        }
                        term->cursor = prev_pos;
                    }
                    return;
                    
                case '3': /* Delete key - expect ~ */
                    /* Will be handled in next char */
                    return;
                    
                case '~':
                    /* Delete key completion */
                    cli_delete_char(term);
                    return;
            }
            break;
            
        case STATE_NORMAL:
            if (c == 27) { /* ESC */
                state = STATE_ESC;
                return;
            }
            break;
    }
    
    /* Normal character processing */
    switch (c) {
        case '\r':
        case '\n':
            cli_execute_line(term);
            break;
            
        case '\t':
            cli_do_completion(term);
            break;
            
        case 127: /* DEL */
        case 8:   /* BS */
            cli_backspace(term);
            break;
            
        case 3: /* Ctrl+C */
            cli_term_output(term, "^C\n");
            term->cursor = 0;
            term->len = 0;
            term->line[0] = '\0';
            break;
            
        case 4: /* Ctrl+D */
            if (term->len == 0) {
                cli_term_output(term, "exit\n");
                /* Signal exit */
            }
            break;
            
        case 1: /* Ctrl+A - beginning of line */
            term->cursor = 0;
            cli_redraw_current_line(term);
            break;

        case 5: /* Ctrl+E - end of line */
            term->cursor = term->len;
            cli_redraw_current_line(term);
            break;

        case 11: /* Ctrl+K - kill to end */
            term->line[term->cursor] = '\0';
            term->len = term->cursor;
            cli_redraw_current_line(term);
            break;

        case 21: /* Ctrl+U - kill line */
            term->cursor = 0;
            term->len = 0;
            term->line[0] = '\0';
            cli_redraw_current_line(term);
            break;
            
        default:
            /* Accept all printable characters including UTF-8 multi-byte */
            if (c >= 32 || (unsigned char)c >= 0x80) {
                cli_insert_char(term, c);
            }
            break;
    }
}

/* Feed character to terminal (called from input source) */
void cli_terminal_feed_char(cli_terminal_t *term, char c)
{
    if (!term || !term->active) return;
    
    /* Add to input buffer */
    size_t next_head = (term->input_buf.head + 1) % sizeof(term->input_buf.data);
    if (next_head != term->input_buf.tail) {
        term->input_buf.data[term->input_buf.head] = c;
        term->input_buf.head = next_head;
    }
}

/* Process pending input for all terminals */
void cli_poll(void)
{
    for (int i = 0; i < g_cli.terminal_count; i++) {
        cli_terminal_t *term = &g_cli.terminals[i];
        if (!term->active) continue;
        
        /* Process buffered input */
        while (term->input_buf.tail != term->input_buf.head) {
            char c = term->input_buf.data[term->input_buf.tail];
            term->input_buf.tail = (term->input_buf.tail + 1) % sizeof(term->input_buf.data);
            cli_process_char(term, c);
        }
        
        /* For stdin terminal, also check platform input */
        if (term->type == CLI_TERMINAL_STDIN) {
            int c;
            while ((c = cli_platform_get_char()) != -1) {
                cli_process_char(term, (char)c);
            }
        }
    }
}

/* Print prompt for terminal */
void cli_terminal_print_prompt(cli_terminal_t *term)
{
    if (!term || !term->active) return;
    
    const char *prompt = NULL;
    if (g_cli.prompt_cb) {
        prompt = g_cli.prompt_cb(term->user_data);
    }
    if (!prompt) {
        prompt = "> ";
    }
    
    cli_term_output(term, prompt);
}

/* Get current line content */
const char* cli_terminal_get_line(cli_terminal_t *term)
{
    if (!term) return NULL;
    return term->line;
}

/* Output from callback context (uses current terminal) */
void cli_output(const char *str)
{
    if (g_cli.current_term) {
        cli_term_output(g_cli.current_term, str);
    }
}

/* Output with newline */
void cli_output_ln(const char *str)
{
    cli_output(str);
    cli_output("\n");
}

/* Feed string to terminal */
void cli_terminal_feed_string(cli_terminal_t *term, const char *str)
{
    if (!term || !str) return;
    while (*str) {
        cli_terminal_feed_char(term, *str++);
    }
}

/* Run CLI main loop (blocking, for stdin-only mode) */
void cli_run(void)
{
    while (g_cli.terminal_count > 0) {
        cli_poll();
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
    }
}

/* Stop CLI main loop */
void cli_stop(void)
{
    /* Mark all terminals as inactive */
    for (int i = 0; i < g_cli.terminal_count; i++) {
        g_cli.terminals[i].active = false;
    }
}
