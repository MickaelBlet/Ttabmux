/*
 * vterm.c - Virtual terminal emulator
 *
 * Implements a VT100/xterm-compatible terminal emulator that maintains
 * a cell grid. Supports CSI sequences for cursor movement, colors (256 +
 * true color), scrolling, alternate screen buffer, and more.
 */

#define _XOPEN_SOURCE 600

#include "ttabmux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/*  Response buffer (written back to PTY by caller)                   */
/* ------------------------------------------------------------------ */

static void vterm_respond(struct vterm *vt, const char *s, int len)
{
    int avail = (int)sizeof(vt->resp_buf) - vt->resp_len;
    if (len > avail) len = avail;
    if (len <= 0) return;
    memcpy(vt->resp_buf + vt->resp_len, s, (size_t)len);
    vt->resp_len += len;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

static void clear_cell(struct cell *c)
{
    c->ch    = ' ';
    c->fg    = COLOR_DEFAULT;
    c->bg    = COLOR_DEFAULT;
    c->attr  = 0;
    c->width = 1;
}

static void clear_cell_attr(struct cell *c, int32_t fg, int32_t bg, uint8_t attr)
{
    c->ch    = ' ';
    c->fg    = fg;
    c->bg    = bg;
    c->attr  = attr;
    c->width = 1;
}

static struct cell *grid_cell(struct vterm *vt, int row, int col)
{
    if (row < 0 || row >= vt->rows || col < 0 || col >= vt->cols)
        return NULL;
    return &vt->cells[row * vt->cols + col];
}

static void clear_region(struct vterm *vt, int r1, int c1, int r2, int c2)
{
    for (int r = r1; r <= r2 && r < vt->rows; r++) {
        int cs = (r == r1) ? c1 : 0;
        int ce = (r == r2) ? c2 : vt->cols - 1;
        for (int c = cs; c <= ce && c < vt->cols; c++)
            clear_cell_attr(grid_cell(vt, r, c), vt->cur_fg, vt->cur_bg, 0);
    }
}

static void scroll_up(struct vterm *vt, int n)
{
    int top = vt->scroll_top;
    int bot = vt->scroll_bottom;
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;

    /* Save scrolled-off lines to scrollback (main screen only) */
    if (top == 0 && !vt->alt_active && vt->scrollback) {
        int save = n;
        if (save > vt->rows) save = vt->rows;
        for (int i = 0; i < save; i++) {
            memcpy(&vt->scrollback[vt->sb_head * vt->cols],
                   &vt->cells[i * vt->cols],
                   (size_t)vt->cols * sizeof(struct cell));
            vt->sb_head = (vt->sb_head + 1) % SCROLLBACK_MAX;
            if (vt->sb_len < SCROLLBACK_MAX) vt->sb_len++;
        }
    }

    /* Move lines up */
    memmove(&vt->cells[top * vt->cols],
            &vt->cells[(top + n) * vt->cols],
            (size_t)(bot - top + 1 - n) * (size_t)vt->cols * sizeof(struct cell));

    /* Clear bottom lines */
    for (int r = bot - n + 1; r <= bot; r++)
        for (int c = 0; c < vt->cols; c++)
            clear_cell(grid_cell(vt, r, c));
}

static void scroll_down(struct vterm *vt, int n)
{
    int top = vt->scroll_top;
    int bot = vt->scroll_bottom;
    if (n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;

    /* Move lines down */
    memmove(&vt->cells[(top + n) * vt->cols],
            &vt->cells[top * vt->cols],
            (size_t)(bot - top + 1 - n) * (size_t)vt->cols * sizeof(struct cell));

    /* Clear top lines */
    for (int r = top; r < top + n; r++)
        for (int c = 0; c < vt->cols; c++)
            clear_cell(grid_cell(vt, r, c));
}

static void clamp_cursor(struct vterm *vt)
{
    if (vt->cursor_row < 0) vt->cursor_row = 0;
    if (vt->cursor_row >= vt->rows) vt->cursor_row = vt->rows - 1;
    if (vt->cursor_col < 0) vt->cursor_col = 0;
    if (vt->cursor_col >= vt->cols) vt->cursor_col = vt->cols - 1;
}

static void reset_tabstops(struct vterm *vt)
{
    if (!vt->tabstops) return;
    memset(vt->tabstops, 0, (size_t)vt->cols);
    for (int i = 0; i < vt->cols; i += 8)
        vt->tabstops[i] = 1;
}

/* ------------------------------------------------------------------ */
/*  Init / Free / Resize                                              */
/* ------------------------------------------------------------------ */

void vterm_init(struct vterm *vt, int rows, int cols)
{
    memset(vt, 0, sizeof(*vt));
    vt->rows = rows;
    vt->cols = cols;
    vt->cells = calloc((size_t)(rows * cols), sizeof(struct cell));
    vt->alt_cells = calloc((size_t)(rows * cols), sizeof(struct cell));
    vt->tabstops = calloc((size_t)cols, 1);
    vt->scrollback = calloc((size_t)(SCROLLBACK_MAX * cols), sizeof(struct cell));

    for (int i = 0; i < rows * cols; i++) {
        clear_cell(&vt->cells[i]);
        clear_cell(&vt->alt_cells[i]);
    }

    vt->cur_fg = COLOR_DEFAULT;
    vt->cur_bg = COLOR_DEFAULT;
    vt->cur_attr = 0;
    vt->scroll_top = 0;
    vt->scroll_bottom = rows - 1;
    vt->auto_wrap = 1;
    vt->cursor_visible = 1;
    vt->state = VT_NORMAL;

    reset_tabstops(vt);
}

void vterm_free(struct vterm *vt)
{
    free(vt->cells);
    free(vt->alt_cells);
    free(vt->tabstops);
    free(vt->scrollback);
    vt->cells = NULL;
    vt->alt_cells = NULL;
    vt->tabstops = NULL;
    vt->scrollback = NULL;
}

void vterm_resize(struct vterm *vt, int rows, int cols)
{
    if (rows == vt->rows && cols == vt->cols) return;

    int old_cols = vt->cols;

    struct cell *new_cells     = calloc((size_t)(rows * cols), sizeof(struct cell));
    struct cell *new_alt_cells = calloc((size_t)(rows * cols), sizeof(struct cell));
    uint8_t     *new_tabs      = calloc((size_t)cols, 1);

    for (int i = 0; i < rows * cols; i++) {
        clear_cell(&new_cells[i]);
        clear_cell(&new_alt_cells[i]);
    }

    /* Copy old content */
    int copy_rows = (rows < vt->rows) ? rows : vt->rows;
    int copy_cols = (cols < vt->cols) ? cols : vt->cols;

    /* If the cursor was below the new height, shift the view */
    int row_offset = 0;
    if (vt->cursor_row >= rows) {
        row_offset = vt->cursor_row - rows + 1;
    }

    for (int r = 0; r < copy_rows && (r + row_offset) < vt->rows; r++) {
        for (int c = 0; c < copy_cols; c++) {
            new_cells[r * cols + c] = vt->cells[(r + row_offset) * vt->cols + c];
        }
    }

    /* Copy alt screen similarly */
    for (int r = 0; r < copy_rows; r++)
        for (int c = 0; c < copy_cols; c++)
            new_alt_cells[r * cols + c] = vt->alt_cells[r * vt->cols + c];

    free(vt->cells);
    free(vt->alt_cells);
    free(vt->tabstops);

    vt->cells     = new_cells;
    vt->alt_cells = new_alt_cells;
    vt->tabstops  = new_tabs;
    vt->rows = rows;
    vt->cols = cols;

    /* Reset tabstops */
    for (int i = 0; i < cols; i += 8)
        vt->tabstops[i] = 1;

    /* Adjust cursor */
    vt->cursor_row -= row_offset;
    clamp_cursor(vt);

    /* Fix scroll region */
    vt->scroll_top = 0;
    vt->scroll_bottom = rows - 1;
    vt->wrap_pending = 0;

    /* Scrollback: resize lines when column width changes */
    if (cols != old_cols) {
        struct cell *new_sb = calloc((size_t)(SCROLLBACK_MAX * cols), sizeof(struct cell));
        if (vt->scrollback && vt->sb_len > 0) {
            int copy_c = (cols < old_cols) ? cols : old_cols;
            for (int i = 0; i < vt->sb_len; i++) {
                int old_idx = (vt->sb_head - vt->sb_len + i + SCROLLBACK_MAX) % SCROLLBACK_MAX;
                struct cell *src = &vt->scrollback[old_idx * old_cols];
                struct cell *dst = &new_sb[i * cols];
                for (int c = 0; c < copy_c; c++)
                    dst[c] = src[c];
                for (int c = copy_c; c < cols; c++)
                    clear_cell(&dst[c]);
            }
        }
        free(vt->scrollback);
        vt->scrollback = new_sb;
        vt->sb_head = vt->sb_len % SCROLLBACK_MAX;
    }
    if (vt->scroll_offset > vt->sb_len)
        vt->scroll_offset = vt->sb_len;
}

struct cell *vterm_cell(struct vterm *vt, int row, int col)
{
    return grid_cell(vt, row, col);
}

struct cell *vterm_sb_line(struct vterm *vt, int depth)
{
    if (depth < 1 || depth > vt->sb_len || !vt->scrollback)
        return NULL;
    int idx = (vt->sb_head - depth + SCROLLBACK_MAX) % SCROLLBACK_MAX;
    return &vt->scrollback[idx * vt->cols];
}

/* ------------------------------------------------------------------ */
/*  CSI parameter parsing                                             */
/* ------------------------------------------------------------------ */

#define MAX_CSI_PARAMS 16

static int parse_csi_params(const char *buf, int len, int *params, int *nparam, int *priv)
{
    *nparam = 0;
    *priv = 0;
    int i = 0;

    /* Check for private marker */
    if (i < len && (buf[i] == '?' || buf[i] == '>' || buf[i] == '!')) {
        *priv = buf[i];
        i++;
    }

    /* Parse semicolon-separated decimal numbers */
    int cur = 0;
    int has_digit = 0;
    while (i < len - 1) {  /* -1 because last char is the command */
        char c = buf[i];
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            has_digit = 1;
        } else if (c == ';' || c == ':') {
            if (*nparam < MAX_CSI_PARAMS) {
                params[(*nparam)++] = has_digit ? cur : -1; /* -1 = missing */
            }
            cur = 0;
            has_digit = 0;
        } else {
            break;
        }
        i++;
    }
    /* Last parameter */
    if (has_digit || *nparam > 0) {
        if (*nparam < MAX_CSI_PARAMS)
            params[(*nparam)++] = has_digit ? cur : -1;
    }

    return buf[len - 1]; /* Return command character */
}

static int param_or(int *params, int nparam, int idx, int def)
{
    if (idx < nparam && params[idx] >= 0)
        return params[idx];
    return def;
}

/* ------------------------------------------------------------------ */
/*  SGR (Select Graphic Rendition)                                    */
/* ------------------------------------------------------------------ */

static void handle_sgr(struct vterm *vt, int *params, int nparam)
{
    if (nparam == 0) {
        /* SGR 0 - reset */
        vt->cur_fg = COLOR_DEFAULT;
        vt->cur_bg = COLOR_DEFAULT;
        vt->cur_attr = 0;
        return;
    }

    for (int i = 0; i < nparam; i++) {
        int p = (params[i] < 0) ? 0 : params[i];
        switch (p) {
        case 0:
            vt->cur_fg = COLOR_DEFAULT;
            vt->cur_bg = COLOR_DEFAULT;
            vt->cur_attr = 0;
            break;
        case 1: vt->cur_attr |= ATTR_BOLD; break;
        case 2: vt->cur_attr |= ATTR_DIM; break;
        case 3: vt->cur_attr |= ATTR_ITALIC; break;
        case 4: vt->cur_attr |= ATTR_UNDERLINE; break;
        case 5: vt->cur_attr |= ATTR_BLINK; break;
        case 7: vt->cur_attr |= ATTR_REVERSE; break;
        case 8: vt->cur_attr |= ATTR_INVISIBLE; break;
        case 9: vt->cur_attr |= ATTR_STRIKE; break;
        case 21: /* double underline = underline */ vt->cur_attr |= ATTR_UNDERLINE; break;
        case 22: vt->cur_attr &= ~(ATTR_BOLD | ATTR_DIM); break;
        case 23: vt->cur_attr &= ~ATTR_ITALIC; break;
        case 24: vt->cur_attr &= ~ATTR_UNDERLINE; break;
        case 25: vt->cur_attr &= ~ATTR_BLINK; break;
        case 27: vt->cur_attr &= ~ATTR_REVERSE; break;
        case 28: vt->cur_attr &= ~ATTR_INVISIBLE; break;
        case 29: vt->cur_attr &= ~ATTR_STRIKE; break;
        /* Foreground colors */
        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            vt->cur_fg = p - 30;
            break;
        case 38:
            /* Extended foreground: 38;5;N or 38;2;R;G;B */
            if (i + 1 < nparam && params[i + 1] == 5 && i + 2 < nparam) {
                vt->cur_fg = params[i + 2];
                i += 2;
            } else if (i + 1 < nparam && params[i + 1] == 2 && i + 4 < nparam) {
                int r = params[i + 2], g = params[i + 3], b = params[i + 4];
                vt->cur_fg = COLOR_TRUE | (r << 16) | (g << 8) | b;
                i += 4;
            }
            break;
        case 39: vt->cur_fg = COLOR_DEFAULT; break;
        /* Background colors */
        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            vt->cur_bg = p - 40;
            break;
        case 48:
            /* Extended background */
            if (i + 1 < nparam && params[i + 1] == 5 && i + 2 < nparam) {
                vt->cur_bg = params[i + 2];
                i += 2;
            } else if (i + 1 < nparam && params[i + 1] == 2 && i + 4 < nparam) {
                int r = params[i + 2], g = params[i + 3], b = params[i + 4];
                vt->cur_bg = COLOR_TRUE | (r << 16) | (g << 8) | b;
                i += 4;
            }
            break;
        case 49: vt->cur_bg = COLOR_DEFAULT; break;
        /* Bright foreground */
        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            vt->cur_fg = p - 90 + 8;
            break;
        /* Bright background */
        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            vt->cur_bg = p - 100 + 8;
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Alternate screen buffer                                           */
/* ------------------------------------------------------------------ */

static void switch_to_alt(struct vterm *vt)
{
    if (vt->alt_active) return;
    vt->alt_active = 1;

    /* Save main screen */
    struct cell *tmp = vt->cells;
    vt->cells = vt->alt_cells;
    vt->alt_cells = tmp;

    vt->alt_saved_row = vt->cursor_row;
    vt->alt_saved_col = vt->cursor_col;

    /* Clear alt screen */
    for (int i = 0; i < vt->rows * vt->cols; i++)
        clear_cell(&vt->cells[i]);

    vt->cursor_row = 0;
    vt->cursor_col = 0;
}

static void switch_to_main(struct vterm *vt)
{
    if (!vt->alt_active) return;
    vt->alt_active = 0;

    struct cell *tmp = vt->cells;
    vt->cells = vt->alt_cells;
    vt->alt_cells = tmp;

    vt->cursor_row = vt->alt_saved_row;
    vt->cursor_col = vt->alt_saved_col;
    clamp_cursor(vt);
}

/* ------------------------------------------------------------------ */
/*  CSI command handler                                               */
/* ------------------------------------------------------------------ */

static void handle_csi(struct vterm *vt)
{
    int params[MAX_CSI_PARAMS];
    int nparam = 0;
    int priv = 0;
    int cmd = parse_csi_params(vt->esc_buf, vt->esc_len, params, &nparam, &priv);

    int n, m;

    if (priv == '?') {
        /* DEC private modes — iterate all params (e.g. CSI?1006;1000h) */
        for (int pi = 0; pi < nparam; pi++) {
            int mode = param_or(params, nparam, pi, 0);
            if (cmd == 'h') {
                /* Set mode */
                switch (mode) {
                case 1:    vt->app_cursor = 1; break;
                case 7:    vt->auto_wrap = 1; break;
                case 25:   vt->cursor_visible = 1; break;
                case 47: case 1047:
                    switch_to_alt(vt);
                    break;
                case 1048:
                    vt->saved_row = vt->cursor_row;
                    vt->saved_col = vt->cursor_col;
                    break;
                case 1049:
                    vt->saved_row = vt->cursor_row;
                    vt->saved_col = vt->cursor_col;
                    switch_to_alt(vt);
                    break;
                case 2004:
                    vt->bracketed_paste = 1;
                    break;
                case 1000: case 1002: case 1003:
                    vt->mouse_mode = mode;
                    break;
                }
            } else if (cmd == 'l') {
                /* Reset mode */
                switch (mode) {
                case 1:    vt->app_cursor = 0; break;
                case 7:    vt->auto_wrap = 0; break;
                case 25:   vt->cursor_visible = 0; break;
                case 47: case 1047:
                    switch_to_main(vt);
                    break;
                case 1048:
                    vt->cursor_row = vt->saved_row;
                    vt->cursor_col = vt->saved_col;
                    clamp_cursor(vt);
                    break;
                case 1049:
                    switch_to_main(vt);
                    vt->cursor_row = vt->saved_row;
                    vt->cursor_col = vt->saved_col;
                    clamp_cursor(vt);
                    break;
                case 2004:
                    vt->bracketed_paste = 0;
                    break;
                case 1000: case 1002: case 1003:
                    vt->mouse_mode = 0;
                    break;
                }
            }
        }
        return;
    }

    if (priv == '>') {
        /* DA2 - Secondary Device Attributes */
        if (cmd == 'c') {
            /* Report as xterm version 300 — tells vim to use ttymouse=sgr */
            const char *resp = "\033[>65;300;1c";
            vterm_respond(vt, resp, (int)strlen(resp));
        }
        return;
    }

    if (priv == '!') {
        /* Ignore */
        return;
    }

    switch (cmd) {
    case 'A': /* CUU - Cursor Up */
        n = param_or(params, nparam, 0, 1);
        vt->cursor_row -= n;
        if (vt->cursor_row < 0) vt->cursor_row = 0;
        vt->wrap_pending = 0;
        break;

    case 'B': /* CUD - Cursor Down */
        n = param_or(params, nparam, 0, 1);
        vt->cursor_row += n;
        if (vt->cursor_row >= vt->rows) vt->cursor_row = vt->rows - 1;
        vt->wrap_pending = 0;
        break;

    case 'C': /* CUF - Cursor Forward */
        n = param_or(params, nparam, 0, 1);
        vt->cursor_col += n;
        if (vt->cursor_col >= vt->cols) vt->cursor_col = vt->cols - 1;
        vt->wrap_pending = 0;
        break;

    case 'D': /* CUB - Cursor Back */
        n = param_or(params, nparam, 0, 1);
        vt->cursor_col -= n;
        if (vt->cursor_col < 0) vt->cursor_col = 0;
        vt->wrap_pending = 0;
        break;

    case 'E': /* CNL - Cursor Next Line */
        n = param_or(params, nparam, 0, 1);
        vt->cursor_row += n;
        if (vt->cursor_row >= vt->rows) vt->cursor_row = vt->rows - 1;
        vt->cursor_col = 0;
        vt->wrap_pending = 0;
        break;

    case 'F': /* CPL - Cursor Previous Line */
        n = param_or(params, nparam, 0, 1);
        vt->cursor_row -= n;
        if (vt->cursor_row < 0) vt->cursor_row = 0;
        vt->cursor_col = 0;
        vt->wrap_pending = 0;
        break;

    case 'G': /* CHA - Cursor Horizontal Absolute */
        n = param_or(params, nparam, 0, 1);
        vt->cursor_col = n - 1;
        clamp_cursor(vt);
        vt->wrap_pending = 0;
        break;

    case 'H': case 'f': /* CUP / HVP - Cursor Position */
        n = param_or(params, nparam, 0, 1);
        m = param_or(params, nparam, 1, 1);
        vt->cursor_row = n - 1;
        vt->cursor_col = m - 1;
        clamp_cursor(vt);
        vt->wrap_pending = 0;
        break;

    case 'J': /* ED - Erase in Display */
        n = param_or(params, nparam, 0, 0);
        switch (n) {
        case 0: /* Erase below */
            clear_region(vt, vt->cursor_row, vt->cursor_col,
                         vt->rows - 1, vt->cols - 1);
            break;
        case 1: /* Erase above */
            clear_region(vt, 0, 0, vt->cursor_row, vt->cursor_col);
            break;
        case 2: case 3: /* Erase all */
            clear_region(vt, 0, 0, vt->rows - 1, vt->cols - 1);
            break;
        }
        break;

    case 'K': /* EL - Erase in Line */
        n = param_or(params, nparam, 0, 0);
        switch (n) {
        case 0: /* Erase to right */
            for (int c = vt->cursor_col; c < vt->cols; c++)
                clear_cell_attr(grid_cell(vt, vt->cursor_row, c),
                                vt->cur_fg, vt->cur_bg, 0);
            break;
        case 1: /* Erase to left */
            for (int c = 0; c <= vt->cursor_col; c++)
                clear_cell_attr(grid_cell(vt, vt->cursor_row, c),
                                vt->cur_fg, vt->cur_bg, 0);
            break;
        case 2: /* Erase whole line */
            for (int c = 0; c < vt->cols; c++)
                clear_cell_attr(grid_cell(vt, vt->cursor_row, c),
                                vt->cur_fg, vt->cur_bg, 0);
            break;
        }
        break;

    case 'L': /* IL - Insert Lines */
        n = param_or(params, nparam, 0, 1);
        if (vt->cursor_row >= vt->scroll_top && vt->cursor_row <= vt->scroll_bottom) {
            int saved_top = vt->scroll_top;
            vt->scroll_top = vt->cursor_row;
            scroll_down(vt, n);
            vt->scroll_top = saved_top;
        }
        break;

    case 'M': /* DL - Delete Lines */
        n = param_or(params, nparam, 0, 1);
        if (vt->cursor_row >= vt->scroll_top && vt->cursor_row <= vt->scroll_bottom) {
            int saved_top = vt->scroll_top;
            vt->scroll_top = vt->cursor_row;
            scroll_up(vt, n);
            vt->scroll_top = saved_top;
        }
        break;

    case 'P': /* DCH - Delete Characters */
        n = param_or(params, nparam, 0, 1);
        {
            int row = vt->cursor_row;
            int col = vt->cursor_col;
            if (n > vt->cols - col) n = vt->cols - col;
            memmove(&vt->cells[row * vt->cols + col],
                    &vt->cells[row * vt->cols + col + n],
                    (size_t)(vt->cols - col - n) * sizeof(struct cell));
            for (int c = vt->cols - n; c < vt->cols; c++)
                clear_cell(grid_cell(vt, row, c));
        }
        break;

    case 'S': /* SU - Scroll Up */
        n = param_or(params, nparam, 0, 1);
        scroll_up(vt, n);
        break;

    case 'T': /* SD - Scroll Down */
        n = param_or(params, nparam, 0, 1);
        scroll_down(vt, n);
        break;

    case 'X': /* ECH - Erase Characters */
        n = param_or(params, nparam, 0, 1);
        for (int i = 0; i < n && vt->cursor_col + i < vt->cols; i++)
            clear_cell_attr(grid_cell(vt, vt->cursor_row, vt->cursor_col + i),
                            vt->cur_fg, vt->cur_bg, 0);
        break;

    case '@': /* ICH - Insert Characters */
        n = param_or(params, nparam, 0, 1);
        {
            int row = vt->cursor_row;
            int col = vt->cursor_col;
            if (n > vt->cols - col) n = vt->cols - col;
            memmove(&vt->cells[row * vt->cols + col + n],
                    &vt->cells[row * vt->cols + col],
                    (size_t)(vt->cols - col - n) * sizeof(struct cell));
            for (int i = 0; i < n; i++)
                clear_cell(grid_cell(vt, row, col + i));
        }
        break;

    case 'd': /* VPA - Vertical Position Absolute */
        n = param_or(params, nparam, 0, 1);
        vt->cursor_row = n - 1;
        clamp_cursor(vt);
        vt->wrap_pending = 0;
        break;

    case 'h': /* SM - Set Mode */
        n = param_or(params, nparam, 0, 0);
        if (n == 4) vt->insert_mode = 1;
        break;

    case 'l': /* RM - Reset Mode */
        n = param_or(params, nparam, 0, 0);
        if (n == 4) vt->insert_mode = 0;
        break;

    case 'm': /* SGR - Select Graphic Rendition */
        handle_sgr(vt, params, nparam);
        break;

    case 'n': /* DSR - Device Status Report */
        n = param_or(params, nparam, 0, 0);
        if (n == 6) {
            /* CPR - Cursor Position Report */
            char cpr[32];
            int clen = snprintf(cpr, sizeof(cpr), "\033[%d;%dR",
                                vt->cursor_row + 1, vt->cursor_col + 1);
            vterm_respond(vt, cpr, clen);
        } else if (n == 5) {
            /* Status report: terminal OK */
            vterm_respond(vt, "\033[0n", 4);
        }
        break;

    case 'r': /* DECSTBM - Set Scrolling Region */
        n = param_or(params, nparam, 0, 1);
        m = param_or(params, nparam, 1, vt->rows);
        if (n < 1) n = 1;
        if (m > vt->rows) m = vt->rows;
        if (n < m) {
            vt->scroll_top = n - 1;
            vt->scroll_bottom = m - 1;
            vt->cursor_row = vt->origin_mode ? vt->scroll_top : 0;
            vt->cursor_col = 0;
        }
        break;

    case 's': /* Save cursor */
        vt->saved_row = vt->cursor_row;
        vt->saved_col = vt->cursor_col;
        break;

    case 'u': /* Restore cursor */
        vt->cursor_row = vt->saved_row;
        vt->cursor_col = vt->saved_col;
        clamp_cursor(vt);
        break;

    case 'c': /* DA1 - Primary Device Attributes */
        {
            /* Report VT220 with ANSI color, national replacement charsets */
            const char *da1 = "\033[?62;22c";
            vterm_respond(vt, da1, (int)strlen(da1));
        }
        break;

    case 'g': /* TBC - Tabulation Clear */
        n = param_or(params, nparam, 0, 0);
        if (n == 0 && vt->cursor_col < vt->cols)
            vt->tabstops[vt->cursor_col] = 0;
        else if (n == 3)
            memset(vt->tabstops, 0, (size_t)vt->cols);
        break;

    case 't': /* Window manipulation - ignore */
        break;

    case 'q': /* DECSCUSR - Set cursor style - ignore */
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  ESC sequence handler                                              */
/* ------------------------------------------------------------------ */

static void handle_esc(struct vterm *vt, char c)
{
    switch (c) {
    case '[': /* CSI */
        vt->state = VT_CSI;
        vt->esc_len = 0;
        return;
    case ']': /* OSC */
        vt->state = VT_OSC;
        vt->esc_len = 0;
        return;
    case 'P': /* DCS */
        vt->state = VT_DCS;
        vt->esc_len = 0;
        return;
    case '(': case ')': case '*': case '+':
        vt->state = VT_CHARSET;
        return;
    case '7': /* DECSC - Save cursor */
        vt->saved_row = vt->cursor_row;
        vt->saved_col = vt->cursor_col;
        break;
    case '8': /* DECRC - Restore cursor */
        vt->cursor_row = vt->saved_row;
        vt->cursor_col = vt->saved_col;
        clamp_cursor(vt);
        break;
    case 'D': /* IND - Index (move down, scroll if needed) */
        if (vt->cursor_row == vt->scroll_bottom)
            scroll_up(vt, 1);
        else if (vt->cursor_row < vt->rows - 1)
            vt->cursor_row++;
        break;
    case 'M': /* RI - Reverse Index (move up, scroll if needed) */
        if (vt->cursor_row == vt->scroll_top)
            scroll_down(vt, 1);
        else if (vt->cursor_row > 0)
            vt->cursor_row--;
        break;
    case 'E': /* NEL - Next Line */
        vt->cursor_col = 0;
        if (vt->cursor_row == vt->scroll_bottom)
            scroll_up(vt, 1);
        else if (vt->cursor_row < vt->rows - 1)
            vt->cursor_row++;
        break;
    case 'c': /* RIS - Full Reset */
        {
            int rows = vt->rows, cols = vt->cols;
            struct cell *cells = vt->cells;
            struct cell *alt = vt->alt_cells;
            uint8_t *tabs = vt->tabstops;
            vterm_init(vt, rows, cols);
            /* vterm_init allocates new buffers, free the old reservation */
            /* Actually vterm_init already allocated, so just reassign */
            /* We need to free what vterm_init allocated and use our existing */
            /* Simpler: just reinit fully */
            (void)cells; (void)alt; (void)tabs;
        }
        break;
    case 'H': /* HTS - Horizontal Tab Set */
        if (vt->cursor_col < vt->cols)
            vt->tabstops[vt->cursor_col] = 1;
        break;
    case '=': /* DECKPAM - Keypad Application Mode */
    case '>': /* DECKPNM - Keypad Numeric Mode */
        /* Ignore */
        break;
    default:
        break;
    }
    vt->state = VT_NORMAL;
}

/* ------------------------------------------------------------------ */
/*  Put character on grid                                             */
/* ------------------------------------------------------------------ */

static void vterm_putc(struct vterm *vt, uint32_t ch)
{
    int width = 1;

    /* Determine character display width */
    if (ch > 0x7F) {
        int w = wcwidth((wchar_t)ch);
        if (w > 0) width = w;
    }

    /* Handle pending wrap */
    if (vt->wrap_pending) {
        vt->wrap_pending = 0;
        vt->cursor_col = 0;
        if (vt->cursor_row == vt->scroll_bottom)
            scroll_up(vt, 1);
        else if (vt->cursor_row < vt->rows - 1)
            vt->cursor_row++;
    }

    /* Insert mode: shift characters right */
    if (vt->insert_mode) {
        int row = vt->cursor_row;
        int col = vt->cursor_col;
        if (col + width < vt->cols) {
            memmove(&vt->cells[row * vt->cols + col + width],
                    &vt->cells[row * vt->cols + col],
                    (size_t)(vt->cols - col - width) * sizeof(struct cell));
        }
    }

    /* Place the character */
    struct cell *c = grid_cell(vt, vt->cursor_row, vt->cursor_col);
    if (c) {
        c->ch    = ch;
        c->fg    = vt->cur_fg;
        c->bg    = vt->cur_bg;
        c->attr  = vt->cur_attr;
        c->width = (uint8_t)width;
    }

    /* For wide characters, mark the next cell as continuation */
    if (width == 2 && vt->cursor_col + 1 < vt->cols) {
        struct cell *c2 = grid_cell(vt, vt->cursor_row, vt->cursor_col + 1);
        if (c2) {
            c2->ch    = 0;
            c2->fg    = vt->cur_fg;
            c2->bg    = vt->cur_bg;
            c2->attr  = vt->cur_attr;
            c2->width = 0;
        }
    }

    /* Advance cursor */
    vt->cursor_col += width;

    /* Check if we need to wrap */
    if (vt->cursor_col >= vt->cols) {
        if (vt->auto_wrap) {
            vt->cursor_col = vt->cols - 1;
            vt->wrap_pending = 1;
        } else {
            vt->cursor_col = vt->cols - 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  OSC handler                                                       */
/* ------------------------------------------------------------------ */

static void handle_osc(struct vterm *vt)
{
    if (vt->esc_len < 2) return;

    /* OSC 0;text — set icon name + window title */
    /* OSC 2;text — set window title */
    if ((vt->esc_buf[0] == '0' || vt->esc_buf[0] == '2') &&
        vt->esc_buf[1] == ';') {
        int len = vt->esc_len - 2;
        if (len > (int)sizeof(vt->title) - 1)
            len = (int)sizeof(vt->title) - 1;
        memcpy(vt->title, &vt->esc_buf[2], (size_t)len);
        vt->title[len] = '\0';
        vt->title_changed = 1;
    }
}

/* ------------------------------------------------------------------ */
/*  Process input data                                                */
/* ------------------------------------------------------------------ */

void vterm_process(struct vterm *vt, const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];

        /* UTF-8 multi-byte handling */
        if (vt->utf8_remaining > 0) {
            if ((ch & 0xC0) == 0x80) {
                vt->utf8_cp = (vt->utf8_cp << 6) | (ch & 0x3F);
                vt->utf8_remaining--;
                if (vt->utf8_remaining == 0 && vt->state == VT_NORMAL) {
                    vterm_putc(vt, vt->utf8_cp);
                }
                continue;
            } else {
                /* Invalid continuation, reset and reprocess */
                vt->utf8_remaining = 0;
            }
        }

        switch (vt->state) {
        case VT_NORMAL:
            if (ch == 0x1B) { /* ESC */
                vt->state = VT_ESC;
            } else if (ch == '\r') {
                vt->cursor_col = 0;
                vt->wrap_pending = 0;
            } else if (ch == '\n' || ch == '\x0B' || ch == '\x0C') {
                /* LF, VT, FF */
                if (vt->cursor_row == vt->scroll_bottom)
                    scroll_up(vt, 1);
                else if (vt->cursor_row < vt->rows - 1)
                    vt->cursor_row++;
                vt->wrap_pending = 0;
            } else if (ch == '\b') {
                if (vt->cursor_col > 0)
                    vt->cursor_col--;
                vt->wrap_pending = 0;
            } else if (ch == '\t') {
                /* Tab: advance to next tab stop */
                int col = vt->cursor_col + 1;
                while (col < vt->cols && !vt->tabstops[col])
                    col++;
                if (col >= vt->cols) col = vt->cols - 1;
                vt->cursor_col = col;
                vt->wrap_pending = 0;
            } else if (ch == '\a') {
                /* Bell - ignore */
            } else if (ch == 0x0E || ch == 0x0F) {
                /* SO/SI - charset switch, ignore */
            } else if (ch >= 0x20 && ch < 0x7F) {
                /* Printable ASCII */
                vterm_putc(vt, ch);
            } else if (ch >= 0xC0) {
                /* Start of UTF-8 multi-byte sequence */
                if ((ch & 0xE0) == 0xC0) {
                    vt->utf8_cp = ch & 0x1F;
                    vt->utf8_remaining = 1;
                } else if ((ch & 0xF0) == 0xE0) {
                    vt->utf8_cp = ch & 0x0F;
                    vt->utf8_remaining = 2;
                } else if ((ch & 0xF8) == 0xF0) {
                    vt->utf8_cp = ch & 0x07;
                    vt->utf8_remaining = 3;
                }
            }
            break;

        case VT_ESC:
            handle_esc(vt, (char)ch);
            break;

        case VT_CSI:
            if (vt->esc_len < MAX_ESC_BUF - 1) {
                vt->esc_buf[vt->esc_len++] = (char)ch;
            }
            /* CSI sequences end with a letter (0x40-0x7E) */
            if (ch >= 0x40 && ch <= 0x7E) {
                handle_csi(vt);
                vt->state = VT_NORMAL;
            }
            break;

        case VT_OSC:
            /* OSC terminated by BEL (0x07) or ST (ESC \) */
            if (ch == 0x07) {
                handle_osc(vt);
                vt->state = VT_NORMAL;
            } else if (ch == 0x1B) {
                /* Might be ST (ESC \), peek at next */
                if (i + 1 < len && data[i + 1] == '\\') {
                    handle_osc(vt);
                    i++;
                }
                vt->state = VT_NORMAL;
            } else {
                /* Accumulate OSC content */
                if (vt->esc_len < MAX_ESC_BUF - 1)
                    vt->esc_buf[vt->esc_len++] = (char)ch;
            }
            break;

        case VT_DCS:
            /* DCS terminated by ST (ESC \) */
            if (ch == 0x1B) {
                vt->state = VT_NORMAL;
                if (i + 1 < len && data[i + 1] == '\\')
                    i++;
            } else if (ch == 0x9C) {
                vt->state = VT_NORMAL;
            }
            break;

        case VT_CHARSET:
            /* Eat one char and go back to normal */
            vt->state = VT_NORMAL;
            break;
        }
    }
}
