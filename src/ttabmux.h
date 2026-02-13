/*
 * ttabmux - Terminal multiplexer with sidebar
 * Copyright (c) 2026 Mickael Blet
 * MIT License
 */

#ifndef TTABMUX_H
#define TTABMUX_H

#include <stdint.h>
#include <sys/types.h>
#include <termios.h>

#define MAX_SESSIONS    10
#define SIDEBAR_WIDTH   20
#define MAX_ESC_BUF     256
#define PREFIX_KEY      0x02  /* Ctrl+B */
#define OUT_BUF_INIT    (1 << 16)

/* Cell attributes */
#define ATTR_BOLD       (1 << 0)
#define ATTR_DIM        (1 << 1)
#define ATTR_ITALIC     (1 << 2)
#define ATTR_UNDERLINE  (1 << 3)
#define ATTR_BLINK      (1 << 4)
#define ATTR_REVERSE    (1 << 5)
#define ATTR_INVISIBLE  (1 << 6)
#define ATTR_STRIKE     (1 << 7)

/* Special color value for "default" */
#define COLOR_DEFAULT   (-1)
/* True color flag: color = 0x01000000 | (R << 16) | (G << 8) | B */
#define COLOR_TRUE      0x01000000
#define COLOR_IS_TRUE(c) ((c) & COLOR_TRUE)
#define COLOR_R(c)      (((c) >> 16) & 0xFF)
#define COLOR_G(c)      (((c) >> 8) & 0xFF)
#define COLOR_B(c)      ((c) & 0xFF)

/* VTerm parser states */
enum vt_state {
    VT_NORMAL,
    VT_ESC,
    VT_CSI,
    VT_OSC,
    VT_DCS,
    VT_CHARSET,
};

/* A single cell on the virtual terminal grid */
struct cell {
    uint32_t ch;          /* Unicode codepoint (0 for empty/continuation) */
    int32_t  fg;          /* Foreground color */
    int32_t  bg;          /* Background color */
    uint8_t  attr;        /* Cell attributes (bold, underline, etc.) */
    uint8_t  width;       /* Display width: 1=normal, 2=wide, 0=continuation */
};

/* Virtual terminal emulator */
struct vterm {
    struct cell *cells;
    struct cell *alt_cells;
    int rows, cols;
    int cursor_row, cursor_col;
    int saved_row, saved_col;
    int32_t cur_fg, cur_bg;
    uint8_t cur_attr;
    int scroll_top, scroll_bottom;
    enum vt_state state;
    char esc_buf[MAX_ESC_BUF];
    int esc_len;
    int auto_wrap;
    int wrap_pending;
    int origin_mode;
    int insert_mode;
    int cursor_visible;
    int alt_active;
    int alt_saved_row, alt_saved_col;
    uint8_t *tabstops;
    /* UTF-8 decode state */
    uint32_t utf8_cp;
    int utf8_remaining;
    /* Bracketed paste mode */
    int bracketed_paste;
    /* Application cursor keys */
    int app_cursor;
};

/* A terminal session */
struct session {
    int pty_fd;
    pid_t pid;
    struct vterm vt;
    char name[64];
    int alive;
    int bell;
};

/* Main application state */
struct ttabmux {
    struct session sessions[MAX_SESSIONS];
    int num_sessions;
    int active;
    int sidebar_width;
    int term_rows, term_cols;
    struct termios orig_termios;
    int running;
    int prefix_mode;
    int show_help;
    /* Rename mode */
    int rename_mode;
    char rename_buf[64];
    int rename_len;
    /* Output buffer for rendering */
    char *out_buf;
    int out_len;
    int out_cap;
};

/* vterm.c */
void vterm_init(struct vterm *vt, int rows, int cols);
void vterm_free(struct vterm *vt);
void vterm_resize(struct vterm *vt, int rows, int cols);
void vterm_process(struct vterm *vt, const char *data, int len);
struct cell *vterm_cell(struct vterm *vt, int row, int col);

/* render.c */
void render_screen(struct ttabmux *t);
void render_free(struct ttabmux *t);

/* main.c helpers */
int  session_create(struct ttabmux *t, const char *shell);
void session_close(struct ttabmux *t, int idx);

#endif /* TTABMUX_H */
