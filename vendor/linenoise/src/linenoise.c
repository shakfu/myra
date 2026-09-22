/* linenoise.c -- guerrilla line editing library against the idea that a
 * line editing lib needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 *
 *   http://github.com/antirez/linenoise
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * ------------------------------------------------------------------------
 *
 * Copyright (c) 2010-2023, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ------------------------------------------------------------------------
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward n chars
 *
 * CUB (CUrsor Backward)
 *    Sequence: ESC [ n D
 *    Effect: moves cursor backward n chars
 *
 * The following is used to get the terminal width if getting
 * the width with the TIOCGWINSZ ioctl fails
 *
 * DSR (Device Status Report)
 *    Sequence: ESC [ 6 n
 *    Effect: reports the current cusor position as ESC [ n ; m R
 *            where n is the row and m is the column
 *
 * When multi line mode is enabled, we also use an additional escape
 * sequence. However multi line editing is disabled by default.
 *
 * CUU (Cursor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars.
 *
 * CUD (Cursor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars.
 *
 * When linenoise_clear_screen() is called, two additional escape sequences
 * are used in order to clear the screen and position the cursor at home
 * position.
 *
 * CUP (Cursor position)
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED (Erase display)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#ifdef _WIN32
/* Windows-specific includes. */
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#define isatty _isatty
#define strcasecmp _stricmp
/* MSVC's _snprintf doesn't null-terminate on overflow, so wrap it. */
static int linenoise_snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = _vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (size > 0) buf[size - 1] = '\0';
    return ret;
}
#define snprintf linenoise_snprintf
/* Windows doesn't have these, stub them out. */
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
/* POSIX permission flags for open() */
#ifndef S_IRUSR
#define S_IRUSR _S_IREAD
#endif
#ifndef S_IWUSR
#define S_IWUSR _S_IWRITE
#endif
#else
/* POSIX-specific includes. */
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <fcntl.h>
#endif

#include "linenoise.h"
#include "internal/utf8.h"

#ifdef _WIN32
/* Windows I/O wrappers using console handles. */
static HANDLE win_get_output_handle(int fd) {
    (void)fd;
    return GetStdHandle(STD_OUTPUT_HANDLE);
}

static int win_write(int fd, const void *buf, size_t count) {
    HANDLE h = win_get_output_handle(fd);
    DWORD written;
    if (!WriteConsoleA(h, buf, (DWORD)count, &written, NULL)) {
        /* Fallback to WriteFile for redirected output. */
        if (!WriteFile(h, buf, (DWORD)count, &written, NULL)) {
            return -1;
        }
    }
    return (int)written;
}

static int win_read(int fd, void *buf, size_t count) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode, read_count;
    (void)fd;

    /* Check if we're reading from a console or redirected input. */
    if (GetConsoleMode(h, &mode)) {
        if (!ReadConsoleA(h, buf, (DWORD)count, &read_count, NULL)) {
            return -1;
        }
    } else {
        if (!ReadFile(h, buf, (DWORD)count, &read_count, NULL)) {
            return -1;
        }
    }
    return (int)read_count;
}

#define write(fd, buf, count) win_write(fd, buf, count)
#define read(fd, buf, count) win_read(fd, buf, count)
#endif

/* Compatibility macros mapping old function names to new utf8 module. */
#define utf8ByteLen         utf8_byte_len
#define utf8DecodeChar      utf8_decode
#define utf8DecodePrev      utf8_decode_prev
#define isVariationSelector utf8_is_variation_selector
#define isSkinToneModifier  utf8_is_skin_tone_modifier
#define isZWJ               utf8_is_zwj
#define isRegionalIndicator utf8_is_regional_indicator
#define isCombiningMark     utf8_is_combining_mark
#define isGraphemeExtend    utf8_is_grapheme_extend
#define utf8PrevCharLen     utf8_prev_grapheme_len
#define utf8NextCharLen     utf8_next_grapheme_len
#define utf8CharWidth       utf8_codepoint_width
#define utf8StrWidth        utf8_str_width
#define utf8SingleCharWidth utf8_single_char_width

/* List of terminals known to not support escape sequences. */
static char *unsupported_term[] = {"dumb","cons25","emacs",NULL};

/* ======================= Error Handling ==================================== */

/* Error code of the last failed call on the calling thread. */
#if defined(_MSC_VER)
#define LN_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define LN_THREAD_LOCAL __thread
#else
#define LN_THREAD_LOCAL
#endif
static LN_THREAD_LOCAL linenoise_error_t last_error = LINENOISE_OK;

static void set_error(linenoise_error_t err) {
    last_error = err;
}

linenoise_error_t linenoise_get_error(void) {
    return last_error;
}

const char *linenoise_error_string(linenoise_error_t err) {
    switch (err) {
        case LINENOISE_OK:              return "Success";
        case LINENOISE_ERR_ERRNO:       return "System error (check errno)";
        case LINENOISE_ERR_NOT_TTY:     return "Not a terminal";
        case LINENOISE_ERR_NOT_SUPPORTED: return "Terminal not supported";
        case LINENOISE_ERR_READ:        return "Read error";
        case LINENOISE_ERR_WRITE:       return "Write error";
        case LINENOISE_ERR_MEMORY:      return "Memory allocation failed";
        case LINENOISE_ERR_INVALID:     return "Invalid argument";
        case LINENOISE_ERR_EOF:         return "End of file";
        case LINENOISE_ERR_INTERRUPTED: return "Interrupted";
        default:                        return "Unknown error";
    }
}

/* ======================= Custom Allocator ================================== */

/* Custom allocator function pointers. NULL means use standard functions. */
static linenoise_malloc_fn custom_malloc = NULL;
static linenoise_free_fn custom_free = NULL;
static linenoise_realloc_fn custom_realloc = NULL;

void linenoise_set_allocator(linenoise_malloc_fn malloc_fn,
                             linenoise_free_fn free_fn,
                             linenoise_realloc_fn realloc_fn) {
    custom_malloc = malloc_fn;
    custom_free = free_fn;
    custom_realloc = realloc_fn;
}

/* Internal allocation wrappers. */
static void *ln_malloc(size_t size) {
    void *ptr = custom_malloc ? custom_malloc(size) : malloc(size);
    if (ptr == NULL) set_error(LINENOISE_ERR_MEMORY);
    return ptr;
}

static void ln_free(void *ptr) {
    if (custom_free) {
        custom_free(ptr);
    } else {
        free(ptr);
    }
}

static void *ln_realloc(void *ptr, size_t size) {
    void *new_ptr = custom_realloc ? custom_realloc(ptr, size) : realloc(ptr, size);
    if (new_ptr == NULL && size > 0) set_error(LINENOISE_ERR_MEMORY);
    return new_ptr;
}

static char *ln_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = ln_malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

/* Forward declarations. */
static char *linenoise_no_tty(void);
static void refresh_line_with_completion(linenoise_state_t *ls, linenoise_completions_t *lc, int flags);
static void refresh_line_with_flags(linenoise_state_t *l, int flags);
static int history_push(linenoise_context_t *ctx, const char *line);
static void history_drop_placeholder(linenoise_state_t *l);
static void fold_clear(linenoise_state_t *l);

/* ======================= Context Structure ================================= */

/* The linenoise_context structure encapsulates all state for one linenoise
 * instance. Editing code reaches it through linenoise_state_t.ctx. */
struct linenoise_context {
    /* Terminal state */
#ifndef _WIN32
    struct termios orig_termios;
#endif
    int rawmode;
    int raw_fd;         /* fd put in raw mode, restored at exit */
    linenoise_state_t *session; /* Active editing session, or NULL. */
    int placeholder;    /* history[history_len-1] is the session's own line */

    /* Configuration */
    int maskmode;
    int mlmode;
    int mousemode;

    /* History */
    int history_max_len;
    int history_len;
    char **history;

    /* Callbacks */
    linenoise_completion_cb_t *completion_callback;
    linenoise_hints_cb_t *hints_callback;
    linenoise_free_hints_cb_t *free_hints_callback;
    linenoise_highlight_cb_t *highlight_callback;
};

/* Process-wide terminal restore at exit: covers the context that most
 * recently entered raw mode. */
static int atexit_registered = 0;
static linenoise_context_t *raw_ctx = NULL;

/* UTF-8 support is now provided by src/utf8.c via internal/utf8.h.
 * The compatibility macros above map old function names to the new module. */

enum KEY_ACTION{
	KEY_NULL = 0,	    /* NULL */
	CTRL_A = 1,         /* Ctrl+a */
	CTRL_B = 2,         /* Ctrl-b */
	CTRL_C = 3,         /* Ctrl-c */
	CTRL_D = 4,         /* Ctrl-d */
	CTRL_E = 5,         /* Ctrl-e */
	CTRL_F = 6,         /* Ctrl-f */
	CTRL_H = 8,         /* Ctrl-h */
	TAB = 9,            /* Tab */
	CTRL_K = 11,        /* Ctrl+k */
	CTRL_L = 12,        /* Ctrl+l */
	ENTER = 13,         /* Enter */
	CTRL_N = 14,        /* Ctrl-n */
	CTRL_P = 16,        /* Ctrl-p */
	CTRL_T = 20,        /* Ctrl-t */
	CTRL_U = 21,        /* Ctrl+u */
	CTRL_W = 23,        /* Ctrl+w */
	CTRL_Y = 25,        /* Ctrl+y (redo) */
	CTRL_Z = 26,        /* Ctrl+z (undo) */
	ESC = 27,           /* Escape */
	BACKSPACE =  127    /* Backspace */
};

static void linenoise_at_exit(void);
#define REFRESH_CLEAN (1<<0)    // Clean the old prompt from the screen
#define REFRESH_WRITE (1<<1)    // Rewrite the prompt on the screen.
#define REFRESH_ALL (REFRESH_CLEAN|REFRESH_WRITE) // Do both.
static void refresh_line(linenoise_state_t *l);

/* Debugging macro. */
#if 0
FILE *lndebug_fp = NULL;
#define lndebug(...) \
    do { \
        if (lndebug_fp == NULL) { \
            lndebug_fp = fopen("/tmp/lndebug.txt","a"); \
            fprintf(lndebug_fp, \
            "[%d %d %d] p: %d, rows: %d, rpos: %d, max: %d, oldmax: %d\n", \
            (int)l->len,(int)l->pos,(int)l->oldpos,plen,rows,rpos, \
            (int)l->oldrows,old_rows); \
        } \
        fprintf(lndebug_fp, ", " __VA_ARGS__); \
        fflush(lndebug_fp); \
    } while (0)
#else
#define lndebug(...) ((void)0)
#endif

/* ======================= Low level terminal handling ====================== */

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
static int is_unsupported_term(void) {
#ifdef _WIN32
    /* Windows console with VT mode is always supported. */
    return 0;
#else
    char *term = getenv("TERM");
    int j;

    if (term == NULL) return 0;
    for (j = 0; unsupported_term[j]; j++)
        if (!strcasecmp(term,unsupported_term[j])) return 1;
    return 0;
#endif
}

#ifdef _WIN32
/* Windows VT mode flags (may not be defined in older SDKs). */
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

static HANDLE h_console_input = INVALID_HANDLE_VALUE;
static HANDLE h_console_output = INVALID_HANDLE_VALUE;
static DWORD orig_input_mode = 0;
static DWORD orig_output_mode = 0;

/* Raw mode for Windows using VT100 emulation (Windows 10+). The console is
 * per process, so its original modes are kept in globals. */
static int enable_raw_mode(linenoise_context_t *ctx, int fd) {
    DWORD input_mode, output_mode;

    /* Test mode: when LINENOISE_ASSUME_TTY is set, skip terminal setup. */
    if (getenv("LINENOISE_ASSUME_TTY")) {
        ctx->rawmode = 1;
        return 0;
    }

    h_console_input = GetStdHandle(STD_INPUT_HANDLE);
    h_console_output = GetStdHandle(STD_OUTPUT_HANDLE);

    if (h_console_input == INVALID_HANDLE_VALUE ||
        h_console_output == INVALID_HANDLE_VALUE) {
        return -1;
    }

    if (!GetConsoleMode(h_console_input, &orig_input_mode)) {
        return -1;
    }
    if (!GetConsoleMode(h_console_output, &orig_output_mode)) {
        return -1;
    }

    if (!atexit_registered) {
        atexit(linenoise_at_exit);
        atexit_registered = 1;
    }

    /* Configure input: disable line input, echo, and processed input.
     * Enable VT input for escape sequence support. */
    input_mode = orig_input_mode;
    input_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    input_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;

    if (!SetConsoleMode(h_console_input, input_mode)) {
        return -1;
    }

    /* Configure output: enable VT processing for escape sequences. */
    output_mode = orig_output_mode;
    output_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;

    if (!SetConsoleMode(h_console_output, output_mode)) {
        /* VT mode not available, restore input mode. */
        SetConsoleMode(h_console_input, orig_input_mode);
        return -1;
    }

    ctx->rawmode = 1;
    ctx->raw_fd = fd;
    raw_ctx = ctx;
    return 0;
}

