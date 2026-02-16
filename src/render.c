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

    /* Session list area: rows 1 .. term_rows-2 */
    int list_start = 1;
    int list_end = t->term_rows - 2; /* exclusive: reserve 2 rows for buttons */
    int list_rows = list_end - list_start;
    if (list_rows < 1) list_rows = 1;

    /* Total items: sessions only ([+] is now in title bar) */
    int total_items = t->num_sessions;
    int need_scroll = (total_items > list_rows);

    /* Content width: reserve 1 col for scrollbar if needed */
    int content_w = sw - 1; /* -1 for the border */
    if (need_scroll) content_w -= 1; /* -1 for scrollbar */
    if (content_w < 1) content_w = 1;

    /* Clamp sidebar_scroll to valid range */
    if (need_scroll) {
        if (t->sidebar_scroll > total_items - list_rows)
            t->sidebar_scroll = total_items - list_rows;
    } else {
        t->sidebar_scroll = 0;
    }
    if (t->sidebar_scroll < 0)
        t->sidebar_scroll = 0;

    /* Scrollbar thumb */
    int thumb_start = 0, thumb_end = 0;
    if (need_scroll) {
        int thumb_h = (list_rows * list_rows) / total_items;
        if (thumb_h < 1) thumb_h = 1;
        int max_scroll = total_items - list_rows;
        if (max_scroll < 1) max_scroll = 1;
        int thumb_top = (t->sidebar_scroll * (list_rows - thumb_h)) / max_scroll;
        thumb_start = thumb_top;
        thumb_end = thumb_top + thumb_h;
    }

    /* Border style: highlight when hovering or dragging the resize edge */
    int hover = (t->sidebar_hover || t->sidebar_drag);
    /* Pre-built border strings: SGR + "│" + reset */
    const char *bdr_vert   = hover
        ? "\033[1;36m\xe2\x94\x82\033[0m"    /* bold cyan │ */
        : "\033[90m\xe2\x94\x82\033[0m";      /* dim gray │ */
    int bdr_vert_len       = hover ? 13 : 12;
    for (int row = 0; row < t->term_rows; row++) {
        buf_printf(t, "\033[%d;1H", row + 1);

        if (row == 0) {
            /* Title bar with [+] button on the right */
            int has_new = (t->num_sessions < MAX_SESSIONS);
            const char *title = " Ttabmux";
            int tlen = (int)strlen(title);
            const char *newbtn = "[+]";
            int nblen = has_new ? (int)strlen(newbtn) : 0;
            int fill = sw - 1 - tlen - nblen;
            if (fill < 0) fill = 0;

            buf_append(t, "\033[0m\033[1;97;44m", 15);
            buf_append(t, title, tlen < sw - 1 ? tlen : sw - 1);
            for (int c = 0; c < fill; c++)
                buf_append(t, " ", 1);
            if (has_new) {
                if (t->sidebar_newbtn_hover)
                    buf_append(t, "\033[0;1;33;44m", 12); /* bold yellow on blue */
                else
                    buf_append(t, "\033[0;97;44m", 10);   /* white on blue */
                buf_append(t, newbtn, nblen);
            }
            buf_append(t, "\033[0m", 4);
            buf_append(t, bdr_vert, bdr_vert_len);
        } else if (row >= list_start && row < list_end) {
            /* Session list area */
            int slot = row - list_start;
            int item_idx = slot + t->sidebar_scroll;

            if (item_idx < t->num_sessions) {
                /* Session entry */
                int idx = item_idx;
                int is_active = (idx == t->active);
                int is_alive  = session_is_alive(&t->sessions[idx]);

                if (is_active) {
                    buf_append(t, "\033[0;1;90;47m", 13);
                } else {
                    buf_append(t, "\033[0;37;40m", 11);
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

                if (llen > content_w) llen = content_w;
                buf_append(t, line, llen);
                for (int c = llen; c < content_w; c++)
                    buf_append(t, " ", 1);
                buf_append(t, "\033[0m", 4);
            } else {
                /* Empty slot */
                buf_append(t, "\033[0m", 4);
                for (int c = 0; c < content_w; c++)
                    buf_append(t, " ", 1);
            }

            /* Scrollbar or padding before border */
            if (need_scroll) {
                if (slot >= thumb_start && slot < thumb_end) {
                    buf_append(t, "\033[0;37m\xe2\x96\x88\033[0m", 14);
                } else {
                    buf_append(t, "\033[0;90m\xe2\x96\x91\033[0m", 14);
                }
            }
            buf_append(t, bdr_vert, bdr_vert_len);
        } else if (row == t->term_rows - 2) {
            /* Action button row: V| H─ / X */
            int btn_w = (sw - 1) / 4;
            int used = 0;

            /* Split Vertical button */
            if (t->sidebar_btn_hover == 3)
                buf_append(t, "\033[0;1;32;40m", 12);  /* bold green */
            else
                buf_append(t, "\033[0;90m", 7);         /* dim gray */
            {
                const char *lbl = " V| ";
                int llen = (int)strlen(lbl);
                if (llen > btn_w) llen = btn_w;
                buf_append(t, lbl, llen);
                for (int c = llen; c < btn_w; c++)
                    buf_append(t, " ", 1);
            }
            used += btn_w;
            buf_append(t, "\033[0m", 4);

            /* Split Horizontal button */
            if (t->sidebar_btn_hover == 4)
                buf_append(t, "\033[0;1;32;40m", 12);  /* bold green */
            else
                buf_append(t, "\033[0;90m", 7);         /* dim gray */
            {
                int printed = 0;
                buf_append(t, " H", 2); printed += 2;
                buf_append(t, "\xe2\x94\x80", 3); printed += 1; /* display width 1 */
                buf_append(t, " ", 1); printed += 1;
                for (int c = printed; c < btn_w; c++)
                    buf_append(t, " ", 1);
            }
            used += btn_w;
            buf_append(t, "\033[0m", 4);

            /* Search button */
            if (t->sidebar_btn_hover == 5)
                buf_append(t, "\033[0;1;33;40m", 12);  /* bold yellow */
            else
                buf_append(t, "\033[0;90m", 7);         /* dim gray */
            {
                const char *lbl = "  / ";
                int llen = (int)strlen(lbl);
                if (llen > btn_w) llen = btn_w;
                buf_append(t, lbl, llen);
                for (int c = llen; c < btn_w; c++)
                    buf_append(t, " ", 1);
            }
            used += btn_w;
            buf_append(t, "\033[0m", 4);

            /* Close button (gets remainder) */
            int btn_w4 = (sw - 1) - used;
            if (t->sidebar_btn_hover == 6)
                buf_append(t, "\033[0;1;31;40m", 12);  /* bold red */
            else
                buf_append(t, "\033[0;90m", 7);         /* dim gray */
            {
                const char *lbl = "  X ";
                int llen = (int)strlen(lbl);
                if (llen > btn_w4) llen = btn_w4;
                buf_append(t, lbl, llen);
                for (int c = llen; c < btn_w4; c++)
                    buf_append(t, " ", 1);
            }
            buf_append(t, "\033[0m", 4);

            buf_append(t, bdr_vert, bdr_vert_len);
        } else if (row == t->term_rows - 1) {
            /* Bottom button bar: [Help] [Quit] */
            int btn_w = (sw - 1) / 2;  /* half width each */
            int btn_w2 = (sw - 1) - btn_w;

            /* Help button */
            if (t->sidebar_btn_hover == 1)
                buf_append(t, "\033[0;1;36;40m", 12);  /* bold cyan on black */
            else
                buf_append(t, "\033[0;90m", 7);         /* dim gray */
            const char *help_lbl = " ? Help";
            int hlen = (int)strlen(help_lbl);
            if (hlen > btn_w) hlen = btn_w;
            buf_append(t, help_lbl, hlen);
            for (int c = hlen; c < btn_w; c++)
                buf_append(t, " ", 1);
            buf_append(t, "\033[0m", 4);

            /* Quit button */
            if (t->sidebar_btn_hover == 2)
                buf_append(t, "\033[0;1;31;40m", 12);  /* bold red on black */
            else
                buf_append(t, "\033[0;90m", 7);         /* dim gray */
            const char *quit_lbl = " x Quit";
            int qlen = (int)strlen(quit_lbl);
            if (qlen > btn_w2) qlen = btn_w2;
            buf_append(t, quit_lbl, qlen);
            for (int c = qlen; c < btn_w2; c++)
                buf_append(t, " ", 1);
            buf_append(t, "\033[0m", 4);

            buf_append(t, bdr_vert, bdr_vert_len);
        } else {
            /* Empty sidebar row */
            buf_append(t, "\033[0m", 4);
            for (int c = 0; c < sw - 1; c++)
                buf_append(t, " ", 1);
            buf_append(t, bdr_vert, bdr_vert_len);
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
        "  x       Close current pane/tab",
        "  v       Vertical split",
        "  s       Horizontal split",
        "  o       Cycle pane focus",
        "  Arrows  Navigate panes",
        "  ,       Rename current terminal",
        "  /       Search (n/N to navigate)",
        "  g       Jump to line number",
        "  d       Detach (quit)",
        "  ?       Toggle this help",
        "",
        "  ESC x3  Quick quit",
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
/*  Action bar rendering (search / jump-to-line)                      */
/* ------------------------------------------------------------------ */

static void render_action_bar(struct ttabmux *t)
{
    int sw = t->sidebar_width;
    int area_cols = t->term_cols - sw;

    /* Draw on the last row of the terminal area */
    buf_printf(t, "\033[%d;%dH", t->term_rows, sw + 1);
    buf_append(t, "\033[0;1;33;44m", 12);  /* Bold yellow on blue */

    int total = 0;

    if (t->action_mode == 3) {
        /* Search navigation mode */
        buf_append(t, " / ", 3);
        buf_append(t, t->action_buf, t->action_len);
        total = 3 + t->action_len;

        /* Show hints */
        const char *hint;
        if (t->search_match_line < 0)
            hint = "  [not found]  n:next N:prev ESC:close";
        else
            hint = "  n:next N:prev ESC:close";
        int hlen = (int)strlen(hint);
        if (total + hlen < area_cols) {
            buf_append(t, "\033[0;37;44m", 10);  /* Normal white on blue */
            buf_append(t, hint, hlen);
            total += hlen;
        }
    } else {
        /* Search input or jump-to-line mode */
        const char *prefix = (t->action_mode == 1) ? " / " : " : ";
        int plen = (int)strlen(prefix);
        buf_append(t, prefix, plen);
        buf_append(t, t->action_buf, t->action_len);
        total = plen + t->action_len;

        /* Show "not found" hint in search input mode */
        if (t->action_mode == 1 && t->action_len > 0 &&
            t->search_match_line < 0) {
            const char *nf = " [not found]";
            int nflen = (int)strlen(nf);
            if (total + nflen < area_cols) {
                buf_append(t, nf, nflen);
                total += nflen;
            }
        }
    }

    buf_append(t, "\033[0;1;33;44m", 12);  /* Restore for padding */
    for (int c = total; c < area_cols; c++)
        buf_append(t, " ", 1);

    buf_append(t, "\033[0m", 4);

    /* Position cursor — show in input modes, hide in nav mode */
    if (t->action_mode != 3) {
        int plen = (t->action_mode == 1) ? 3 : 3;
        buf_printf(t, "\033[%d;%dH", t->term_rows,
                   sw + 1 + plen + t->action_len);
        buf_append(t, "\033[?25h", 6);
    }
}

/* ------------------------------------------------------------------ */
/*  Terminal content area rendering                                   */
/* ------------------------------------------------------------------ */

/* Render a single pane's vterm content (multi-pane mode, with per-pane scrollbar) */
static void render_pane_content(struct ttabmux *t, struct pane *p)
{
    struct vterm *vt = &p->vt;
    int sw = t->sidebar_width;
    int so = vt->scroll_offset;
    if (so > vt->sb_len) so = vt->sb_len;

    /* Per-pane scrollbar */
    int show_sb = pane_has_scrollbar(p);
    int content_w = show_sb ? (p->w - 1) : p->w;
    if (content_w < 1) content_w = 1;

    /* Scrollbar thumb position */
    int sb_thumb_start = 0, sb_thumb_end = 0;
    if (show_sb) {
        int total_lines = vt->sb_len + vt->rows;
        int visible = vt->rows;
        if (visible > total_lines) visible = total_lines;
        int track_h = p->h;
        int thumb_h = (visible * track_h) / total_lines;
        if (thumb_h < 1) thumb_h = 1;
        int max_offset = vt->sb_len;
        if (max_offset < 1) max_offset = 1;
        int thumb_top = ((max_offset - so) * (track_h - thumb_h)) / max_offset;
        sb_thumb_start = thumb_top;
        sb_thumb_end = thumb_top + thumb_h;
    }

    /* Only show search/selection highlights on the active pane */
    int is_active_pane = (p == cur_pane(t));

    int32_t prev_fg = COLOR_DEFAULT;
    int32_t prev_bg = COLOR_DEFAULT;
    uint8_t prev_attr = 0;
    int need_sgr_reset = 1;

    for (int row = 0; row < p->h && p->y + row < t->term_rows; row++) {
        buf_printf(t, "\033[%d;%dH", p->y + row + 1, sw + p->x + 1);

        struct cell *line = NULL;
        int sb_depth = so - row;
        if (sb_depth > 0) {
            line = vterm_sb_line(vt, sb_depth);
        } else {
            int screen_row = row - so;
            if (screen_row >= 0 && screen_row < vt->rows)
                line = &vt->cells[screen_row * vt->cols];
        }

        if (!line) {
            buf_append(t, "\033[0m", 4);
            for (int col = 0; col < content_w; col++)
                buf_append(t, " ", 1);
            need_sgr_reset = 1;
        } else {
            int abs_line = vt->sb_len - so + row;
            int highlight_match = (is_active_pane &&
                                   (t->action_mode == 1 || t->action_mode == 3) &&
                                   t->search_match_line >= 0 &&
                                   abs_line == t->search_match_line);

            int sel_on_row = 0;
            int sel_sl = t->sel_start_line, sel_sc = t->sel_start_col;
            int sel_el = t->sel_end_line, sel_ec = t->sel_end_col;
            if (t->sel_active && is_active_pane) {
                if (sel_sl > sel_el || (sel_sl == sel_el && sel_sc > sel_ec)) {
                    int tmp;
                    tmp = sel_sl; sel_sl = sel_el; sel_el = tmp;
                    tmp = sel_sc; sel_sc = sel_ec; sel_ec = tmp;
                }
                if (abs_line >= sel_sl && abs_line <= sel_el)
                    sel_on_row = 1;
            }

            for (int col = 0; col < vt->cols && col < content_w; col++) {
                struct cell *c = &line[col];
                if (c->width == 0) continue;

                int in_match = (highlight_match &&
                                col >= t->search_match_col &&
                                col < t->search_match_col + t->search_match_len);

                int in_sel = 0;
                if (sel_on_row) {
                    if (sel_sl == sel_el) {
                        in_sel = (col >= sel_sc && col <= sel_ec);
                    } else if (abs_line == sel_sl) {
                        in_sel = (col >= sel_sc);
                    } else if (abs_line == sel_el) {
                        in_sel = (col <= sel_ec);
                    } else {
                        in_sel = 1;
                    }
                }

                if (in_match) {
                    buf_append(t, "\033[0;1;30;43m", 12);
                    prev_fg = COLOR_DEFAULT;
                    prev_bg = COLOR_DEFAULT;
                    prev_attr = 0;
                    need_sgr_reset = 1;
                } else if (in_sel) {
                    buf_append(t, "\033[7m", 4);
                    prev_fg = COLOR_DEFAULT;
                    prev_bg = COLOR_DEFAULT;
                    prev_attr = 0;
                    need_sgr_reset = 1;
                } else if (need_sgr_reset || c->fg != prev_fg || c->bg != prev_bg || c->attr != prev_attr) {
                    emit_sgr(t, c->fg, c->bg, c->attr);
                    prev_fg = c->fg;
                    prev_bg = c->bg;
                    prev_attr = c->attr;
                    need_sgr_reset = 0;
                }

                if (c->ch == 0 || c->ch == ' ') {
                    buf_append(t, " ", 1);
                } else {
                    char utf8[4];
                    int n = encode_utf8(c->ch, utf8);
                    buf_append(t, utf8, n);
                }
            }
        }

        /* Draw per-pane scrollbar at the rightmost column */
        if (show_sb) {
            buf_printf(t, "\033[%d;%dH", p->y + row + 1, sw + p->x + p->w);
            if (row >= sb_thumb_start && row < sb_thumb_end) {
                buf_append(t, "\033[0;37m\xe2\x96\x88\033[0m", 14);
            } else {
                buf_append(t, "\033[0;90m\xe2\x96\x91\033[0m", 14);
            }
        }
    }

    buf_append(t, "\033[0m", 4);
}

/* Draw dividers between panes by traversing the layout tree */
static void render_dividers(struct ttabmux *t, struct session *s,
                            int node_idx, int x, int y, int w, int h)
{
    if (node_idx < 0 || !s->layout[node_idx].used) return;
    struct layout_node *n = &s->layout[node_idx];
    if (n->is_leaf) return;

    int sw = t->sidebar_width;
    struct pane *ap = &s->panes[s->active_pane];

    /* Determine divider color: drag/hover > adjacent > default */
    int is_drag_or_hover = (t->split_drag && t->split_drag_node == node_idx) ||
                           (!t->split_drag && t->split_hover_node == node_idx);

    if (n->split_type == SPLIT_VERT) {
        int avail = w - 1;
        if (avail < 2) avail = 2;
        int left_w = (int)(avail * n->split_ratio);
        if (left_w < 1) left_w = 1;
        int right_w = avail - left_w;
        if (right_w < 1) right_w = 1;

        int div_x = x + left_w;  /* content-area column of divider */
        const char *color;
        int clen;
        if (is_drag_or_hover) {
            color = "\033[1;33m";  /* bold yellow */
            clen = 7;
        } else {
            int adjacent = (ap->x + ap->w == div_x) || (ap->x == div_x + 1);
            color = adjacent ? "\033[1;36m" : "\033[90m";
            clen = adjacent ? 7 : 5;
        }

        for (int r = y; r < y + h && r < t->term_rows; r++) {
            buf_printf(t, "\033[%d;%dH", r + 1, sw + div_x + 1);
            buf_append(t, color, clen);
            buf_append(t, "\xe2\x94\x82", 3);  /* U+2502 │ */
        }
        buf_append(t, "\033[0m", 4);

        render_dividers(t, s, n->child[0], x, y, left_w, h);
        render_dividers(t, s, n->child[1], x + left_w + 1, y, right_w, h);
    } else {
        int avail = h - 1;
        if (avail < 2) avail = 2;
        int top_h = (int)(avail * n->split_ratio);
        if (top_h < 1) top_h = 1;
        int bot_h = avail - top_h;
        if (bot_h < 1) bot_h = 1;

        int div_y = y + top_h;  /* content-area row of divider */
        const char *color;
        int clen;
        if (is_drag_or_hover) {
            color = "\033[1;33m";  /* bold yellow */
            clen = 7;
        } else {
            int adjacent = (ap->y + ap->h == div_y) || (ap->y == div_y + 1);
            color = adjacent ? "\033[1;36m" : "\033[90m";
            clen = adjacent ? 7 : 5;
        }

        buf_printf(t, "\033[%d;%dH", div_y + 1, sw + x + 1);
        buf_append(t, color, clen);
        for (int c = 0; c < w; c++)
            buf_append(t, "\xe2\x94\x80", 3);  /* U+2500 ─ */
        buf_append(t, "\033[0m", 4);

        render_dividers(t, s, n->child[0], x, y, w, top_h);
        render_dividers(t, s, n->child[1], x, y + top_h + 1, w, bot_h);
    }
}

static void render_terminal(struct ttabmux *t)
{
    if (t->num_sessions == 0) return;

    struct session *s = &t->sessions[t->active];

    if (s->num_panes > 1) {
        /* Multi-pane mode: render each pane, then draw dividers */
        for (int i = 0; i < s->num_panes; i++)
            render_pane_content(t, &s->panes[i]);

        int cw = t->term_cols - t->sidebar_width;
        int ch = t->term_rows;
        render_dividers(t, s, s->root_node, 0, 0, cw, ch);
        return;
    }

    /* Single-pane mode: full rendering with gutter and scrollbar */
    struct pane *p = &s->panes[0];
    struct vterm *vt = &p->vt;
    int sw = t->sidebar_width;
    int area_cols = t->term_cols - sw;
    int so = vt->scroll_offset;
    if (so > vt->sb_len) so = vt->sb_len;

    /* Line number gutter */
    int show_linenr = (vt->sb_len > 0 && !vt->alt_active);
    int gutter_w = 0;
    if (show_linenr) {
        int max_line = vt->sb_len + vt->rows;
        gutter_w = 1;
        for (int n = max_line; n >= 10; n /= 10)
            gutter_w++;
        gutter_w += 1;
    }

    /* Scrollbar */
    int show_scrollbar = (vt->sb_len > 0 && !vt->alt_active);
    int sb_col = t->term_cols;
    int content_start = sw + 1 + gutter_w;
    int content_cols = area_cols - gutter_w;
    int thumb_start = 0, thumb_end = 0;

    if (show_scrollbar) {
        content_cols -= 1;
        if (content_cols < 1) content_cols = 1;

        int total_lines = vt->sb_len + vt->rows;
        int visible = vt->rows;
        if (visible > total_lines) visible = total_lines;

        int track_h = t->term_rows;
        int thumb_h = (visible * track_h) / total_lines;
        if (thumb_h < 1) thumb_h = 1;

        int max_offset = vt->sb_len;
        if (max_offset < 1) max_offset = 1;
        int thumb_top = ((max_offset - so) * (track_h - thumb_h)) / max_offset;
        thumb_start = thumb_top;
        thumb_end = thumb_top + thumb_h;
    }

    int32_t prev_fg = COLOR_DEFAULT;
    int32_t prev_bg = COLOR_DEFAULT;
    uint8_t prev_attr = 0;
    int need_sgr_reset = 1;

    for (int row = 0; row < vt->rows && row < t->term_rows; row++) {
        if (show_linenr) {
            buf_printf(t, "\033[%d;%dH", row + 1, sw + 1);
            int line_nr = vt->sb_len - so + row + 1;
            if (line_nr >= 1 && line_nr <= vt->sb_len + vt->rows) {
                buf_printf(t, "\033[0;90m%*d \033[0m", gutter_w - 1, line_nr);
            } else {
                buf_printf(t, "\033[0m%*s", gutter_w, "");
            }
            need_sgr_reset = 1;
        }

        buf_printf(t, "\033[%d;%dH", row + 1, content_start);

        struct cell *line = NULL;
        int sb_depth = so - row;
        if (sb_depth > 0) {
            line = vterm_sb_line(vt, sb_depth);
        } else {
            int screen_row = row - so;
            if (screen_row >= 0 && screen_row < vt->rows)
                line = &vt->cells[screen_row * vt->cols];
        }

        if (!line) {
            buf_append(t, "\033[0m", 4);
            for (int col = 0; col < content_cols; col++)
                buf_append(t, " ", 1);
            need_sgr_reset = 1;
            continue;
        }

        int abs_line = vt->sb_len - so + row;
        int highlight_match = ((t->action_mode == 1 || t->action_mode == 3) &&
                               t->search_match_line >= 0 &&
                               abs_line == t->search_match_line);

        int sel_on_row = 0;
        int sel_sl = t->sel_start_line, sel_sc = t->sel_start_col;
        int sel_el = t->sel_end_line, sel_ec = t->sel_end_col;
        if (t->sel_active) {
            if (sel_sl > sel_el || (sel_sl == sel_el && sel_sc > sel_ec)) {
                int tmp;
                tmp = sel_sl; sel_sl = sel_el; sel_el = tmp;
                tmp = sel_sc; sel_sc = sel_ec; sel_ec = tmp;
            }
            if (abs_line >= sel_sl && abs_line <= sel_el)
                sel_on_row = 1;
        }

        for (int col = 0; col < vt->cols && col < content_cols; col++) {
            struct cell *c = &line[col];
            if (c->width == 0) continue;

            int in_match = (highlight_match &&
                            col >= t->search_match_col &&
                            col < t->search_match_col + t->search_match_len);

            int in_sel = 0;
            if (sel_on_row) {
                if (sel_sl == sel_el) {
                    in_sel = (col >= sel_sc && col <= sel_ec);
                } else if (abs_line == sel_sl) {
                    in_sel = (col >= sel_sc);
                } else if (abs_line == sel_el) {
                    in_sel = (col <= sel_ec);
                } else {
                    in_sel = 1;
                }
            }

            if (in_match) {
                buf_append(t, "\033[0;1;30;43m", 12);
                prev_fg = COLOR_DEFAULT;
                prev_bg = COLOR_DEFAULT;
                prev_attr = 0;
                need_sgr_reset = 1;
            } else if (in_sel) {
                buf_append(t, "\033[7m", 4);
                prev_fg = COLOR_DEFAULT;
                prev_bg = COLOR_DEFAULT;
                prev_attr = 0;
                need_sgr_reset = 1;
            } else if (need_sgr_reset || c->fg != prev_fg || c->bg != prev_bg || c->attr != prev_attr) {
                emit_sgr(t, c->fg, c->bg, c->attr);
                prev_fg = c->fg;
                prev_bg = c->bg;
                prev_attr = c->attr;
                need_sgr_reset = 0;
            }

            if (c->ch == 0 || c->ch == ' ') {
                buf_append(t, " ", 1);
            } else {
                char utf8[4];
                int n = encode_utf8(c->ch, utf8);
                buf_append(t, utf8, n);
            }
        }
    }

    if (show_scrollbar) {
        for (int row = 0; row < vt->rows && row < t->term_rows; row++) {
            buf_printf(t, "\033[%d;%dH", row + 1, sb_col);
            if (row >= thumb_start && row < thumb_end) {
                buf_append(t, "\033[0;37m\xe2\x96\x88\033[0m", 14);
            } else {
                buf_append(t, "\033[0;90m\xe2\x96\x91\033[0m", 14);
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
        if (t->action_mode)
            render_action_bar(t);
    }

    /* Position cursor and show it */
    if (t->num_sessions > 0 && !t->show_help && !t->rename_mode && !t->action_mode) {
        struct pane *p = cur_pane(t);
        if (p) {
            struct vterm *vt = &p->vt;
            if (vt->cursor_visible && vt->scroll_offset == 0) {
                struct session *s = &t->sessions[t->active];
                int cur_screen_row, cur_screen_col;
                if (s->num_panes > 1) {
                    cur_screen_row = p->y + vt->cursor_row + 1;
                    cur_screen_col = t->sidebar_width + p->x + vt->cursor_col + 1;
                } else {
                    cur_screen_row = vt->cursor_row + 1;
                    int gw = pane_gutter_width(p);
                    cur_screen_col = vt->cursor_col + t->sidebar_width + 1 + gw;
                }
                buf_printf(t, "\033[%d;%dH", cur_screen_row, cur_screen_col);
                buf_append(t, "\033[?25h", 6);
            }
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
