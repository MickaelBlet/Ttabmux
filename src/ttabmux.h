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
#include <time.h>

#define MAX_SESSIONS    100
#define MAX_PANES       16
#define MAX_LAYOUT_NODES (MAX_PANES * 2 - 1)
#define SIDEBAR_WIDTH   25
#define MAX_ESC_BUF     256
#define PREFIX_KEY      0x02  /* Ctrl+B */
#define OUT_BUF_INIT    (1 << 16)
#define SCROLLBACK_MAX  100000

#define SPLIT_VERT      0   /* left | right */
#define SPLIT_HORIZ     1   /* top / bottom */

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
    /* Mouse tracking mode requested by the child app */
    int mouse_mode;  /* 0=off, 1000/1002/1003=tracking level */
    /* Window title (set via OSC 0/2) */
    char title[256];
    int title_changed;
    /* Scrollback buffer */
    struct cell *scrollback;  /* ring buffer: SCROLLBACK_MAX * cols cells */
    int sb_len;               /* lines stored (0..SCROLLBACK_MAX) */
    int sb_head;              /* ring buffer write position */
    int scroll_offset;        /* view offset: 0=live, >0=scrolled into history */
    /* Response buffer for DA/DSR queries (written back to PTY by caller) */
    char resp_buf[128];
    int resp_len;
    /* Horizontal scroll offset (wide mode) */
    int col_offset;
};

/* A terminal pane (one PTY within a tab) */
struct pane {
    int pty_fd;
    pid_t pid;
    struct vterm vt;
    int alive;
    int x, y, w, h;  /* region in content area coords */
};

/* Binary layout tree node (array-based) */
struct layout_node {
    int used;
    int is_leaf;        /* 1=pane, 0=split */
    int split_type;     /* SPLIT_VERT or SPLIT_HORIZ */
    float split_ratio;  /* fraction for child[0], default 0.5 */
    int parent;         /* index, -1=root */
    int child[2];       /* indices, -1=none */
    int pane_idx;       /* for leaves: index into panes[] */
};

/* A terminal session (tab) containing one or more panes */
struct session {
    struct pane panes[MAX_PANES];
    int num_panes;
    int active_pane;
    struct layout_node layout[MAX_LAYOUT_NODES];
    int root_node;
    char name[256];
    int renamed;            /* 1 = user renamed; suppress OSC title updates */
    int bell;
    int wide_cols;          /* 0 = normal, >0 = PTY column count for wide mode */
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
    const char *default_shell;  /* shell for new tabs (Ctrl+B c) */
    int show_help;
    /* Rename mode */
    int rename_mode;
    char rename_buf[256];
    int rename_len;
    int rename_cursor_row;   /* screen row of rename cursor (set by render) */
    int rename_cursor_col;   /* screen col of rename cursor (set by render) */
    /* Wide mode input */
    int wide_mode;           /* 1 = inputting wide cols value */
    char wide_buf[16];
    int wide_len;
    /* Action bar (search / jump-to-line) */
    int action_mode;         /* 0=off, 1=search, 2=jump-to-line */
    char action_buf[256];
    int action_len;
    int search_match_line;   /* absolute line of current match (-1=none) */
    int search_match_col;    /* column of current match start */
    int search_match_len;    /* length of match in cells */
    int search_match_index;  /* 1-based index of current match */
    int search_match_total;  /* total number of matches */
    /* Sidebar drag resize */
    int sidebar_drag;
    int sidebar_hover;      /* mouse is on the resize border */
    /* Sidebar session list scroll offset */
    int sidebar_scroll;
    int sidebar_sb_drag;    /* dragging the sidebar scrollbar */
    /* Scrollbar drag */
    int scrollbar_drag;
    /* Horizontal scrollbar drag */
    int hscrollbar_drag;
    int hscrollbar_drag_pane;   /* pane index being dragged */
    /* Split pane divider drag/resize */
    int split_drag;
    int split_drag_node;
    int split_drag_x, split_drag_y, split_drag_w, split_drag_h;
    int split_hover_node;    /* layout node of hovered divider, -1 = none */
    /* Sidebar bottom button hover: 0=none, 1=help, 2=quit, 3=split-v, 4=split-h, 5=search, 6=close */
    int sidebar_btn_hover;
    /* Sidebar [+] new tab button hover */
    int sidebar_newbtn_hover;
    /* Triple-ESC quit detection */
    int esc_count;          /* consecutive lone ESC presses */
    /* Input escape sequence buffer (for mouse parsing) */
    int inp_state;        /* 0=normal, 1=ESC, 2=CSI, 3=CSI< (SGR mouse) */
    char inp_buf[64];
    int inp_len;
    /* Output buffer for rendering */
    char *out_buf;
    int out_len;
    int out_cap;
    /* Text selection */
    int sel_active;          /* 1 = selection exists (visible highlight) */
    int sel_dragging;        /* 1 = mouse is held down, updating sel_end */
    int sel_start_line;      /* absolute line (0-indexed across sb+screen) */
    int sel_start_col;       /* cell column */
    int sel_end_line;        /* absolute line of end of selection */
    int sel_end_col;         /* cell column of end of selection */
    int sel_click_count;     /* 1=single, 2=double(word), 3=triple(line) */
    struct timespec sel_last_click; /* timestamp of last click for multi-click */
    int sel_last_click_line; /* line of last click */
    int sel_last_click_col;  /* col of last click */
};

/* Inline helpers */

static inline struct pane *cur_pane(struct ttabmux *t) {
    if (t->num_sessions == 0) return NULL;
    struct session *s = &t->sessions[t->active];
    return &s->panes[s->active_pane];
}

static inline int pane_gutter_width(struct pane *p) {
    (void)p;
    return 0;
}

static inline int pane_has_scrollbar(struct pane *p) {
    return (p->vt.sb_len > 0 && !p->vt.alt_active) ? 1 : 0;
}

static inline int pane_needs_hscroll(struct pane *p, int visible_w) {
    return (p->vt.cols > visible_w) ? 1 : 0;
}

static inline int session_is_alive(struct session *s) {
    for (int i = 0; i < s->num_panes; i++)
        if (s->panes[i].alive) return 1;
    return 0;
}

/* vterm.c */
void vterm_init(struct vterm *vt, int rows, int cols);
void vterm_free(struct vterm *vt);
void vterm_resize(struct vterm *vt, int rows, int cols);
void vterm_process(struct vterm *vt, const char *data, int len);
struct cell *vterm_cell(struct vterm *vt, int row, int col);
struct cell *vterm_sb_line(struct vterm *vt, int depth);

/* render.c */
void render_screen(struct ttabmux *t);
void render_free(struct ttabmux *t);

/* main.c helpers */
int  session_create(struct ttabmux *t, const char *shell);
void session_close(struct ttabmux *t, int idx);

#endif /* TTABMUX_H */