static void disable_raw_mode(linenoise_context_t *ctx) {
    if (getenv("LINENOISE_ASSUME_TTY")) {
        ctx->rawmode = 0;
        return;
    }

    if (ctx->rawmode) {
        SetConsoleMode(h_console_input, orig_input_mode);
        SetConsoleMode(h_console_output, orig_output_mode);
        ctx->rawmode = 0;
    }
    if (raw_ctx == ctx) raw_ctx = NULL;
}

/* Read a single byte with a timeout on Windows. */
static int read_byte_with_timeout(int fd, char *c, int timeout_ms) {
    DWORD wait_result;
    DWORD read_count;
    (void)fd;

    if (timeout_ms == 0) {
        /* Non-blocking check. */
        DWORD events;
        if (!GetNumberOfConsoleInputEvents(h_console_input, &events) || events == 0) {
            return 0;
        }
    } else if (timeout_ms > 0) {
        wait_result = WaitForSingleObject(h_console_input, (DWORD)timeout_ms);
        if (wait_result == WAIT_TIMEOUT) return 0;
        if (wait_result != WAIT_OBJECT_0) return -1;
    }

    if (!ReadConsoleA(h_console_input, c, 1, &read_count, NULL)) {
        return -1;
    }
    return (read_count > 0) ? 1 : -1;
}

#else /* POSIX */

