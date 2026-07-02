/*
 * hdgl_term.c — Terminal Line Editor
 * ====================================
 *
 * Proper terminal handling for the Router64 shell:
 *   - Raw mode input (no line buffering)
 *   - Backspace, Ctrl-U (clear line), Ctrl-C (interrupt)
 *   - History: up/down arrow keys (last 64 commands)
 *   - Terminal width detection
 *   - Color detection (NO_COLOR env var)
 *   - Proper restoration on exit
 */

#include "hdgl_term.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>

#define HIST_MAX    64
#define LINE_MAX    256

static struct termios g_orig;
static int            g_raw = 0;
static int            g_color = 1;
static int            g_width = 80;

/* History ring buffer */
static char  g_hist[HIST_MAX][LINE_MAX];
static int   g_hist_count = 0;   /* total entries added */
static int   g_hist_pos   = 0;   /* current position during browse */

/* Signal flag */
static volatile int g_interrupted = 0;
static void on_sigint(int s) { (void)s; g_interrupted = 1; }

/* ── Init / cleanup ────────────────────────────────────────────────────── */
void term_init(void) {
    /* Color: disabled if NO_COLOR set or not a tty */
    g_color = !getenv("NO_COLOR") && isatty(STDOUT_FILENO);

    /* Terminal width */
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        g_width = ws.ws_col;

    /* SIGINT handler */
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
}

void term_raw(void) {
    if (!isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &g_orig);
    struct termios t = g_orig;
    t.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ISIG);
    t.c_iflag &= ~(IXON);
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    g_raw = 1;
}

void term_restore(void) {
    if (g_raw && isatty(STDIN_FILENO)) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig);
        g_raw = 0;
    }
}

int term_width(void)  { return g_width; }
int term_color(void)  { return g_color; }

/* ── Color helpers ─────────────────────────────────────────────────────── */
const char *term_cyan(void)   { return g_color ? "\033[1;36m" : ""; }
const char *term_red(void)    { return g_color ? "\033[1;31m" : ""; }
const char *term_green(void)  { return g_color ? "\033[1;32m" : ""; }
const char *term_yellow(void) { return g_color ? "\033[1;33m" : ""; }
const char *term_purple(void) { return g_color ? "\033[1;35m" : ""; }
const char *term_blue(void)   { return g_color ? "\033[0;34m" : ""; }
const char *term_reset(void)  { return g_color ? "\033[0m"    : ""; }
const char *term_bold(void)   { return g_color ? "\033[1m"    : ""; }

/* ── History ───────────────────────────────────────────────────────────── */
static void hist_add(const char *line) {
    if (!line || !line[0]) return;
    /* Don't add duplicate of last entry */
    if (g_hist_count > 0) {
        int last = (g_hist_count-1) % HIST_MAX;
        if (strcmp(g_hist[last], line)==0) return;
    }
    strncpy(g_hist[g_hist_count % HIST_MAX], line, LINE_MAX-1);
    g_hist_count++;
    g_hist_pos = g_hist_count;
}

static const char *hist_prev(void) {
    if (g_hist_count == 0) return NULL;
    if (g_hist_pos <= 0) return NULL;
    g_hist_pos--;
    if (g_hist_pos < 0) g_hist_pos = 0;
    int idx = g_hist_pos % HIST_MAX;
    /* Make sure this slot is valid */
    if (g_hist_pos >= g_hist_count) return NULL;
    return g_hist[idx];
}

static const char *hist_next(void) {
    if (g_hist_pos >= g_hist_count) return "";
    g_hist_pos++;
    if (g_hist_pos >= g_hist_count) { g_hist_pos = g_hist_count; return ""; }
    return g_hist[g_hist_pos % HIST_MAX];
}

/* ── Redraw current line ───────────────────────────────────────────────── */
static void redraw(const char *prompt, const char *buf, int pos) {
    /* Move to start of line, clear it, reprint */
    printf("\r\033[K");     /* CR + erase to end of line */
    printf("%s", prompt);
    printf("%s", buf);
    /* Position cursor */
    int plen = 0;
    /* Count visible prompt chars (skip ANSI escapes) */
    for (const char *p = prompt; *p; p++) {
        if (*p == '\033') { while (*p && *p != 'm') p++; }
        else plen++;
    }
    int cur_col = plen + pos;
    int end_col = plen + (int)strlen(buf);
    if (cur_col < end_col) {
        /* Move cursor left by (end-pos) */
        printf("\033[%dD", end_col - cur_col);
    }
    fflush(stdout);
}

/* ── Read a line with full editing ────────────────────────────────────── */
/*
 * Returns: TERM_LINE_OK    — line in buf, len > 0
 *          TERM_LINE_EMPTY — empty line (just Enter)
 *          TERM_LINE_EOF   — Ctrl-D / EOF
 *          TERM_LINE_INT   — Ctrl-C / interrupted
 */
