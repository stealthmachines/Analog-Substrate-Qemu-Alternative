/*
 * hdgl_term.h — Terminal API
 */
#pragma once

/* readline return codes */
#define TERM_LINE_OK    0
#define TERM_LINE_EMPTY 1
#define TERM_LINE_EOF   2
#define TERM_LINE_INT   3

void        term_init(void);
void        term_raw(void);
void        term_restore(void);
int         term_width(void);
int         term_color(void);

const char *term_cyan(void);
const char *term_red(void);
const char *term_green(void);
const char *term_yellow(void);
const char *term_purple(void);
const char *term_blue(void);
const char *term_reset(void);
const char *term_bold(void);

/* Returns TERM_LINE_* */
int  term_readline(const char *prompt, char *buf, int bufmax);
int  term_readline_simple(const char *prompt, char *buf, int bufmax);

void term_rule(char ch);
void term_clear(void);