/* Raw mode: 1960 magic shit. */
static int enable_raw_mode(linenoise_context_t *ctx, int fd) {
    struct termios raw;

    /* Test mode: when LINENOISE_ASSUME_TTY is set, skip terminal setup.
     * This allows testing via pipes without a real terminal. */
    if (getenv("LINENOISE_ASSUME_TTY")) {
        ctx->rawmode = 1;
        return 0;
    }

    if (!isatty(STDIN_FILENO)) goto fatal;
    if (!atexit_registered) {
        atexit(linenoise_at_exit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd,&ctx->orig_termios) == -1) goto fatal;

    raw = ctx->orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
    ctx->rawmode = 1;
    ctx->raw_fd = fd;
    raw_ctx = ctx;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

static void disable_raw_mode(linenoise_context_t *ctx) {
    /* Test mode: nothing to restore. */
    if (getenv("LINENOISE_ASSUME_TTY")) {
        ctx->rawmode = 0;
        return;
    }
    /* Don't even check the return value as it's too late. */
    if (ctx->rawmode && tcsetattr(ctx->raw_fd,TCSAFLUSH,&ctx->orig_termios) != -1)
        ctx->rawmode = 0;
    if (raw_ctx == ctx) raw_ctx = NULL;
}

/* Read a single byte with a timeout. Returns 1 on success, 0 on timeout,
 * -1 on error. timeout_ms is the timeout in milliseconds. */
static int read_byte_with_timeout(int fd, char *c, int timeout_ms) {
    fd_set readfds;
    struct timeval tv;

    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(fd + 1, &readfds, NULL, NULL, &tv);
    if (ret <= 0) return ret;  /* 0 = timeout, -1 = error */
    return read(fd, c, 1);
}

#endif /* _WIN32 */

/* Enable mouse tracking mode. Sends escape sequences to enable X10 mouse
 * reporting with SGR extended coordinates. */
static void enable_mouse_tracking(int fd) {
    /* \x1b[?1000h - Enable X10 mouse reporting (button press)
     * \x1b[?1002h - Enable cell motion mouse tracking (drag events)
     * \x1b[?1006h - Enable SGR extended mouse mode (better coordinate encoding) */
    const char *seq = "\x1b[?1000h\x1b[?1002h\x1b[?1006h";
    if (write(fd, seq, strlen(seq)) == -1) { /* Ignore errors */ }
}

/* Disable mouse tracking mode. */
static void disable_mouse_tracking(int fd) {
    const char *seq = "\x1b[?1006l\x1b[?1002l\x1b[?1000l";
    if (write(fd, seq, strlen(seq)) == -1) { /* Ignore errors */ }
}

/* Ask the terminal to wrap pasted input between ESC[200~ and ESC[201~, so
 * that a paste can be told apart from typed input. */
static void enable_bracketed_paste(int fd) {
    if (write(fd, "\x1b[?2004h", 8) == -1) { /* Ignore errors */ }
}

/* Leave bracketed paste mode. */
static void disable_bracketed_paste(int fd) {
    if (write(fd, "\x1b[?2004l", 8) == -1) { /* Ignore errors */ }
}

#ifdef _WIN32

/* Get terminal columns on Windows. */
static int get_columns(int ifd, int ofd) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    (void)ifd; (void)ofd;

    /* Test mode: use LINENOISE_COLS env var for fixed width. */
    char *cols_env = getenv("LINENOISE_COLS");
    if (cols_env) return atoi(cols_env);

    if (GetConsoleScreenBufferInfo(h_console_output, &csbi)) {
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
    return 80;
}

/* Internal: Clear the screen on Windows. */
static void clear_screen(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    COORD coord = {0, 0};
    DWORD written, console_size;

    if (!GetConsoleScreenBufferInfo(h_console_output, &csbi)) {
        /* Fallback to escape sequence. */
        DWORD dummy;
        WriteConsoleA(h_console_output, "\x1b[H\x1b[2J", 7, &dummy, NULL);
        return;
    }

    console_size = csbi.dwSize.X * csbi.dwSize.Y;
    FillConsoleOutputCharacterA(h_console_output, ' ', console_size, coord, &written);
    FillConsoleOutputAttribute(h_console_output, csbi.wAttributes, console_size, coord, &written);
    SetConsoleCursorPosition(h_console_output, coord);
}

#else /* POSIX */

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor. */
static int get_cursor_position(int ifd, int ofd) {
    char buf[32];
    int cols, rows;
    unsigned int i = 0;

    /* Report cursor location */
    if (write(ofd, "\x1b[6n", 4) != 4) return -1;

    /* Read the response: ESC [ rows ; cols R */
    while (i < sizeof(buf)-1) {
        if (read(ifd,buf+i,1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    /* Parse it. */
    if (buf[0] != ESC || buf[1] != '[') return -1;
    if (sscanf(buf+2,"%d;%d",&rows,&cols) != 2) return -1;
    return cols;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int get_columns(int ifd, int ofd) {
    struct winsize ws;

    /* Test mode: use LINENOISE_COLS env var for fixed width. */
    char *cols_env = getenv("LINENOISE_COLS");
    if (cols_env) return atoi(cols_env);

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /* ioctl() failed. Try to query the terminal itself. */
        int start, cols;

        /* Get the initial position so we can restore it later. */
        start = get_cursor_position(ifd,ofd);
        if (start == -1) goto failed;

        /* Go to right margin and get position. */
        if (write(ofd,"\x1b[999C",6) != 6) goto failed;
        cols = get_cursor_position(ifd,ofd);
        if (cols == -1) goto failed;

        /* Restore position. */
        if (cols > start) {
            char seq[LINENOISE_SEQ_SIZE];
            snprintf(seq,sizeof(seq),"\x1b[%dD",cols-start);
            if (write(ofd,seq,strlen(seq)) == -1) {
                /* Can't recover... */
            }
        }
        return cols;
    } else {
        return ws.ws_col;
    }

failed:
    return 80;
}

/* Internal: Clear the screen. Used to handle ctrl+l */
static void clear_screen(void) {
    if (write(STDOUT_FILENO,"\x1b[H\x1b[2J",7) <= 0) {
        /* nothing to do, just to avoid warning. */
    }
}

#endif /* _WIN32 */

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void linenoise_beep(void) {
    fprintf(stderr, "\x7");
    fflush(stderr);
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by linenoise_add_completion(). */
static void free_completions(linenoise_completions_t *lc) {
    size_t i;
    for (i = 0; i < lc->len; i++)
        ln_free(lc->cvec[i]);
    if (lc->cvec != NULL)
        ln_free(lc->cvec);
}

/* Called by complete_line() and linenoise_show() to render the current
 * edited line with the proposed completion. If the current completion table
 * is already available, it is passed as second argument, otherwise the
 * function will use the callback to obtain it.
 *
 * Flags are the same as refresh_line*(), that is REFRESH_* macros. */
static void refresh_line_with_completion(linenoise_state_t *ls, linenoise_completions_t *lc, int flags) {
    /* Obtain the table of completions if the caller didn't provide one. */
    linenoise_completions_t ctable = { 0, NULL };
    if (lc == NULL) {
        ls->ctx->completion_callback(ls->buf,&ctable);
        lc = &ctable;
    }

    /* Show the edited line with completion if possible, or just refresh. */
    if (ls->completion_idx < lc->len) {
        linenoise_state_t saved = *ls;
        ls->len = ls->pos = strlen(lc->cvec[ls->completion_idx]);
        ls->buf = lc->cvec[ls->completion_idx];
        ls->fold_count = 0;
        refresh_line_with_flags(ls,flags);
        ls->len = saved.len;
        ls->pos = saved.pos;
        ls->buf = saved.buf;
        ls->fold_count = saved.fold_count;
    } else {
        refresh_line_with_flags(ls,flags);
    }

    /* Free the completions table if we allocated it locally. */
    if (lc == &ctable) free_completions(&ctable);
}

/* This is an helper function for linenoise_edit_*() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 *
 * The state of the editing is encapsulated into the pointed linenoise_state_t
 * structure as described in the structure definition.
 *
 * If the function returns non-zero, the caller should handle the
 * returned value as a byte read from the standard input, and process
 * it as usually: this basically means that the function may return a byte
 * read from the termianl but not processed. Otherwise, if zero is returned,
 * the input was consumed by the complete_line() function to navigate the
 * possible completions, and the caller should read for the next characters
 * from stdin. */
static int complete_line(linenoise_state_t *ls, int keypressed) {
    linenoise_completions_t lc = { 0, NULL };
    int nwritten;
    char c = keypressed;

    ls->ctx->completion_callback(ls->buf,&lc);
    if (lc.len == 0) {
        linenoise_beep();
        ls->in_completion = 0;
        c = 0; /* The key was consumed: don't insert a literal TAB. */
    } else {
        switch(c) {
            case 9: /* tab */
                if (ls->in_completion == 0) {
                    ls->in_completion = 1;
                    ls->completion_idx = 0;
                } else {
                    ls->completion_idx = (ls->completion_idx+1) % (lc.len+1);
                    if (ls->completion_idx == lc.len) linenoise_beep();
                }
                c = 0;
                break;
            case 27: /* escape */
                /* Re-show original buffer */
                if (ls->completion_idx < lc.len) refresh_line(ls);
                ls->in_completion = 0;
                c = 0;
                break;
            default:
                /* Update buffer and return */
                if (ls->completion_idx < lc.len) {
                    nwritten = snprintf(ls->buf,ls->buflen,"%s",
                        lc.cvec[ls->completion_idx]);
                    ls->len = ls->pos = nwritten;
                    fold_clear(ls);
                }
                ls->in_completion = 0;
                break;
        }

        /* Show completion or original buffer */
        if (ls->in_completion && ls->completion_idx < lc.len) {
            refresh_line_with_completion(ls,&lc,REFRESH_ALL);
        } else {
            refresh_line(ls);
        }
    }

    free_completions(&lc);
    return c; /* Return last read character */
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void linenoise_add_completion(linenoise_completions_t *lc, const char *str) {
    size_t len = strlen(str);
    char *copy, **cvec;

    copy = ln_malloc(len+1);
    if (copy == NULL) return;
    memcpy(copy,str,len+1);
    cvec = ln_realloc(lc->cvec,sizeof(char*)*(lc->len+1));
    if (cvec == NULL) {
        ln_free(copy);
        return;
    }
    lc->cvec = cvec;
    lc->cvec[lc->len++] = copy;
}

/* =========================== Line editing ================================= */

/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf {
    char *b;
    int len;
};

static void ab_init(struct abuf *ab) {
    ab->b = NULL;
    ab->len = 0;
}

static void ab_append(struct abuf *ab, const char *s, int len) {
    char *new = ln_realloc(ab->b,ab->len+len);

    if (new == NULL) return;
    memcpy(new+ab->len,s,len);
    ab->b = new;
    ab->len += len;
}

static void ab_free(struct abuf *ab) {
    ln_free(ab->b);
}

/* ===================== Bracketed paste and folding ======================== */

/* Minimum byte length of a single line paste before it gets folded. */
#define PASTE_FOLD_THRESHOLD 200
/* Context characters kept visible around a fold rebuilt from history. */
#define PASTE_FOLD_CONTEXT 8
/* Upper bound for a single paste, and for the growth of a dynamic buffer. */
#define PASTE_MAX_BYTES (1024*1024)

/* A fold is a display-only replacement for a range in l->buf. The edited
 * buffer always keeps the real bytes; the refresh code asks render_buffer()
 * for a temporary printable version plus the cursor position inside it. */
struct fold {
    size_t start;
    size_t end;
    char display[64];
    size_t displaylen;
};

struct folds {
    int count;
    struct fold fold[LINENOISE_MAX_FOLDS];
};

/* Return the number of logical lines in the range. */
static size_t fold_count_lines(const char *buf, size_t len) {
    size_t lines = 1, j;
    for (j = 0; j < len; j++) {
        if (buf[j] == '\n') lines++;
    }
    return lines;
}

/* Return true if the text should be folded: if it contains newlines or is at
 * least PASTE_FOLD_THRESHOLD bytes long. */
static int should_fold_text(const char *buf, size_t len) {
    return memchr(buf, '\n', len) != NULL || len >= PASTE_FOLD_THRESHOLD;
}

/* Fill f->display with the text shown instead of the folded range. */
static void fold_set_rendered_text(struct fold *f, const char *buf) {
    size_t hidden = f->end - f->start;
    size_t lines = fold_count_lines(buf + f->start, hidden);
    int n;

    if (lines > 1)
        n = snprintf(f->display,sizeof(f->display),
                     "[... %lu pasted lines ...]",(unsigned long)lines);
    else
        n = snprintf(f->display,sizeof(f->display),
                     "[... %lu pasted chars ...]",(unsigned long)hidden);
    if (n < 0) n = 0;
    f->displaylen = (size_t)n;
}

/* Populate f with one fold reconstructed from a history entry. History stores
 * the real text, but not the original paste boundaries, so we reconstruct an
 * approximation of the text we want to hide on the fly: if it is long or
 * contains newlines. */
static int build_history_fold(linenoise_state_t *l, struct fold *f) {
    f->start = f->end = f->displaylen = 0;
    if (l->len == 0 || l->ctx->maskmode) return 0;
    if (!should_fold_text(l->buf,l->len)) return 0;

    f->start = 0;
    f->end = l->len;
    if (l->len > PASTE_FOLD_CONTEXT*2) {
        size_t pos = 0, chars = 0;
        int nl = 0;

        /* We leave (if possible) a few chars on the start before the fold,
         * to give context. */
        while (pos < l->len && chars < PASTE_FOLD_CONTEXT) {
            size_t step = utf8NextCharLen(l->buf,pos,l->len);
            if (step == 0 || pos + step > l->len) break;
            if (l->buf[pos] == '\n') nl = 1;
            pos += step;
            chars++;
        }
        f->start = nl ? 0 : pos;

        /* And also on the end side. */
        pos = l->len;
        chars = 0;
        nl = 0;
        while (pos > 0 && chars < PASTE_FOLD_CONTEXT) {
            size_t step = utf8PrevCharLen(l->buf,pos);
            if (step == 0 || step > pos) break;
            pos -= step;
            if (l->buf[pos] == '\n') nl = 1;
            chars++;
        }
        f->end = nl ? l->len : pos;
        if (f->start >= f->end) {
            f->start = 0;
            f->end = l->len;
        }
    }
    fold_set_rendered_text(f,l->buf);
    return 1;
}

/* Populate fs with the folds to render for the current buffer. As a side
 * effect, the rendered text of each fold is updated. Return 1 if folding
 * should be used, or 0 if the buffer should be rendered as-is. */
static int get_render_folds(linenoise_state_t *l, struct folds *fs) {
    int j;

    fs->count = 0;
    if (l->len == 0 || l->ctx->maskmode) return 0;

    for (j = 0; j < l->fold_count; j++) {
        struct fold *f;
        size_t start = l->fold_start[j];
        size_t end = l->fold_end[j];

        if (start >= end || end > l->len) continue;
        f = fs->fold + fs->count++;
        f->start = start;
        f->end = end;
        fold_set_rendered_text(f,l->buf);
    }
    return fs->count != 0;
}

/* Return the freshly allocated string content that is actually displayed in
 * the user prompt. It can be the actual edited line, or a special version
 * where pasted or multiline history ranges are replaced by their folded
 * "[...]" style versions. outpos is l->pos translated into this rendered
 * buffer. Returns 0 on success, -1 on allocation failure. */
static int render_buffer(linenoise_state_t *l, char **out, size_t *outlen, size_t *outpos) {
    struct folds fs;
    size_t len, pos, src, dst;
    char *r;
    int j, pos_set = 0;

    if (!get_render_folds(l,&fs)) {
        /* Keep the refresh code simple: it always owns a temporary render
         * buffer, even when the render is identical to the real edit buffer. */
        r = ln_malloc(l->len+1);
        if (r == NULL) return -1;
        memcpy(r,l->buf,l->len);
        r[l->len] = '\0';
        *out = r;
        *outlen = l->len;
        *outpos = l->pos;
        return 0;
    }

    /* Gaps are copied as-is, folded ranges are replaced by their markers.
     * The bytes inside each [start,end) range stay in l->buf but are not
     * emitted to the terminal. */
    len = l->len;
    for (j = 0; j < fs.count; j++) {
        struct fold *f = fs.fold+j;
        len -= f->end - f->start;
        len += f->displaylen;
    }
    r = ln_malloc(len+1);
    if (r == NULL) return -1;

    src = dst = 0;
    pos = 0;
    for (j = 0; j < fs.count; j++) {
        struct fold *f = fs.fold+j;
        size_t gap = f->start - src;

        if (!pos_set && l->pos <= f->start) {
            pos = dst + (l->pos - src);
            pos_set = 1;
        }
        memcpy(r+dst,l->buf+src,gap);
        dst += gap;

        if (!pos_set && l->pos < f->end) {
            pos = dst + f->displaylen;
            pos_set = 1;
        }
        memcpy(r+dst,f->display,f->displaylen);
        dst += f->displaylen;
        if (!pos_set && l->pos == f->end) {
            pos = dst;
            pos_set = 1;
        }
        src = f->end;
    }
    if (!pos_set) pos = dst + (l->pos - src);
    memcpy(r+dst,l->buf+src,l->len-src);
    r[len] = '\0';

    *out = r;
    *outlen = len;
    *outpos = pos;
    return 0;
}

/* Return the number of bytes to move right from pos. If pos is at the start of
 * a folded range, the whole hidden range is skipped by one cursor movement. */
static size_t edit_next_len(linenoise_state_t *l, size_t pos) {
    struct folds fs;
    int j;

    if (get_render_folds(l,&fs)) {
        for (j = 0; j < fs.count; j++) {
            if (pos == fs.fold[j].start)
                return fs.fold[j].end - fs.fold[j].start;
        }
    }
    return utf8NextCharLen(l->buf,pos,l->len);
}

/* Return the number of bytes to move left from pos. If pos is at the end of a
 * folded range, the whole hidden range is skipped by one cursor movement. */
static size_t edit_prev_len(linenoise_state_t *l, size_t pos) {
    struct folds fs;
    int j;

    if (get_render_folds(l,&fs)) {
        for (j = 0; j < fs.count; j++) {
            if (pos == fs.fold[j].end)
                return fs.fold[j].end - fs.fold[j].start;
        }
    }
    return utf8PrevCharLen(l->buf,pos);
}

/* Add a fold range, keeping the array sorted by start offset. */
static void fold_add(linenoise_state_t *l, size_t start, size_t end) {
    int j;

    if (start >= end || l->fold_count == LINENOISE_MAX_FOLDS) return;
    j = l->fold_count;
    while (j > 0 && start < l->fold_start[j-1]) {
        l->fold_start[j] = l->fold_start[j-1];
        l->fold_end[j] = l->fold_end[j-1];
        j--;
    }
    l->fold_start[j] = start;
    l->fold_end[j] = end;
    l->fold_count++;
}

/* Clear all remembered fold ranges. */
static void fold_clear(linenoise_state_t *l) {
    l->fold_count = 0;
}

/* Remove one remembered fold range. */
static void fold_remove(linenoise_state_t *l, int j) {
    memmove(l->fold_start+j,l->fold_start+j+1,
            sizeof(size_t)*(l->fold_count-j-1));
    memmove(l->fold_end+j,l->fold_end+j+1,
            sizeof(size_t)*(l->fold_count-j-1));
    l->fold_count--;
}

/* Return true if [pos,pos+len) overlaps any folded range. */
static int range_overlaps_fold(linenoise_state_t *l, size_t pos, size_t len) {
    size_t end = pos + len;
    int j;

    for (j = 0; j < l->fold_count; j++) {
        if (end > l->fold_start[j] && pos < l->fold_end[j])
            return 1;
    }
    return 0;
}

/* Adjust fold ranges after an insertion. If insertion somehow lands inside a
 * fold, remove that fold because it no longer maps to an unchanged range. */
static void adjust_folds_after_insert(linenoise_state_t *l, size_t pos, size_t len) {
    int j = 0;

    while (j < l->fold_count) {
        if (pos <= l->fold_start[j]) {
            l->fold_start[j] += len;
            l->fold_end[j] += len;
            j++;
        } else if (pos < l->fold_end[j]) {
            fold_remove(l,j);
        } else {
            j++;
        }
    }
}

/* Adjust fold ranges after a deletion. If the deletion overlaps a fold, remove
 * that fold because it no longer maps to an unchanged range. */
static void adjust_folds_after_delete(linenoise_state_t *l, size_t pos, size_t len) {
    size_t end = pos + len;
    int j = 0;

    while (j < l->fold_count) {
        if (end <= l->fold_start[j]) {
            l->fold_start[j] -= len;
            l->fold_end[j] -= len;
            j++;
        } else if (pos >= l->fold_end[j]) {
            j++;
        } else {
            fold_remove(l,j);
        }
    }
}

/* Helper to append text with syntax highlighting.
 * If 'highlight' is set, calls it to get colors for each byte,
 * then outputs the text with appropriate ANSI color codes.
 *
 * Color values use 256-color palette:
 *   0       = default (no color change)
 *   1-15    = basic ANSI colors (legacy 16-color support)
 *   16-255  = extended 256-color palette
 *
 * For colors 1-15, the old behavior is preserved for compatibility:
 *   1-7     = standard colors (red, green, yellow, blue, magenta, cyan, white)
 *   8-15    = bold/bright variants
 *
 * For colors 16-255, we use the 256-color escape sequence:
 *   \x1b[38;5;Xm where X is the color number
 */
static void ab_append_highlighted(struct abuf *ab, linenoise_highlight_cb_t *highlight,
                                  const char *buf, size_t len) {
    char seq[32];
    char *colors = NULL;
    size_t i;
    int cur_color = 0;

    if (!highlight || len == 0) {
        /* Output text, clearing each line after newlines for multiline support. */
        size_t start = 0;
        for (i = 0; i < len; i++) {
            if (buf[i] == '\n') {
                ab_append(ab, buf + start, (int)(i - start + 1));
                ab_append(ab, "\r\x1b[0K", 5);  /* Go to column 0 and clear line */
                start = i + 1;
            }
        }
        if (start < len) {
            ab_append(ab, buf + start, (int)(len - start));
        }
        return;
    }

    /* Allocate and zero the colors array. */
    colors = ln_malloc(len);
    if (!colors) {
        ab_append(ab, buf, (int)len);
        return;
    }
    memset(colors, 0, len);

    /* Call the highlight callback. */
    highlight(buf, colors, len);

    /* Output text with color changes. */
    for (i = 0; i < len; i++) {
        int new_color = (unsigned char)colors[i];
        if (new_color != cur_color) {
            /* Output color change sequence. */
            if (new_color == 0) {
                /* Reset to default. */
                ab_append(ab, "\x1b[0m", 4);
            } else if (new_color < 16) {
                /* Legacy 16-color mode for backward compatibility. */
                int bold = (new_color & 8) ? 1 : 0;
                int fg = 30 + (new_color & 7);  /* 31-37 */
                if (bold) {
                    snprintf(seq, sizeof(seq), "\x1b[1;%dm", fg);
                } else {
                    snprintf(seq, sizeof(seq), "\x1b[%dm", fg);
                }
                ab_append(ab, seq, (int)strlen(seq));
            } else {
                /* 256-color mode. */
                snprintf(seq, sizeof(seq), "\x1b[38;5;%dm", new_color);
                ab_append(ab, seq, (int)strlen(seq));
            }
            cur_color = new_color;
        }
        ab_append(ab, buf + i, 1);
        /* After outputting a newline, go to column 0 and clear the line. */
        if (buf[i] == '\n') {
            ab_append(ab, "\r\x1b[0K", 5);
        }
    }

    /* Reset color at the end if we changed it. */
    if (cur_color != 0) {
        ab_append(ab, "\x1b[0m", 4);
    }

    ln_free(colors);
}

/* Helper of refresh_single_line() and refresh_multi_line() to show hints
 * to the right of the prompt. Now uses display widths for proper UTF-8. */
void refresh_show_hints(struct abuf *ab, linenoise_state_t *l, int pwidth, size_t bufwidth) {
    char seq[LINENOISE_SEQ_SIZE];
    if (l->ctx->hints_callback && pwidth + bufwidth < l->cols) {
        int color = -1, bold = 0;
        char *hint = l->ctx->hints_callback(l->buf,&color,&bold);
        if (hint) {
            size_t hintlen = strlen(hint);
            size_t hintwidth = utf8StrWidth(hint, hintlen);
            size_t hintmaxwidth = l->cols - (pwidth + bufwidth);
            /* Truncate hint to fit, respecting UTF-8 boundaries. */
            if (hintwidth > hintmaxwidth) {
                size_t i = 0, w = 0;
                while (i < hintlen) {
                    size_t clen = utf8NextCharLen(hint, i, hintlen);
                    int cwidth = utf8SingleCharWidth(hint + i, clen);
                    if (w + cwidth > hintmaxwidth) break;
                    w += cwidth;
                    i += clen;
                }
                hintlen = i;
            }
            if (bold == 1 && color == -1) color = 37;
            if (color != -1 || bold != 0)
                snprintf(seq,sizeof(seq),"\033[%d;%d;49m",bold,color);
            else
                seq[0] = '\0';
            ab_append(ab,seq,strlen(seq));
            ab_append(ab,hint,hintlen);
            if (color != -1 || bold != 0)
                ab_append(ab,"\033[0m",4);
            /* Call the function to free the hint returned. */
            if (l->ctx->free_hints_callback) l->ctx->free_hints_callback(hint);
        }
    }
}

/* Calculate total display rows for text with embedded newlines.
 * Takes into account both terminal width wrapping and explicit newlines.
 * initial_col is the starting column (e.g., prompt width for first line). */
static int calc_rows_with_newlines(const char *buf, size_t len, size_t cols, size_t initial_col) {
    int rows = 1;
    size_t col = initial_col;
    size_t i = 0;

    while (i < len) {
        if (buf[i] == '\n') {
            rows++;
            col = 0;
            i++;
        } else {
            size_t clen = utf8NextCharLen(buf, i, len);
            int cwidth = utf8SingleCharWidth(buf + i, clen);
            col += cwidth;
            if (col >= cols) {
                rows++;
                col = col - cols;  /* Wrap: remaining width goes to next row */
            }
            i += clen;
        }
    }
    return rows;
}

/* Calculate cursor row and column for a position in text with embedded newlines.
 * Returns the row (1-based) in *out_row and column (0-based) in *out_col.
 * initial_col is the starting column (e.g., prompt width for first line).
 * buflen is the total buffer length (needed for UTF-8 boundary detection). */
static void calc_cursor_pos_with_newlines(const char *buf, size_t pos, size_t buflen,
                                          size_t cols, size_t initial_col,
                                          int *out_row, int *out_col) {
    int row = 1;
    size_t col = initial_col;
    size_t i = 0;

    while (i < pos) {
        if (buf[i] == '\n') {
            row++;
            col = 0;
            i++;
        } else {
            size_t clen = utf8NextCharLen(buf, i, buflen);
            int cwidth = utf8SingleCharWidth(buf + i, clen);
            col += cwidth;
            if (col >= cols) {
                row++;
                col = col - cols;
            }
            i += clen;
        }
    }
    *out_row = row;
    *out_col = (int)col;
}

/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal.
 *
 * Flags is REFRESH_* macros. The function can just remove the old
 * prompt, just write it, or both.
 *
 * This function is UTF-8 aware and uses display widths (not byte counts)
 * for cursor positioning and horizontal scrolling. */
static void refresh_single_line(linenoise_state_t *l, int flags) {
    char seq[LINENOISE_SEQ_SIZE];
    size_t pwidth = utf8StrWidth(l->prompt, l->plen); /* Prompt display width */
    int fd = l->ofd;
    char *render = NULL;    /* Display version of the buffer (folds applied). */
    char *buf;
    size_t len;             /* Byte length of buffer to display */
    size_t pos;             /* Byte position of cursor in display buffer */
    size_t poscol;          /* Display column of cursor */
    size_t lencol;          /* Display width of buffer */
    size_t fullwidth;       /* Display width before horizontal trimming. */
    struct abuf ab;

    if (render_buffer(l,&render,&len,&pos) == -1) return;
    buf = render;

    /* Calculate the display width up to cursor and total display width. */
    poscol = utf8StrWidth(buf, pos);
    lencol = utf8StrWidth(buf, len);
    fullwidth = lencol;

    /* Scroll the buffer horizontally if cursor is past the right edge.
     * We need to trim full UTF-8 characters from the left until the
     * cursor position fits within the terminal width. */
    while (pwidth + poscol >= l->cols) {
        size_t clen = utf8NextCharLen(buf, 0, len);
        int cwidth = utf8SingleCharWidth(buf, clen);
        buf += clen;
        len -= clen;
        pos -= clen;
        poscol -= cwidth;
        lencol -= cwidth;
    }

    /* Trim from the right if the line still doesn't fit. */
    while (pwidth + lencol > l->cols) {
        size_t clen = utf8PrevCharLen(buf, len);
        int cwidth = utf8SingleCharWidth(buf + len - clen, clen);
        len -= clen;
        lencol -= cwidth;
    }

    ab_init(&ab);
    /* Cursor to left edge */
    snprintf(seq,sizeof(seq),"\r");
    ab_append(&ab,seq,strlen(seq));

    if (flags & REFRESH_WRITE) {
        /* Write the prompt and the current buffer content */
        ab_append(&ab,l->prompt,l->plen);
        if (l->ctx->maskmode == 1) {
            /* In mask mode, we output one '*' per UTF-8 character, not byte */
            size_t i = 0;
            while (i < len) {
                ab_append(&ab,"*",1);
                i += utf8NextCharLen(buf, i, len);
            }
        } else {
            ab_append_highlighted(&ab,l->ctx->highlight_callback,buf,len);
        }
        /* Show hints if any. */
        refresh_show_hints(&ab,l,pwidth,fullwidth);
    }

    /* Erase to right */
    snprintf(seq,sizeof(seq),"\x1b[0K");
    ab_append(&ab,seq,strlen(seq));

    if (flags & REFRESH_WRITE) {
        /* Move cursor to original position (using display column, not byte). */
        snprintf(seq,sizeof(seq),"\r\x1b[%dC", (int)(poscol+pwidth));
        ab_append(&ab,seq,strlen(seq));
    }

    if (write(fd,ab.b,ab.len) == -1) {} /* Can't recover from write error. */
    ab_free(&ab);
    ln_free(render);
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal.
 *
 * Flags is REFRESH_* macros. The function can just remove the old
 * prompt, just write it, or both.
 *
 * This function is UTF-8 aware and uses display widths for positioning. */
static void refresh_multi_line(linenoise_state_t *l, int flags) {
    char seq[LINENOISE_SEQ_SIZE];
    size_t pwidth = utf8StrWidth(l->prompt, l->plen);  /* Prompt display width */
    char *render = NULL;     /* Display version of the buffer (folds applied). */
    size_t render_len, render_pos;
    size_t bufwidth;
    int rows;      /* rows used by current rendered buffer */
    int rpos = l->oldrpos;   /* cursor relative row from previous refresh. */
    int rpos2; /* rpos after refresh. */
    int col; /* column position, zero-based. */
    int old_rows = l->oldrows;
    int fd = l->ofd, j;
    struct abuf ab;

    if (render_buffer(l,&render,&render_len,&render_pos) == -1) return;
    bufwidth = utf8StrWidth(render, render_len);

    /* Calculate rows accounting for embedded newlines in the buffer. */
    rows = calc_rows_with_newlines(render, render_len, l->cols, pwidth);

    l->oldrows = rows;

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */
    ab_init(&ab);

    if (flags & REFRESH_CLEAN) {
        if (old_rows-rpos > 0) {
            lndebug("go down %d", old_rows-rpos);
            snprintf(seq,sizeof(seq),"\x1b[%dB", old_rows-rpos);
            ab_append(&ab,seq,strlen(seq));
        }

        /* Now for every row clear it, go up. */
        for (j = 0; j < old_rows-1; j++) {
            lndebug("clear+up");
            snprintf(seq,sizeof(seq),"\r\x1b[0K\x1b[1A");
            ab_append(&ab,seq,strlen(seq));
        }
    }

    if (flags & REFRESH_ALL) {
        /* Clean the top line. */
        lndebug("clear");
        snprintf(seq,sizeof(seq),"\r\x1b[0K");
        ab_append(&ab,seq,strlen(seq));

        /* If current content has more rows than old content, clear extra rows.
         * This handles the case where we added a newline to the buffer.
         * old_rows must be at least 1 for this to make sense. */
        if (rows > old_rows && old_rows >= 1) {
            int extra_rows = rows - old_rows;
            for (j = 0; j < extra_rows; j++) {
                snprintf(seq,sizeof(seq),"\n\x1b[0K");
                ab_append(&ab,seq,strlen(seq));
            }
            /* Go back up to the top line. */
            snprintf(seq,sizeof(seq),"\x1b[%dA\r", extra_rows);
            ab_append(&ab,seq,strlen(seq));
        }
    }

    if (flags & REFRESH_WRITE) {
        /* Write the prompt and the current buffer content */
        ab_append(&ab,l->prompt,l->plen);
        if (l->ctx->maskmode == 1) {
            /* In mask mode, output one '*' per UTF-8 character, not byte */
            size_t i = 0;
            while (i < render_len) {
                ab_append(&ab,"*",1);
                i += utf8NextCharLen(render, i, render_len);
            }
        } else {
            ab_append_highlighted(&ab,l->ctx->highlight_callback,render,render_len);
        }

        /* Show hints if any. */
        refresh_show_hints(&ab,l,pwidth,bufwidth);

        /* Calculate cursor position accounting for embedded newlines. */
        calc_cursor_pos_with_newlines(render, render_pos, render_len, l->cols, pwidth, &rpos2, &col);
        lndebug("rpos2 %d col %d", rpos2, col);

        /* If we are at the very end of the screen with our prompt, we need to
         * emit a newline and move the prompt to the first column. */
        if (render_pos &&
            render_pos == render_len &&
            col == 0)
        {
            lndebug("<newline>");
            ab_append(&ab,"\n",1);
            snprintf(seq,sizeof(seq),"\r");
            ab_append(&ab,seq,strlen(seq));
            rows++;
            if (rows > (int)l->oldrows) l->oldrows = rows;
        }

        /* Go up till we reach the expected position. */
        if (rows-rpos2 > 0) {
            lndebug("go-up %d", rows-rpos2);
            snprintf(seq,sizeof(seq),"\x1b[%dA", rows-rpos2);
            ab_append(&ab,seq,strlen(seq));
        }

        /* Set column. */
        lndebug("set col %d", 1+col);
        if (col)
            snprintf(seq,sizeof(seq),"\r\x1b[%dC", col);
        else
            snprintf(seq,sizeof(seq),"\r");
        ab_append(&ab,seq,strlen(seq));
    }

    lndebug("\n");
    l->oldpos = l->pos;
    if (flags & REFRESH_WRITE) l->oldrpos = rpos2;

    if (write(fd,ab.b,ab.len) == -1) {} /* Can't recover from write error. */
    ab_free(&ab);
    ln_free(render);
}

/* Calls the two low level functions refresh_single_line() or
 * refresh_multi_line() according to the selected mode. */
static void refresh_line_with_flags(linenoise_state_t *l, int flags) {
    if (l->ctx->mlmode)
        refresh_multi_line(l,flags);
    else
        refresh_single_line(l,flags);
}

/* Utility function to avoid specifying REFRESH_ALL all the times. */
static void refresh_line(linenoise_state_t *l) {
    refresh_line_with_flags(l,REFRESH_ALL);
}

/* Hide the current line, when using the multiplexing API. */
void linenoise_hide(linenoise_state_t *l) {
    if (l->ctx->mlmode)
        refresh_multi_line(l,REFRESH_CLEAN);
    else
        refresh_single_line(l,REFRESH_CLEAN);
}

/* Show the current line, when using the multiplexing API. */
void linenoise_show(linenoise_state_t *l) {
    if (l->in_completion) {
        refresh_line_with_completion(l,NULL,REFRESH_WRITE);
    } else {
        refresh_line_with_flags(l,REFRESH_WRITE);
    }
}

/* Insert the character(s) 'c' of length 'clen' at cursor current position.
 * This handles both single-byte ASCII and multi-byte UTF-8 sequences.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
/* Make sure the edit buffer can hold 'needed' bytes plus the nul term.
 * Only dynamic buffers (linenoise_edit_start_dynamic() and the blocking API)
 * can grow: with a caller-provided fixed buffer there is nothing to grow.
 * Returns 0 on success, -1 if the buffer cannot hold that much. */
static int edit_grow(linenoise_state_t *l, size_t needed) {
    size_t newsize;
    char *newbuf;

    if (needed <= l->buflen) return 0;
    if (!l->buf_dynamic || needed > PASTE_MAX_BYTES) return -1;

    /* Grow exponentially, but stop at the configured maximum before the
     * doubling would overflow or go past it. */
    newsize = l->buflen ? l->buflen : 16;
    while (newsize < needed) {
        if (newsize > PASTE_MAX_BYTES/2) {
            newsize = PASTE_MAX_BYTES;
            break;
        }
        newsize *= 2;
    }
    if (newsize < needed) return -1;

    /* Allocate one extra byte for the nul terminator. */
    newbuf = ln_realloc(l->buf, newsize+1);
    if (newbuf == NULL) return -1;
    l->buf = newbuf;
    l->buflen = newsize;
    return 0;
}

/* Insert bytes into l->buf without repainting the prompt. The paste path uses
 * this to first store the real pasted bytes, then mark their range as folded,
 * and only then refresh, so raw pasted newlines are never printed directly.
 * Returns 0 on success, -1 if the bytes don't fit. */
static int edit_insert_no_refresh(linenoise_state_t *l, const char *c, size_t clen) {
    size_t insert_pos = l->pos;

    if (clen > SIZE_MAX-l->len || edit_grow(l,l->len+clen) == -1)
        return -1;

    if (l->len == l->pos) {
        memcpy(l->buf+l->pos, c, clen);
    } else {
        memmove(l->buf+l->pos+clen, l->buf+l->pos, l->len-l->pos);
        memcpy(l->buf+l->pos, c, clen);
    }
    l->pos += clen;
    l->len += clen;
    l->buf[l->len] = '\0';
    adjust_folds_after_insert(l,insert_pos,clen);
    return 0;
}

int linenoise_edit_insert(linenoise_state_t *l, const char *c, size_t clen) {
    if (l->len == l->pos) {
        int needs_refresh = memchr(c, '\n', clen) != NULL ||
                            memchr(c, '\r', clen) != NULL;

        /* Append at end of line. */
        if (edit_insert_no_refresh(l,c,clen) == -1) return 0;
        if (!needs_refresh && !l->ctx->mlmode && !l->ctx->hints_callback && !l->ctx->highlight_callback &&
            (l->ctx->maskmode || l->fold_count == 0))
        {
            if (utf8StrWidth(l->prompt,l->plen)+utf8StrWidth(l->buf,l->len) < l->cols) {
                /* Avoid a full update of the line in the trivial case:
                 * single-width char, no hints, no highlighting, fits in one line. */
                if (l->ctx->maskmode == 1) {
                    if (write(l->ofd,"*",1) == -1) return -1;
                } else {
                    if (write(l->ofd,c,clen) == -1) return -1;
                }
                return 0;
            }
        }
        refresh_line(l);
    } else {
        /* Insert in the middle of the line. */
        if (edit_insert_no_refresh(l,c,clen) == -1) return 0;
        refresh_line(l);
    }
    return 0;
}

/* Move cursor on the left. Moves by one UTF-8 character, not byte. */
void linenoise_edit_move_left(linenoise_state_t *l) {
    if (l->pos > 0) {
        l->pos -= edit_prev_len(l, l->pos);
        refresh_line(l);
    }
}

/* Move cursor on the right. Moves by one UTF-8 character, not byte. */
void linenoise_edit_move_right(linenoise_state_t *l) {
    if (l->pos != l->len) {
        l->pos += edit_next_len(l, l->pos);
        refresh_line(l);
    }
}

/* Move cursor to the start of the line. */
void linenoise_edit_move_home(linenoise_state_t *l) {
    if (l->pos != 0) {
        l->pos = 0;
        refresh_line(l);
    }
}

/* Move cursor to the end of the line. */
void linenoise_edit_move_end(linenoise_state_t *l) {
    if (l->pos != l->len) {
        l->pos = l->len;
        refresh_line(l);
    }
}

/* Move cursor to a specific display column within the buffer.
 * col is 0-based column position relative to start of buffer (not prompt).
 * Used for mouse click positioning. */
static void linenoise_edit_move_to_column(linenoise_state_t *l, int col) {
    size_t pos = 0;
    int current_col = 0;

    /* Walk through the buffer counting display columns until we reach
     * the target column or end of buffer. */
    while (pos < l->len && current_col < col) {
        size_t clen = utf8NextCharLen(l->buf, pos, l->len);
        int cwidth = utf8SingleCharWidth(l->buf + pos, clen);
        /* If this character would put us past the target, stop before it. */
        if (current_col + cwidth > col) {
            break;
        }
        current_col += cwidth;
        pos += clen;
    }

    if (l->pos != pos) {
        l->pos = pos;
        refresh_line(l);
    }
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define LINENOISE_HISTORY_NEXT 0
#define LINENOISE_HISTORY_PREV 1
void linenoise_edit_history_next(linenoise_state_t *l, int dir) {
    /* Browsing stores edits in the session's own line, so it needs one. */
    if (l->ctx->placeholder && l->ctx->history_len > 1) {
        const char *src;
        size_t len;
        struct fold f;

        /* Update the current history entry before to
         * overwrite it with the next one. */
        ln_free(l->ctx->history[l->ctx->history_len - 1 - l->history_index]);
        l->ctx->history[l->ctx->history_len - 1 - l->history_index] = ln_strdup(l->buf);
        /* Show the new entry */
        l->history_index += (dir == LINENOISE_HISTORY_PREV) ? 1 : -1;
        if (l->history_index < 0) {
            l->history_index = 0;
            return;
        } else if (l->history_index >= l->ctx->history_len) {
            l->history_index = l->ctx->history_len-1;
            return;
        }
        /* Copy the selected history entry into the edit buffer. With a
         * fixed-size buffer, truncate the entry if it does not fit. */
        src = l->ctx->history[l->ctx->history_len - 1 - l->history_index];
        len = strlen(src);
        if (edit_grow(l,len) == -1 && len > l->buflen) len = l->buflen;
        memcpy(l->buf,src,len);
        l->buf[len] = '\0';
        l->len = l->pos = len;
        fold_clear(l);

        /* History stores the real text, but not the original paste ranges.
         * If the recalled entry needs folding, create one display fold now
         * so text typed after the recall remains outside the folded range. */
        if (build_history_fold(l,&f)) fold_add(l,f.start,f.end);
        refresh_line(l);
    }
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key.
 * Now handles multi-byte UTF-8 characters. */
void linenoise_edit_delete(linenoise_state_t *l) {
    if (l->len > 0 && l->pos < l->len) {
        size_t clen = edit_next_len(l, l->pos);
        adjust_folds_after_delete(l,l->pos,clen);
        memmove(l->buf+l->pos, l->buf+l->pos+clen, l->len-l->pos-clen);
        l->len -= clen;
        l->buf[l->len] = '\0';
        refresh_line(l);
    }
}

/* Backspace implementation. Deletes the UTF-8 character before the cursor. */
void linenoise_edit_backspace(linenoise_state_t *l) {
    if (l->pos > 0 && l->len > 0) {
        size_t clen = edit_prev_len(l, l->pos);
        adjust_folds_after_delete(l,l->pos-clen,clen);
        memmove(l->buf+l->pos-clen, l->buf+l->pos, l->len-l->pos);
        l->pos -= clen;
        l->len -= clen;
        l->buf[l->len] = '\0';
        refresh_line(l);
    }
}

/* Delete the previous word, maintaining the cursor at the start of the
 * current word. Handles UTF-8 by moving character-by-character. */
void linenoise_edit_delete_prev_word(linenoise_state_t *l) {
    size_t old_pos = l->pos;
    size_t diff;

    /* Skip spaces before the word (move backwards by UTF-8 chars). */
    while (l->pos > 0 && l->buf[l->pos-1] == ' ')
        l->pos -= edit_prev_len(l, l->pos);
    /* Skip non-space characters (move backwards by UTF-8 chars). */
    while (l->pos > 0 && l->buf[l->pos-1] != ' ')
        l->pos -= edit_prev_len(l, l->pos);
    diff = old_pos - l->pos;
    adjust_folds_after_delete(l,l->pos,diff);
    memmove(l->buf+l->pos, l->buf+old_pos, l->len-old_pos+1);
    l->len -= diff;
    refresh_line(l);
}

/* Move cursor to the start of the previous word. */
void linenoise_edit_move_word_left(linenoise_state_t *l) {
    if (l->pos == 0) return;
    /* Skip spaces before the word. */
    while (l->pos > 0 && l->buf[l->pos-1] == ' ')
        l->pos -= utf8PrevCharLen(l->buf, l->pos);
    /* Skip non-space characters. */
    while (l->pos > 0 && l->buf[l->pos-1] != ' ')
        l->pos -= utf8PrevCharLen(l->buf, l->pos);
    refresh_line(l);
}

/* Move cursor to the end of the next word. */
void linenoise_edit_move_word_right(linenoise_state_t *l) {
    if (l->pos >= l->len) return;
    /* Skip current word characters. */
    while (l->pos < l->len && l->buf[l->pos] != ' ')
        l->pos += utf8NextCharLen(l->buf, l->pos, l->len);
    /* Skip spaces after the word. */
    while (l->pos < l->len && l->buf[l->pos] == ' ')
        l->pos += utf8NextCharLen(l->buf, l->pos, l->len);
    refresh_line(l);
}

/* Delete the word to the right of the cursor. */
void linenoise_edit_delete_word_right(linenoise_state_t *l) {
    size_t old_pos = l->pos;
    size_t diff;

    if (l->pos >= l->len) return;
    /* Skip current word characters. */
    while (l->pos < l->len && l->buf[l->pos] != ' ')
        l->pos += utf8NextCharLen(l->buf, l->pos, l->len);
    /* Skip spaces after the word. */
    while (l->pos < l->len && l->buf[l->pos] == ' ')
        l->pos += utf8NextCharLen(l->buf, l->pos, l->len);
    diff = l->pos - old_pos;
    adjust_folds_after_delete(l,old_pos,diff);
    memmove(l->buf+old_pos, l->buf+l->pos, l->len-l->pos+1);
    l->len -= diff;
    l->pos = old_pos;
    refresh_line(l);
}

/* ======================= Undo/Redo Support ================================= */

#define LINENOISE_UNDO_MAX 100  /* Maximum undo stack size */

/* Undo entry structure. */
typedef struct linenoise_undo_entry {
    char *buf;          /* Buffer content snapshot */
    size_t len;         /* Buffer length */
    size_t pos;         /* Cursor position */
} undo_entry_t;

/* Save current state to the session's undo stack. */
static void undo_save(linenoise_state_t *l) {
    undo_entry_t *entry;

    /* Initialize stack if needed. */
    if (l->undo_stack == NULL) {
        l->undo_stack = ln_malloc(sizeof(undo_entry_t) * LINENOISE_UNDO_MAX);
        if (l->undo_stack == NULL) return;
        memset(l->undo_stack, 0, sizeof(undo_entry_t) * LINENOISE_UNDO_MAX);
    }

    /* Discard any redo entries after current position. */
    while (l->undo_len > l->undo_idx) {
        l->undo_len--;
        ln_free(l->undo_stack[l->undo_len].buf);
        l->undo_stack[l->undo_len].buf = NULL;
    }

    /* If stack is full, remove oldest entry. */
    if (l->undo_len >= LINENOISE_UNDO_MAX) {
        ln_free(l->undo_stack[0].buf);
        memmove(l->undo_stack, l->undo_stack + 1, sizeof(undo_entry_t) * (LINENOISE_UNDO_MAX - 1));
        l->undo_len--;
        l->undo_idx--;
    }

    /* Save current state. */
    entry = &l->undo_stack[l->undo_len];
    entry->buf = ln_malloc(l->len + 1);
    if (entry->buf == NULL) return;
    memcpy(entry->buf, l->buf, l->len + 1);
    entry->len = l->len;
    entry->pos = l->pos;
    l->undo_len++;
    l->undo_idx = l->undo_len;
}

/* Undo: restore previous state. */
void linenoise_edit_undo(linenoise_state_t *l) {
    undo_entry_t *entry;

    if (l->undo_stack == NULL || l->undo_idx <= 0) {
        linenoise_beep();
        return;
    }

    /* Save current state for redo if we're at the top. */
    if (l->undo_idx == l->undo_len) {
        undo_save(l);
        l->undo_idx--;  /* Move back one more since undo_save incremented */
    }

    l->undo_idx--;
    entry = &l->undo_stack[l->undo_idx];

    /* Restore state. */
    if (entry->len <= l->buflen) {
        memcpy(l->buf, entry->buf, entry->len + 1);
        l->len = entry->len;
        l->pos = entry->pos;
        fold_clear(l);
        refresh_line(l);
    }
}

/* Redo: restore next state. */
void linenoise_edit_redo(linenoise_state_t *l) {
    undo_entry_t *entry;

    if (l->undo_stack == NULL || l->undo_idx >= l->undo_len - 1) {
        linenoise_beep();
        return;
    }

    l->undo_idx++;
    entry = &l->undo_stack[l->undo_idx];

    /* Restore state. */
    if (entry->len <= l->buflen) {
        memcpy(l->buf, entry->buf, entry->len + 1);
        l->len = entry->len;
        l->pos = entry->pos;
        fold_clear(l);
        refresh_line(l);
    }
}

/* Free the session's undo stack. */
static void undo_free(linenoise_state_t *l) {
    if (l->undo_stack != NULL) {
        for (int i = 0; i < l->undo_len; i++) {
            ln_free(l->undo_stack[i].buf);
        }
        ln_free(l->undo_stack);
        l->undo_stack = NULL;
    }
    l->undo_len = 0;
    l->undo_idx = 0;
}

static void edit_stop(linenoise_state_t *l);

/* Internal: Start a session of 'ctx' on the state 'l'. */
static int edit_start(linenoise_context_t *ctx, linenoise_state_t *l, int stdin_fd, int stdout_fd, char *buf, size_t buflen, const char *prompt) {
    if (ctx->session != NULL) {
        set_error(LINENOISE_ERR_INVALID);
        return -1;
    }

    /* Populate the linenoise state that we pass to functions implementing
     * specific editing functionalities. */
    l->ctx = ctx;
    l->undo_stack = NULL;
    l->undo_len = 0;
    l->undo_idx = 0;
    l->in_completion = 0;
    l->ifd = stdin_fd != -1 ? stdin_fd : STDIN_FILENO;
    l->ofd = stdout_fd != -1 ? stdout_fd : STDOUT_FILENO;
    l->buf = buf;
    l->buflen = buflen;
    l->buf_dynamic = 0;  /* Fixed buffer by default */
    l->prompt = prompt;
    l->plen = strlen(prompt);
    l->oldpos = l->pos = 0;
    l->len = 0;
    fold_clear(l);

    /* Enter raw mode. */
    if (enable_raw_mode(ctx, l->ifd) == -1) return -1;
    ctx->session = l;

    /* Enable mouse tracking if requested. */
    if (ctx->mousemode) {
        enable_mouse_tracking(l->ofd);
    }

    /* Enable bracketed paste, so that large or multi line pastes can be
     * detected and folded on screen. */
    enable_bracketed_paste(l->ofd);

    l->cols = get_columns(stdin_fd, stdout_fd);
    l->oldrows = 0;
    l->oldrpos = 1;  /* Cursor starts on row 1. */
    l->history_index = 0;

    /* Buffer starts empty. */
    l->buf[0] = '\0';
    l->buflen--; /* Make sure there is always space for the nulterm */

    /* If stdin is not a tty, stop here with the initialization. We
     * will actually just read a line from standard input in blocking
     * mode later, in linenoise_edit_feed(). */
    if (!isatty(l->ifd) && !getenv("LINENOISE_ASSUME_TTY")) return 0;

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    if (history_push(ctx, "") == 0) {
        set_error(LINENOISE_ERR_MEMORY);
        edit_stop(l);
        return -1;
    }
    ctx->placeholder = 1;

    if (write(l->ofd,prompt,l->plen) == -1) {
        edit_stop(l);
        return -1;
    }
    return 0;
}

/* Public: Start editing with context support.
 * This is part of the multiplexed API of Linenoise, used for event-driven
 * programs. The context's settings are used for the editing session.
 *
 * Returns 0 on success, -1 on error. */
int linenoise_edit_start(linenoise_context_t *ctx, linenoise_state_t *l,
                         int stdin_fd, int stdout_fd,
                         char *buf, size_t buflen, const char *prompt) {
    if (!ctx) {
        set_error(LINENOISE_ERR_INVALID);
        return -1;
    }
    return edit_start(ctx, l, stdin_fd, stdout_fd, buf, buflen, prompt);
}

/* Start editing with a dynamically-sized buffer.
 * Unlike linenoise_edit_start(), this allocates its own buffer that grows
 * automatically as needed. The buffer is freed when linenoise_edit_stop()
 * is called, and the returned line from linenoise_edit_feed() should be
 * freed with linenoise_free().
 *
 * initial_size: Starting buffer size (will grow as needed). Use 0 for default.
 * Returns 0 on success, -1 on error. */
int linenoise_edit_start_dynamic(linenoise_context_t *ctx, linenoise_state_t *l,
                                 int stdin_fd, int stdout_fd,
                                 size_t initial_size, const char *prompt) {
    char *buf;

    if (!ctx) return -1;

    /* Use default initial size if not specified. */
    if (initial_size == 0) {
        initial_size = 256;
    }

    /* Allocate the initial buffer. */
    buf = ln_malloc(initial_size);
    if (buf == NULL) {
        set_error(LINENOISE_ERR_MEMORY);
        return -1;
    }
    buf[0] = '\0';

    /* Start editing with the allocated buffer. */
    int result = linenoise_edit_start(ctx, l, stdin_fd, stdout_fd, buf, initial_size, prompt);
    if (result == -1) {
        ln_free(buf);
        return -1;
    }

    /* Mark buffer as dynamic so it will be auto-grown and freed. */
    l->buf_dynamic = 1;
    return 0;
}

/* Make sure the temporary paste buffer can hold len+need bytes. Return -1 on
 * allocation failure or if the requested size is over PASTE_MAX_BYTES. */
static int paste_buffer_reserve(char **buf, size_t *cap, size_t len, size_t need) {
    size_t want;
    char *nb;

    /* Nothing to do if the current paste buffer already has room for the
     * bytes collected so far plus the new bytes we want to append. */
    if (*cap >= len + need) return 0;

    /* Start small, then double like the line buffer. The cap avoids turning a
     * huge paste into an unbounded allocation attempt. */
    want = *cap ? *cap : 64;
    while (want < len + need) {
        size_t doubled = want*2;
        if (doubled <= want || doubled > PASTE_MAX_BYTES) {
            want = PASTE_MAX_BYTES;
            break;
        }
        want = doubled;
    }
    if (want < len + need) return -1;

    /* ln_realloc(NULL, want) handles the first allocation too. */
    nb = ln_realloc(*buf, want);
    if (nb == NULL) return -1;
    *buf = nb;
    *cap = want;
    return 0;
}

/* Append bytes to the temporary paste buffer, growing both it and l->buf as
 * needed. Return -1 if the paste is too large or allocation fails. */
static int paste_buffer_append(linenoise_state_t *l, char **buf, size_t *cap,
                               size_t *len, const char *s, size_t slen, size_t maxlen) {
    size_t needed;

    if (*len > maxlen || slen > maxlen-*len) return -1;
    if (*len > SIZE_MAX-slen) return -1;
    needed = *len+slen;
    if (l->len > SIZE_MAX-needed) return -1;
    if (edit_grow(l,l->len+needed) == -1) return -1;
    if (paste_buffer_reserve(buf,cap,*len,slen) == -1) return -1;
    memcpy(*buf+*len,s,slen);
    *len = needed;
    return 0;
}

/* Read a bracketed paste until ESC[201~ and insert the real bytes. If folding
 * is needed, remember the inserted range so that only the rendering is
 * shortened, while the edit buffer keeps the pasted text. */
static void edit_paste(linenoise_state_t *l) {
    static const char END[] = "\x1b[201~";
    const size_t ENDLEN = sizeof(END)-1;
    char *buf = NULL;
    size_t cap = 0, len = 0, match = 0;
    size_t maxlen = l->buf_dynamic ? PASTE_MAX_BYTES : l->buflen;
    int overflowed = 0;

    maxlen = maxlen > l->len ? maxlen - l->len : 0;
    /* Once all fold slots are used, consume later pastes without storing them. */
    if (l->fold_count == LINENOISE_MAX_FOLDS) maxlen = 0;

    while (1) {
        char c;
        if (read(l->ifd, &c, 1) != 1) break;

        /* Track a possible ESC[201~ terminator without copying it into the
         * paste. If it turns out to be ordinary input, flush the partial
         * match below. */
        if (c == END[match]) {
            match++;
            if (match == ENDLEN) break;
            continue;
        }

        if (match > 0) {
            if (!overflowed &&
                paste_buffer_append(l,&buf,&cap,&len,END,match,maxlen) == -1)
                overflowed = 1;
            match = 0;
            if (c == END[0]) {
                match = 1;
                continue;
            }
        }

        if (!overflowed &&
            paste_buffer_append(l,&buf,&cap,&len,&c,1,maxlen) == -1)
            overflowed = 1;
    }

    if (overflowed) {
        ln_free(buf);
        linenoise_beep();
        return;
    }
    if (buf == NULL) return;

    {
        /* Normalize pasted CR and CRLF to LF, so the edit buffer uses one
         * internal newline representation. */
        size_t r = 0, w = 0;
        while (r < len) {
            if (buf[r] == '\r') {
                buf[w++] = '\n';
                r += (r+1 < len && buf[r+1] == '\n') ? 2 : 1;
            } else {
                buf[w++] = buf[r++];
            }
        }
        len = w;
    }

    if (!l->ctx->maskmode && should_fold_text(buf,len)) {
        size_t start = l->pos;
        if (edit_insert_no_refresh(l,buf,len) == -1) {
            ln_free(buf);
            linenoise_beep();
            return;
        }
        fold_add(l,start,start+len);
        refresh_line(l);
    } else {
        linenoise_edit_insert(l,buf,len);
    }
    ln_free(buf);
}

char *linenoise_edit_more = "If you see this, you are misusing the API: when linenoise_edit_feed() is called, if it returns linenoise_edit_more the user is yet editing the line. See the README file for more information.";

/* This function is part of the multiplexed API of linenoise, see the top
 * comment on linenoise_edit_start() for more information. Call this function
 * each time there is some data to read from the standard input file
 * descriptor. In the case of blocking operations, this function can just be
 * called in a loop, and block.
 *
 * The function returns linenoise_edit_more to signal that line editing is still
 * in progress, that is, the user didn't yet pressed enter / CTRL-D. Otherwise
 * the function returns the pointer to the heap-allocated buffer with the
 * edited line, that the user should free with linenoise_free().
 *
 * On special conditions, NULL is returned and errno is populated:
 *
 * EAGAIN if the user pressed Ctrl-C
 * ENOENT if the user pressed Ctrl-D
 *
 * Some other errno: I/O error.
 */
char *linenoise_edit_feed(linenoise_state_t *l) {
    /* Not a TTY, pass control to line reading without character
     * count limits. */
    if (!isatty(l->ifd) && !getenv("LINENOISE_ASSUME_TTY")) return linenoise_no_tty();

    char c;
    int nread;
    char seq[8];  /* Enough for extended sequences like ESC [ 1 ; 5 C */

    nread = read(l->ifd,&c,1);
    if (nread < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? linenoise_edit_more : NULL;
    } else if (nread == 0) {
        return NULL;
    }

    /* Handle Tab key: on continuation lines insert spaces for indentation,
     * otherwise trigger completion if callback is set. */
    if (c == 9 && !l->in_completion) {
        /* Check if cursor is on a continuation line (after a newline). */
        int on_continuation_line = 0;
        size_t i;
        for (i = 0; i < l->pos; i++) {
            if (l->buf[i] == '\n') {
                on_continuation_line = 1;
                break;
            }
        }
        if (on_continuation_line) {
            /* Insert 4 spaces for indentation. */
            if (linenoise_edit_insert(l, "    ", 4)) return NULL;
            return linenoise_edit_more;
        }
    }

    /* Autocomplete when the callback is set. complete_line() returns the
     * character to be handled next, or zero when the key was consumed to
     * navigate the completions (or because there was nothing to complete). */
    if ((l->in_completion || c == 9) && l->ctx->completion_callback != NULL) {
        int retval = complete_line(l,c);
        /* Read next character when 0 */
        if (retval == 0) return linenoise_edit_more;
        c = (char)retval;
    }

    switch(c) {
    case ENTER:    /* enter */
        history_drop_placeholder(l);
        if (l->ctx->mlmode) linenoise_edit_move_end(l);
        if (l->ctx->hints_callback) {
            /* Force a refresh without hints to leave the previous
             * line as the user typed it after a newline. */
            linenoise_hints_cb_t *hc = l->ctx->hints_callback;
            l->ctx->hints_callback = NULL;
            refresh_line(l);
            l->ctx->hints_callback = hc;
        }
        return ln_strdup(l->buf);
    case CTRL_C:     /* ctrl-c */
        history_drop_placeholder(l);
        errno = EAGAIN;
        set_error(LINENOISE_ERR_INTERRUPTED);
        return NULL;
    case BACKSPACE:   /* backspace */
    case 8:     /* ctrl-h */
        undo_save(l);
        linenoise_edit_backspace(l);
        break;
    case CTRL_D:     /* ctrl-d, remove char at right of cursor, or if the
                        line is empty, act as end-of-file. */
        if (l->len > 0) {
            undo_save(l);
            linenoise_edit_delete(l);
        } else {
            history_drop_placeholder(l);
            errno = ENOENT;
            set_error(LINENOISE_ERR_EOF);
            return NULL;
        }
        break;
    case CTRL_T:    /* ctrl-t, swaps current character with previous. */
        /* Handle UTF-8: swap the two UTF-8 characters around cursor. */
        if (l->pos > 0 && l->pos < l->len) {
            char tmp[32];
            size_t prevlen = edit_prev_len(l, l->pos);
            size_t currlen = edit_next_len(l, l->pos);
            size_t prevstart = l->pos - prevlen;
            if (prevlen > sizeof(tmp) || currlen > sizeof(tmp)) break;
            if (range_overlaps_fold(l,prevstart,prevlen+currlen)) {
                linenoise_beep();
                break;
            }
            undo_save(l);
            /* Copy current char to tmp, move previous char right, paste tmp. */
            memcpy(tmp, l->buf + l->pos, currlen);
            memmove(l->buf + prevstart + currlen, l->buf + prevstart, prevlen);
            memcpy(l->buf + prevstart, tmp, currlen);
            if (l->pos + currlen <= l->len) l->pos += currlen;
            refresh_line(l);
        }
        break;
    case CTRL_B:     /* ctrl-b */
        linenoise_edit_move_left(l);
        break;
    case CTRL_F:     /* ctrl-f */
        linenoise_edit_move_right(l);
        break;
    case CTRL_P:    /* ctrl-p */
        linenoise_edit_history_next(l, LINENOISE_HISTORY_PREV);
        break;
    case CTRL_N:    /* ctrl-n */
        linenoise_edit_history_next(l, LINENOISE_HISTORY_NEXT);
        break;
    case ESC:    /* escape sequence */
        /* Read the next two bytes representing the escape sequence.
         * Use timeout to avoid hanging on partial sequences (e.g., user
         * pressing ESC alone). 100ms is enough for terminal responses. */
        if (read_byte_with_timeout(l->ifd,seq,100) != 1) break;
        if (read_byte_with_timeout(l->ifd,seq+1,100) != 1) break;

        /* ESC [ sequences. */
        if (seq[0] == '[') {
            /* SGR mouse event: ESC [ < button ; x ; y M/m */
            if (seq[1] == '<' && l->ctx->mousemode) {
                /* Read the rest of the mouse sequence. */
                char mouse_seq[32];
                int mi = 0;
                while (mi < 30) {
                    if (read_byte_with_timeout(l->ifd, &mouse_seq[mi], 100) != 1) break;
                    if (mouse_seq[mi] == 'M' || mouse_seq[mi] == 'm') {
                        mouse_seq[mi+1] = '\0';
                        break;
                    }
                    mi++;
                }
                if (mi > 0 && (mouse_seq[mi] == 'M' || mouse_seq[mi] == 'm')) {
                    /* Parse: button;x;y */
                    int button = 0, x = 0, y = 0;
                    char *p = mouse_seq;
                    button = atoi(p);
                    while (*p && *p != ';') p++;
                    if (*p == ';') { p++; x = atoi(p); }
                    while (*p && *p != ';') p++;
                    if (*p == ';') { p++; y = atoi(p); }
                    (void)y;  /* Row not used for single-line mode */

                    /* Handle left button press (button 0) for click-to-position.
                     * x is 1-based terminal column. Subtract prompt width to get
                     * position within the edit buffer. */
                    if (button == 0 && mouse_seq[mi] == 'M') {
                        int pwidth = (int)utf8StrWidth(l->prompt, l->plen);
                        int col = x - 1 - pwidth;  /* Convert to 0-based buffer column */
                        if (col >= 0) {
                            linenoise_edit_move_to_column(l, col);
                        } else {
                            /* Click was on prompt, move to start. */
                            linenoise_edit_move_home(l);
                        }
                    }
                }
            }
            else if (seq[1] >= '0' && seq[1] <= '9') {
                /* Extended escape: read the numeric parameter, that can be
                 * more than one digit long (as in the bracketed paste start
                 * sequence ESC [ 200 ~), then the final byte. */
                char param[8];
                size_t plen = 1;
                char final = 0;

                param[0] = seq[1];
                while (plen < sizeof(param)) {
                    char pc;
                    if (read_byte_with_timeout(l->ifd,&pc,100) != 1) break;
                    if (pc >= '0' && pc <= '9') {
                        param[plen++] = pc;
                    } else {
                        final = pc;
                        break;
                    }
                }
                seq[2] = final;
                if (final == '~') {
                    if (plen == 1) {
                        switch(param[0]) {
                        case '3': /* Delete key. */
                            undo_save(l);
                            linenoise_edit_delete(l);
                            break;
                        case '5': /* Page Up - treat as history prev for now */
                            linenoise_edit_history_next(l, LINENOISE_HISTORY_PREV);
                            break;
                        case '6': /* Page Down - treat as history next for now */
                            linenoise_edit_history_next(l, LINENOISE_HISTORY_NEXT);
                            break;
                        }
                    } else if (plen == 3 && memcmp(param,"200",3) == 0) {
                        /* Start of a bracketed paste. */
                        undo_save(l);
                        edit_paste(l);
                    }
                } else if (final == ';') {
                    /* Modified key sequence: ESC [ 1 ; <mod> <key> */
                    if (read_byte_with_timeout(l->ifd,seq+3,100) != 1) break;
                    if (read_byte_with_timeout(l->ifd,seq+4,100) != 1) break;
                    int modifier = seq[3] - '0';
                    /* modifier: 2=Shift, 3=Alt, 5=Ctrl, 7=Ctrl+Alt */
                    if (modifier == 5 || modifier == 3) {  /* Ctrl or Alt */
                        switch(seq[4]) {
                        case 'C': /* Ctrl/Alt+Right - word right */
                            linenoise_edit_move_word_right(l);
                            break;
                        case 'D': /* Ctrl/Alt+Left - word left */
                            linenoise_edit_move_word_left(l);
                            break;
                        }
                    }
                }
            } else {
                switch(seq[1]) {
                case 'A': /* Up */
                    linenoise_edit_history_next(l, LINENOISE_HISTORY_PREV);
                    break;
                case 'B': /* Down */
                    linenoise_edit_history_next(l, LINENOISE_HISTORY_NEXT);
                    break;
                case 'C': /* Right */
                    linenoise_edit_move_right(l);
                    break;
                case 'D': /* Left */
                    linenoise_edit_move_left(l);
                    break;
                case 'H': /* Home */
                    linenoise_edit_move_home(l);
                    break;
                case 'F': /* End*/
                    linenoise_edit_move_end(l);
                    break;
                }
            }
        }

        /* ESC O sequences. */
        else if (seq[0] == 'O') {
            switch(seq[1]) {
            case 'H': /* Home */
                linenoise_edit_move_home(l);
                break;
            case 'F': /* End*/
                linenoise_edit_move_end(l);
                break;
            }
        }

        /* Alt+letter sequences (ESC followed by letter). */
        else if (seq[0] == 'b' || seq[0] == 'B') {
            /* Alt+b: move word left */
            linenoise_edit_move_word_left(l);
        }
        else if (seq[0] == 'f' || seq[0] == 'F') {
            /* Alt+f: move word right */
            linenoise_edit_move_word_right(l);
        }
        else if (seq[0] == 'd' || seq[0] == 'D') {
            /* Alt+d: delete word right */
            undo_save(l);
            linenoise_edit_delete_word_right(l);
        }
        else if (seq[0] == BACKSPACE || seq[0] == CTRL_H) {
            /* Alt+Backspace: delete previous word */
            undo_save(l);
            linenoise_edit_delete_prev_word(l);
        }
        break;
    default:
        /* Handle UTF-8 multi-byte sequences. When we receive the first byte
         * of a multi-byte UTF-8 character, read the remaining bytes to
         * complete the sequence before inserting. */
        {
            char utf8[4];
            int utf8_len = utf8ByteLen(c);
            utf8[0] = c;
            if (utf8_len > 1) {
                /* Read remaining bytes of the UTF-8 sequence. */
                int i;
                for (i = 1; i < utf8_len; i++) {
                    if (read(l->ifd, utf8+i, 1) != 1) break;
                }
            }
            if (linenoise_edit_insert(l, utf8, utf8_len)) return NULL;
        }
        break;
    case CTRL_U: /* Ctrl+u, delete the whole line. */
        undo_save(l);
        l->buf[0] = '\0';
        l->pos = l->len = 0;
        fold_clear(l);
        refresh_line(l);
        break;
    case CTRL_K: /* Ctrl+k, delete from current to end of line. */
        undo_save(l);
        adjust_folds_after_delete(l,l->pos,l->len-l->pos);
        l->buf[l->pos] = '\0';
        l->len = l->pos;
        refresh_line(l);
        break;
    case CTRL_A: /* Ctrl+a, go to the start of the line */
        linenoise_edit_move_home(l);
        break;
    case CTRL_E: /* ctrl+e, go to the end of the line */
        linenoise_edit_move_end(l);
        break;
    case CTRL_L: /* ctrl+l, clear screen */
        clear_screen();
        refresh_line(l);
        break;
    case CTRL_W: /* ctrl+w, delete previous word */
        undo_save(l);
        linenoise_edit_delete_prev_word(l);
        break;
    case CTRL_Y: /* Ctrl+y, redo */
        linenoise_edit_redo(l);
        break;
    case CTRL_Z: /* Ctrl+z, undo */
        linenoise_edit_undo(l);
        break;
    }
    return linenoise_edit_more;
}

/* Internal: End the session and restore the terminal. */
static void edit_stop(linenoise_state_t *l) {
    linenoise_context_t *ctx = l->ctx;

    history_drop_placeholder(l);
    undo_free(l);
    if (ctx->session == l) ctx->session = NULL;

    if (!isatty(l->ifd) && !getenv("LINENOISE_ASSUME_TTY")) return;
    /* Disable mouse tracking before restoring terminal. */
    if (ctx->mousemode) {
        disable_mouse_tracking(l->ofd);
    }
    disable_bracketed_paste(l->ofd);
    disable_raw_mode(ctx);
    printf("\n");
}

/* Public: Stop editing and restore terminal.
 * This is part of the multiplexed linenoise API. Call this when
 * linenoise_edit_feed() returns something other than linenoise_edit_more. */
void linenoise_edit_stop(linenoise_state_t *l) {
    edit_stop(l);

    /* Free dynamic buffer if allocated by linenoise_edit_start_dynamic(). */
    if (l->buf_dynamic && l->buf) {
        ln_free(l->buf);
        l->buf = NULL;
        l->buf_dynamic = 0;
    }
}

/* This just implements a blocking loop for the multiplexed API.
 * In many applications that are not event-drivern, we can just call
 * the blocking linenoise API, wait for the user to complete the editing
 * and return the buffer. */
static char *blocking_edit(linenoise_context_t *ctx, int stdin_fd, int stdout_fd, const char *prompt)
{
    linenoise_state_t l;
    char *buf = ln_malloc(LINENOISE_MAX_LINE);
    char *res;

    if (buf == NULL) {
        errno = ENOMEM;
        set_error(LINENOISE_ERR_MEMORY);
        return NULL;
    }

    if (edit_start(ctx,&l,stdin_fd,stdout_fd,buf,LINENOISE_MAX_LINE,prompt) == -1) {
        ln_free(buf);
        return NULL;
    }
    /* This wrapper owns l.buf, so the edit state is free to grow it in order
     * to hold large pasted input. */
    l.buf_dynamic = 1;
    while((res = linenoise_edit_feed(&l)) == linenoise_edit_more);
    edit_stop(&l);
    ln_free(l.buf);
    return res;
}

/* This special mode is used by linenoise in order to print scan codes
 * on screen for debugging / development purposes. It is implemented
 * by the linenoise_example program using the --keycodes option. */
void linenoise_print_key_codes(void) {
    linenoise_context_t kc;
    char quit[4];

    memset(&kc, 0, sizeof(kc));

    printf("Linenoise key codes debugging mode.\n"
            "Press keys to see scan codes. Type 'quit' at any time to exit.\n");
    if (enable_raw_mode(&kc, STDIN_FILENO) == -1) return;
    memset(quit,' ',4);
    while(1) {
        char c;
        int nread;

        nread = read(STDIN_FILENO,&c,1);
        if (nread <= 0) continue;
        memmove(quit,quit+1,sizeof(quit)-1); /* shift string to left. */
        quit[sizeof(quit)-1] = c; /* Insert current char on the right. */
        if (memcmp(quit,"quit",sizeof(quit)) == 0) break;

        printf("'%c' %02x (%d) (type quit to exit)\n",
            isprint(c) ? c : '?', (int)c, (int)c);
        printf("\r"); /* Go left edge manually, we are in raw mode. */
        fflush(stdout);
    }
    disable_raw_mode(&kc);
}

/* This function is called when linenoise() is called with the standard
 * input file descriptor not attached to a TTY. So for example when the
 * program using linenoise is called in pipe or with a file redirected
 * to its standard input. In this case, we want to be able to return the
 * line regardless of its length (by default we are limited to 4k). */
/* Read a newline terminated record from fp with no fixed-size buffer, so
 * that there is no limit to the length of the returned line. Used for non-tty
 * input, unsupported terminals, and history loading.
 *
 * Returns NULL at EOF (with nothing read) or on error: in the latter case
 * *err, when not NULL, is set to 1. */
static char *read_file_line(FILE *fp, int *err) {
    char *line = NULL;
    size_t len = 0, cap = 0;

    if (err) *err = 0;
    while(1) {
        if (len+1 >= cap) {
            size_t newcap = cap ? cap*2 : 16;
            char *oldval = line;
            if (newcap <= cap) {
                ln_free(line);
                if (err) *err = 1;
                errno = ENOMEM;
                return NULL;
            }
            line = ln_realloc(line,newcap);
            if (line == NULL) {
                if (oldval) ln_free(oldval);
                if (err) *err = 1;
                return NULL;
            }
            cap = newcap;
        }
        int c = fgetc(fp);
        if (c == EOF || c == '\n') {
            if (c == EOF && len == 0) {
                ln_free(line);
                set_error(LINENOISE_ERR_EOF);
                return NULL;
            } else {
                line[len] = '\0';
                return line;
            }
        } else {
            line[len] = c;
            len++;
        }
    }
}

static char *linenoise_no_tty(void) {
    return read_file_line(stdin,NULL);
}

/* Internal: The high level line reading function.
 * This function checks if the terminal has basic capabilities, just checking
 * for a blacklist of stupid terminals, and later either calls the line
 * editing function or uses dummy fgets() so that you will be able to type
 * something even in the most desperate of the conditions. */
static char *read_line(linenoise_context_t *ctx, const char *prompt) {
    if (!isatty(STDIN_FILENO) && !getenv("LINENOISE_ASSUME_TTY")) {
        /* Not a tty: read from file / pipe. In this mode we don't want any
         * limit to the line size, so we call a function to handle that. */
        return linenoise_no_tty();
    } else if (is_unsupported_term()) {
        char *retval;
        size_t len;

        printf("%s",prompt);
        fflush(stdout);
        retval = linenoise_no_tty();
        if (retval == NULL) {
            set_error(LINENOISE_ERR_EOF);
            return NULL;
        }
        len = strlen(retval);
        while(len && retval[len-1] == '\r') {
            len--;
            retval[len] = '\0';
        }
        return retval;
    } else {
        return blocking_edit(ctx,STDIN_FILENO,STDOUT_FILENO,prompt);
    }
}

/* This is just a wrapper the user may want to call in order to make sure
 * the linenoise returned buffer is freed with the same allocator it was
 * created with. Useful when the main program is using an alternative
 * allocator. */
void linenoise_free(void *ptr) {
    if (ptr == linenoise_edit_more) return; /* Protect from API misuse. */
    ln_free(ptr);
}

/* ================================ History ================================= */

/* At exit we'll try to fix the terminal to the initial conditions. */
static void linenoise_at_exit(void) {
    if (raw_ctx) disable_raw_mode(raw_ctx);
}

/* Internal: Append a copy of 'line' to the context history, without the
 * duplicate check. While a session is active its own line stays last, so
 * the new entry goes right before it.
 * It uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries. */
static int history_push(linenoise_context_t *ctx, const char *line) {
    int pinned = ctx->placeholder;
    char *linecopy;
    int pos;

    if (ctx->history_max_len == 0) return 0;

    /* Initialization on first call. */
    if (ctx->history == NULL) {
        ctx->history = ln_malloc(sizeof(char*)*ctx->history_max_len);
        if (ctx->history == NULL) return 0;
        memset(ctx->history,0,(sizeof(char*)*ctx->history_max_len));
    }

    /* No room if the session's own line fills the history. */
    if (ctx->history_max_len <= pinned) return 0;
    linecopy = ln_strdup(line);
    if (!linecopy) return 0;

    /* If we reached the max length, remove the older line. */
    if (ctx->history_len == ctx->history_max_len) {
        ln_free(ctx->history[0]);
        memmove(ctx->history,ctx->history+1,sizeof(char*)*(ctx->history_max_len-1));
        ctx->history_len--;
    }
    pos = ctx->history_len - pinned;
    memmove(ctx->history+pos+1,ctx->history+pos,sizeof(char*)*pinned);
    ctx->history[pos] = linecopy;
    ctx->history_len++;

    /* Keep a session that is browsing history on the same entry. */
    if (pinned && ctx->session->history_index > 0) {
        ctx->session->history_index++;
        if (ctx->session->history_index >= ctx->history_len)
            ctx->session->history_index = ctx->history_len-1;
    }
    return 1;
}

/* Internal: Remove the session's own line from the context history. */
static void history_drop_placeholder(linenoise_state_t *l) {
    linenoise_context_t *ctx = l->ctx;

    if (!ctx->placeholder) return;
    ctx->history_len--;
    ln_free(ctx->history[ctx->history_len]);
    ctx->placeholder = 0;
}

/* ========================= Context-based API ============================== */

/* Create a new linenoise context with default settings. */
linenoise_context_t *linenoise_context_create(void) {
    linenoise_context_t *ctx = ln_malloc(sizeof(linenoise_context_t));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->raw_fd = -1;
    ctx->history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
    return ctx;
}

/* Destroy a linenoise context and free all associated resources. */
void linenoise_context_destroy(linenoise_context_t *ctx) {
    if (!ctx) return;

    /* Restore the terminal and drop the at-exit reference to ctx. */
    disable_raw_mode(ctx);

    /* Free history. */
    if (ctx->history) {
        for (int j = 0; j < ctx->history_len; j++) {
            ln_free(ctx->history[j]);
        }
        ln_free(ctx->history);
    }

    ln_free(ctx);
}

/* Set multi-line mode for a context. */
void linenoise_set_multiline(linenoise_context_t *ctx, int ml) {
    if (ctx) ctx->mlmode = ml;
}

/* Set mask mode for a context (password entry). */
void linenoise_set_mask_mode(linenoise_context_t *ctx, int enable) {
    if (ctx) ctx->maskmode = enable;
}

/* Set mouse mode for a context (click to position cursor). */
void linenoise_set_mouse_mode(linenoise_context_t *ctx, int enable) {
    if (!ctx) return;
    /* Apply the change to an active session's terminal right away. */
    if (ctx->session && ctx->mousemode != enable) {
        if (enable) enable_mouse_tracking(ctx->session->ofd);
        else disable_mouse_tracking(ctx->session->ofd);
    }
    ctx->mousemode = enable;
}

/* Set completion callback for a context. */
void linenoise_set_completion_callback(linenoise_context_t *ctx, linenoise_completion_cb_t *fn) {
    if (ctx) ctx->completion_callback = fn;
}

/* Set hints callback for a context. */
void linenoise_set_hints_callback(linenoise_context_t *ctx, linenoise_hints_cb_t *fn) {
    if (ctx) ctx->hints_callback = fn;
}

/* Set free hints callback for a context. */
void linenoise_set_free_hints_callback(linenoise_context_t *ctx, linenoise_free_hints_cb_t *fn) {
    if (ctx) ctx->free_hints_callback = fn;
}

/* Set syntax highlighting callback for a context. */
void linenoise_set_highlight_callback(linenoise_context_t *ctx, linenoise_highlight_cb_t *fn) {
    if (ctx) ctx->highlight_callback = fn;
}

/* Add a line to history for a context. */
int linenoise_history_add(linenoise_context_t *ctx, const char *line) {
    int last;

    if (!ctx) return 0;

    /* Don't add duplicates of the previous line. */
    last = ctx->history_len - ctx->placeholder - 1;
    if (last >= 0 && !strcmp(ctx->history[last], line)) return 0;
    return history_push(ctx, line);
}

/* Set maximum history length for a context. */
int linenoise_history_set_max_len(linenoise_context_t *ctx, int len) {
    char **new_history;

    if (!ctx) return 0;
    if (len < 1) return 0;

    if (ctx->history) {
        int tocopy = ctx->history_len;
        new_history = ln_malloc(sizeof(char*) * len);
        if (new_history == NULL) return 0;

        if (len < tocopy) {
            int j;
            for (j = 0; j < tocopy - len; j++)
                ln_free(ctx->history[j]);
            tocopy = len;
        }
        memset(new_history, 0, sizeof(char*) * len);
        memcpy(new_history, ctx->history + (ctx->history_len - tocopy),
               sizeof(char*) * tocopy);
        ln_free(ctx->history);
        ctx->history = new_history;
    }
    ctx->history_max_len = len;
    if (ctx->history_len > ctx->history_max_len)
        ctx->history_len = ctx->history_max_len;
    if (ctx->session && ctx->session->history_index >= ctx->history_len)
        ctx->session->history_index = ctx->history_len > 0 ? ctx->history_len-1 : 0;
    return 1;
}

/* Save history to file for a context. */
int linenoise_history_save(linenoise_context_t *ctx, const char *filename) {
    int fd;
    FILE *fp;

    if (!ctx) return -1;

    fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR);
    if (fd == -1) return -1;

    fp = fdopen(fd, "w");
    if (fp == NULL) {
        close(fd);
        return -1;
    }
    for (int j = 0; j < ctx->history_len; j++) {
        /* Keep the history file newline separated: embedded newlines in an
         * entry are stored as CR and converted back by
         * linenoise_history_load(). */
        const char *p = ctx->history[j];
        while (*p) {
            fputc(*p == '\n' ? '\r' : *p, fp);
            p++;
        }
        fputc('\n', fp);
    }
    fclose(fp);
    return 0;
}

/* Load history from file for a context. */
int linenoise_history_load(linenoise_context_t *ctx, const char *filename) {
    FILE *fp;
    char *buf;
    int err = 0;

    if (!ctx) return -1;

    fp = fopen(filename, "r");
    if (fp == NULL) return -1;

    while ((buf = read_file_line(fp, &err)) != NULL) {
        size_t j;
        /* Rebuild embedded newlines that were saved as CR. */
        for (j = 0; buf[j]; j++) {
            if (buf[j] == '\r') buf[j] = '\n';
        }
        linenoise_history_add(ctx, buf);
        ln_free(buf);
    }
    if (err || ferror(fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

/* Main line editing function using a context. */
char *linenoise_read(linenoise_context_t *ctx, const char *prompt) {
    if (!ctx || ctx->session) {
        set_error(LINENOISE_ERR_INVALID);
        return NULL;
    }
    return read_line(ctx, prompt);
}

/* Clear the screen using the context's output (currently just uses stdout). */
void linenoise_clear_screen(linenoise_context_t *ctx) {
    (void)ctx;  /* Currently unused, but kept for API consistency. */
    clear_screen();
}