int term_readline(const char *prompt, char *buf, int bufmax) {
    buf[0] = '\0';
    int pos = 0;
    int len = 0;
    g_hist_pos = g_hist_count;
    g_interrupted = 0;

    printf("%s", prompt); fflush(stdout);

    while (1) {
        /* Check signal */
        if (g_interrupted) {
            printf("\n"); fflush(stdout);
            g_interrupted = 0;
            return TERM_LINE_INT;
        }

        int c = getchar();
        if (c == EOF) {
            printf("\n"); fflush(stdout);
            return TERM_LINE_EOF;
        }

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            printf("\n"); fflush(stdout);
            if (len > 0) hist_add(buf);
            return len > 0 ? TERM_LINE_OK : TERM_LINE_EMPTY;
        }

        if (c == 3) { /* Ctrl-C */
            printf("^C\n"); fflush(stdout);
            buf[0] = '\0';
            return TERM_LINE_INT;
        }

        if (c == 4) { /* Ctrl-D — EOF */
            if (len == 0) {
                printf("\n"); fflush(stdout);
                return TERM_LINE_EOF;
            }
            /* Ctrl-D with content: delete char at cursor */
            if (pos < len) {
                memmove(buf+pos, buf+pos+1, len-pos);
                len--;
                redraw(prompt, buf, pos);
            }
            continue;
        }

        if (c == 21) { /* Ctrl-U — clear line */
            buf[0] = '\0'; pos = 0; len = 0;
            redraw(prompt, buf, pos);
            continue;
        }

        if (c == 11) { /* Ctrl-K — kill to end of line */
            buf[pos] = '\0'; len = pos;
            redraw(prompt, buf, pos);
            continue;
        }

        if (c == 1) { /* Ctrl-A — start of line */
            pos = 0; redraw(prompt, buf, pos);
            continue;
        }

        if (c == 5) { /* Ctrl-E — end of line */
            pos = len; redraw(prompt, buf, pos);
            continue;
        }

        if (c == 127 || c == 8) { /* Backspace */
            if (pos > 0) {
                memmove(buf+pos-1, buf+pos, len-pos+1);
                pos--; len--;
                redraw(prompt, buf, pos);
            }
            continue;
        }

        if (c == 27) { /* Escape sequence */
            int c2 = getchar();
            if (c2 == '[') {
                int c3 = getchar();
                switch (c3) {
                case 'A': { /* Up arrow — history prev */
                    const char *h = hist_prev();
                    if (h) { strncpy(buf, h, bufmax-1); buf[bufmax-1]='\0'; len=strlen(buf); pos=len; redraw(prompt,buf,pos); }
                    break;
                }
                case 'B': { /* Down arrow — history next */
                    const char *h = hist_next();
                    if (h) { strncpy(buf, h, bufmax-1); buf[bufmax-1]='\0'; len=strlen(buf); pos=len; redraw(prompt,buf,pos); }
                    break;
                }
                case 'C': /* Right arrow */
                    if (pos < len) { pos++; redraw(prompt,buf,pos); }
                    break;
                case 'D': /* Left arrow */
                    if (pos > 0) { pos--; redraw(prompt,buf,pos); }
                    break;
                case '3': { /* Delete (Esc [ 3 ~) */
                    int c4 = getchar();
                    if (c4 == '~' && pos < len) {
                        memmove(buf+pos, buf+pos+1, len-pos);
                        len--;
                        redraw(prompt,buf,pos);
                    }
                    break;
                }
                case 'H': /* Home */
                    pos=0; redraw(prompt,buf,pos); break;
                case 'F': /* End */
                    pos=len; redraw(prompt,buf,pos); break;
                }
            }
            continue;
        }

        /* Printable character — insert at cursor */
        if (c >= 32 && c < 127 && len < bufmax-1) {
            memmove(buf+pos+1, buf+pos, len-pos+1);
            buf[pos] = (char)c;
            pos++; len++;
            redraw(prompt, buf, pos);
        }
    }
}

/* ── Non-raw fallback (for pipes/scripts) ─────────────────────────────── */
int term_readline_simple(const char *prompt, char *buf, int bufmax) {
    printf("%s", prompt); fflush(stdout);
    if (!fgets(buf, bufmax, stdin)) return TERM_LINE_EOF;
    size_t l = strlen(buf);
    if (l > 0 && buf[l-1]=='\n') { buf[--l]='\0'; }
    return l > 0 ? TERM_LINE_OK : TERM_LINE_EMPTY;
}

/* ── Print horizontal rule ────────────────────────────────────────────── */
void term_rule(char ch) {
    int w = g_width < 72 ? g_width : 72;
    if (g_color) printf("\033[0;90m");
    for (int i=0; i<w; i++) putchar(ch);
    if (g_color) printf("\033[0m");
    putchar('\n');
}

/* ── Clear screen ─────────────────────────────────────────────────────── */
void term_clear(void) {
    if (g_color) printf("\033[2J\033[H");
    fflush(stdout);
}
