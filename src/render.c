/*
 * render.c - Screen rendering for ttabmux
 *
 * Handles drawing the sidebar and the active terminal content area
 * using ANSI escape sequences. Uses a buffered output approach
 * to minimize flicker.
 */

#define _XOPEN_SOURCE 600

#include "ttabmux.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Output buffer helpers                                             */
/* ------------------------------------------------------------------ */

static void buf_ensure(struct ttabmux *t, int need)
{
    if (t->out_len + need >= t->out_cap) {
        t->out_cap = (t->out_cap + need) * 2;
        t->out_buf = realloc(t->out_buf, (size_t)t->out_cap);
    }
}

static void buf_append(struct ttabmux *t, const char *s, int len)
{
    buf_ensure(t, len);
    memcpy(t->out_buf + t->out_len, s, (size_t)len);
    t->out_len += len;
}

static void buf_printf(struct ttabmux *t, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void buf_printf(struct ttabmux *t, const char *fmt, ...)
{
    buf_ensure(t, 256);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t->out_buf + t->out_len, (size_t)(t->out_cap - t->out_len), fmt, ap);
    va_end(ap);
    if (n > 0) t->out_len += n;
}

static void buf_flush(struct ttabmux *t)
{
    if (t->out_len > 0) {
        if (write(STDOUT_FILENO, t->out_buf, (size_t)t->out_len) < 0) {
            /* Ignore write errors during rendering */
        }
        t->out_len = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Color / SGR emission                                              */
/* ------------------------------------------------------------------ */

static void emit_color_fg(struct ttabmux *t, int32_t color)
{
    if (color == COLOR_DEFAULT) {
        buf_append(t, "\033[39m", 5);
    } else if (COLOR_IS_TRUE(color)) {
        buf_printf(t, "\033[38;2;%d;%d;%dm",
                   COLOR_R(color), COLOR_G(color), COLOR_B(color));
    } else if (color >= 0 && color < 8) {
        buf_printf(t, "\033[%dm", 30 + color);
    } else if (color >= 8 && color < 16) {
        buf_printf(t, "\033[%dm", 90 + color - 8);
    } else {
        buf_printf(t, "\033[38;5;%dm", color);
    }
}

static void emit_color_bg(struct ttabmux *t, int32_t color)
{
    if (color == COLOR_DEFAULT) {
        buf_append(t, "\033[49m", 5);
    } else if (COLOR_IS_TRUE(color)) {
        buf_printf(t, "\033[48;2;%d;%d;%dm",
                   COLOR_R(color), COLOR_G(color), COLOR_B(color));
    } else if (color >= 0 && color < 8) {
        buf_printf(t, "\033[%dm", 40 + color);
    } else if (color >= 8 && color < 16) {
        buf_printf(t, "\033[%dm", 100 + color - 8);
    } else {
        buf_printf(t, "\033[48;5;%dm", color);
    }
}

static void emit_sgr(struct ttabmux *t, int32_t fg, int32_t bg, uint8_t attr)
{
    buf_append(t, "\033[0m", 4);  /* Reset first */

    if (attr & ATTR_BOLD)      buf_append(t, "\033[1m", 4);
    if (attr & ATTR_DIM)       buf_append(t, "\033[2m", 4);
    if (attr & ATTR_ITALIC)    buf_append(t, "\033[3m", 4);
    if (attr & ATTR_UNDERLINE) buf_append(t, "\033[4m", 4);
    if (attr & ATTR_BLINK)     buf_append(t, "\033[5m", 4);
    if (attr & ATTR_REVERSE)   buf_append(t, "\033[7m", 4);
    if (attr & ATTR_INVISIBLE) buf_append(t, "\033[8m", 4);
    if (attr & ATTR_STRIKE)    buf_append(t, "\033[9m", 4);

    if (fg != COLOR_DEFAULT) emit_color_fg(t, fg);
    if (bg != COLOR_DEFAULT) emit_color_bg(t, bg);
}

/* ------------------------------------------------------------------ */
/*  UTF-8 encoding helper                                             */
/* ------------------------------------------------------------------ */

static int encode_utf8(uint32_t cp, char *buf)
{
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

/* ------------------------------------------------------------------ */
/*  Sidebar rendering                                                 */
/* ------------------------------------------------------------------ */

static void render_sidebar(struct ttabmux *t)
{
    int sw = t->sidebar_width;

    for (int row = 0; row < t->term_rows; row++) {
        buf_printf(t, "\033[%d;1H", row + 1);  /* Move to row, col 1 */

        if (row == 0) {
            /* Title bar */
            buf_append(t, "\033[0m\033[1;97;44m", 16);  /* Bold white on blue */
            const char *title = " Ttabmux";
            int tlen = (int)strlen(title);
            buf_append(t, title, tlen < sw ? tlen : sw);
            for (int c = tlen; c < sw - 1; c++)
                buf_append(t, " ", 1);
            buf_append(t, "\033[0m", 4);
            /* Border */
            buf_append(t, "\033[90m\xe2\x94\x82\033[0m", 12);  /* dim vertical bar */
        } else if (row == 1) {
            /* Separator under title */
            buf_append(t, "\033[90m", 5);
            for (int c = 0; c < sw - 1; c++)
                buf_append(t, "\xe2\x94\x80", 3);  /* horizontal bar */
            buf_append(t, "\xe2\x94\xbc", 3);  /* cross */
            buf_append(t, "\033[0m", 4);
        } else if (row >= 2 && row - 2 < t->num_sessions) {
            /* Session entry */
            int idx = row - 2;
            int is_active = (idx == t->active);
            int is_alive  = t->sessions[idx].alive;

            if (is_active) {
                buf_append(t, "\033[0;1;97;44m", 13);  /* Bold white on blue */
            } else {
                buf_append(t, "\033[0;37;40m", 11);     /* White on black */
            }

            char line[64];
            int llen;
            if (is_alive) {
                llen = snprintf(line, sizeof(line), " %s%d: %s",
                                is_active ? "> " : "  ",
                                idx + 1,
                                t->sessions[idx].name);
            } else {
                llen = snprintf(line, sizeof(line), " %s%d: [dead]",
                                is_active ? "> " : "  ",
                                idx + 1);
            }

            /* Truncate or pad to sidebar width */
            if (llen > sw - 1) llen = sw - 1;
            buf_append(t, line, llen);
            for (int c = llen; c < sw - 1; c++)
                buf_append(t, " ", 1);

            buf_append(t, "\033[0m", 4);
            buf_append(t, "\033[90m\xe2\x94\x82\033[0m", 12);
        } else if (row == t->term_rows - 2) {
            /* Help hint separator */
            buf_append(t, "\033[90m", 5);
            for (int c = 0; c < sw - 1; c++)
                buf_append(t, "\xe2\x94\x80", 3);
            buf_append(t, "\xe2\x94\xbc", 3);
            buf_append(t, "\033[0m", 4);
        } else if (row == t->term_rows - 1) {
            /* Status bar with keybinding hint */
            buf_append(t, "\033[0;90m", 7);
            const char *hint = " ^B ? help";
            int hlen = (int)strlen(hint);
            buf_append(t, hint, hlen < sw - 1 ? hlen : sw - 1);
            for (int c = hlen; c < sw - 1; c++)
                buf_append(t, " ", 1);
            buf_append(t, "\033[0m", 4);
            buf_append(t, "\033[90m\xe2\x94\x82\033[0m", 12);
        } else {
            /* Empty sidebar row */
            buf_append(t, "\033[0m", 4);
            for (int c = 0; c < sw - 1; c++)
                buf_append(t, " ", 1);
            buf_append(t, "\033[90m\xe2\x94\x82\033[0m", 12);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Help overlay rendering                                            */
/* ------------------------------------------------------------------ */

static void render_help(struct ttabmux *t)
{
    int sw = t->sidebar_width;
    int area_cols = t->term_cols - sw;
    int area_rows = t->term_rows;

    static const char *help_lines[] = {
        "",
        "  Ttabmux - Key Bindings",
        "  ----------------------",
        "",
        "  Prefix key: Ctrl+B",
        "",
        "  c       Create new terminal",
        "  n       Next terminal",
        "  p       Previous terminal",
        "  1-9     Switch to terminal N",
        "  x       Close current terminal",
        "  ,       Rename current terminal",
        "  d       Detach (quit)",
        "  ?       Toggle this help",
        "",
        "  Press any key to close help",
        NULL
    };

    buf_append(t, "\033[0m", 4);

    int nlines = 0;
    while (help_lines[nlines]) nlines++;

    int start_row = (area_rows - nlines) / 2;
    if (start_row < 0) start_row = 0;

    for (int row = 0; row < area_rows; row++) {
        buf_printf(t, "\033[%d;%dH", row + 1, sw + 1);

        int help_idx = row - start_row;
        if (help_idx >= 0 && help_idx < nlines && help_lines[help_idx]) {
            const char *line = help_lines[help_idx];
            int llen = (int)strlen(line);
            if (llen > area_cols) llen = area_cols;
            buf_append(t, "\033[1;36m", 7);  /* Bold cyan */
            buf_append(t, line, llen);
            for (int c = llen; c < area_cols; c++)
                buf_append(t, " ", 1);
        } else {
            for (int c = 0; c < area_cols; c++)
                buf_append(t, " ", 1);
        }
    }
    buf_append(t, "\033[0m", 4);
}

/* ------------------------------------------------------------------ */
/*  Rename mode overlay                                               */
/* ------------------------------------------------------------------ */

static void render_rename(struct ttabmux *t)
{
    int sw = t->sidebar_width;
    int area_cols = t->term_cols - sw;
    int mid_row = t->term_rows / 2;

    buf_printf(t, "\033[%d;%dH", mid_row, sw + 1);
    buf_append(t, "\033[0;1;33m", 9);  /* Bold yellow */

    const char *prompt = " Rename: ";
    int plen = (int)strlen(prompt);
    buf_append(t, prompt, plen);
    buf_append(t, t->rename_buf, t->rename_len);

    int total = plen + t->rename_len;
    for (int c = total; c < area_cols; c++)
        buf_append(t, " ", 1);

    buf_append(t, "\033[0m", 4);
}

/* ------------------------------------------------------------------ */
/*  Terminal content area rendering                                   */
/* ------------------------------------------------------------------ */

static void render_terminal(struct ttabmux *t)
{
    if (t->num_sessions == 0) return;

    struct vterm *vt = &t->sessions[t->active].vt;
    int sw = t->sidebar_width;
    int area_cols = t->term_cols - sw;

    /* Track previous colors to minimize SGR output */
    int32_t prev_fg = COLOR_DEFAULT;
    int32_t prev_bg = COLOR_DEFAULT;
    uint8_t prev_attr = 0;
    int need_sgr_reset = 1;

    for (int row = 0; row < vt->rows && row < t->term_rows; row++) {
        buf_printf(t, "\033[%d;%dH", row + 1, sw + 1);

        for (int col = 0; col < vt->cols && col < area_cols; col++) {
            struct cell *c = vterm_cell(vt, row, col);
            if (!c) {
                buf_append(t, " ", 1);
                continue;
            }

            /* Skip continuation cells (part of a wide character) */
            if (c->width == 0) continue;

            /* Emit SGR if colors/attrs changed */
            if (need_sgr_reset || c->fg != prev_fg || c->bg != prev_bg || c->attr != prev_attr) {
                emit_sgr(t, c->fg, c->bg, c->attr);
                prev_fg = c->fg;
                prev_bg = c->bg;
                prev_attr = c->attr;
                need_sgr_reset = 0;
            }

            /* Emit the character */
            if (c->ch == 0 || c->ch == ' ') {
                buf_append(t, " ", 1);
            } else {
                char utf8[4];
                int n = encode_utf8(c->ch, utf8);
                buf_append(t, utf8, n);
            }
        }
    }

    buf_append(t, "\033[0m", 4);
}

/* ------------------------------------------------------------------ */
/*  Main render function                                              */
/* ------------------------------------------------------------------ */

void render_screen(struct ttabmux *t)
{
    /* Allocate output buffer if needed */
    if (!t->out_buf) {
        t->out_cap = OUT_BUF_INIT;
        t->out_buf = malloc((size_t)t->out_cap);
    }
    t->out_len = 0;

    /* Hide cursor */
    buf_append(t, "\033[?25l", 6);

    /* Render sidebar */
    render_sidebar(t);

    /* Render main area */
    if (t->show_help) {
        render_help(t);
    } else if (t->rename_mode) {
        render_terminal(t);
        render_rename(t);
    } else {
        render_terminal(t);
    }

    /* Position cursor and show it */
    if (t->num_sessions > 0 && !t->show_help && !t->rename_mode) {
        struct vterm *vt = &t->sessions[t->active].vt;
        if (vt->cursor_visible) {
            int cur_screen_row = vt->cursor_row + 1;
            int cur_screen_col = vt->cursor_col + t->sidebar_width + 1;
            buf_printf(t, "\033[%d;%dH", cur_screen_row, cur_screen_col);
            buf_append(t, "\033[?25h", 6);
        }
    }

    buf_flush(t);
}

void render_free(struct ttabmux *t)
{
    free(t->out_buf);
    t->out_buf = NULL;
    t->out_cap = 0;
    t->out_len = 0;
}
