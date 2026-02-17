/*
 * main.c - Ttabmux entry point and event loop
 *
 * Manages terminal sessions via PTYs, handles user input (including
 * the Ctrl+B prefix key), multiplexes I/O with poll(), and
 * coordinates rendering.  Supports split panes (vertical/horizontal).
 */

#define _XOPEN_SOURCE 600
#define _GNU_SOURCE

#include "ttabmux.h"
#include "debug.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#endif

/* Suppress unused return value warnings for write() */
#define IGNORE_RESULT(x) do { if (x) {} } while (0)

/* ------------------------------------------------------------------ */
/*  Globals for signal handling                                       */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_winch = 0;
static volatile sig_atomic_t g_child = 0;

static void handle_sigwinch(int sig)
{
    (void)sig;
    g_winch = 1;
}

static void handle_sigchld(int sig)
{
    (void)sig;
    g_child = 1;
}

/* ------------------------------------------------------------------ */
/*  Terminal raw mode                                                  */
/* ------------------------------------------------------------------ */

static int term_raw_mode(struct ttabmux *t)
{
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &t->orig_termios) < 0)
        return -1;

    raw = t->orig_termios;
    raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cflag |= (unsigned)(CS8);
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0)
        return -1;
    return 0;
}

static void term_restore(struct ttabmux *t)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t->orig_termios);
}

/* ------------------------------------------------------------------ */
/*  Get terminal size                                                 */
/* ------------------------------------------------------------------ */

static void get_term_size(struct ttabmux *t)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        t->term_rows = ws.ws_row;
        t->term_cols = ws.ws_col;
    } else {
        t->term_rows = 24;
        t->term_cols = 80;
    }
}

/* ------------------------------------------------------------------ */
/*  Layout tree helpers                                               */
/* ------------------------------------------------------------------ */

static int layout_alloc(struct session *s)
{
    for (int i = 0; i < MAX_LAYOUT_NODES; i++)
        if (!s->layout[i].used)
            return i;
    return -1;
}

static void layout_free(struct session *s, int idx)
{
    if (idx >= 0 && idx < MAX_LAYOUT_NODES)
        memset(&s->layout[idx], 0, sizeof(s->layout[idx]));
}

static int layout_make_leaf(struct session *s, int pane_idx)
{
    int idx = layout_alloc(s);
    if (idx < 0) return -1;
    struct layout_node *n = &s->layout[idx];
    n->used = 1;
    n->is_leaf = 1;
    n->pane_idx = pane_idx;
    n->parent = -1;
    n->child[0] = -1;
    n->child[1] = -1;
    n->split_ratio = 0.5f;
    return idx;
}

static void layout_reflow(struct session *s, int node_idx,
                           int x, int y, int w, int h)
{
    if (node_idx < 0 || !s->layout[node_idx].used) return;
    struct layout_node *n = &s->layout[node_idx];

    if (n->is_leaf) {
        if (n->pane_idx >= 0 && n->pane_idx < s->num_panes) {
            struct pane *p = &s->panes[n->pane_idx];
            p->x = x;
            p->y = y;
            p->w = (w > 0) ? w : 1;
            p->h = (h > 0) ? h : 1;
        }
        return;
    }

    if (n->split_type == SPLIT_VERT) {
        int avail = w - 1; /* 1 col for divider */
        if (avail < 2) avail = 2;
        int left_w = (int)(avail * n->split_ratio);
        if (left_w < 1) left_w = 1;
        int right_w = avail - left_w;
        if (right_w < 1) right_w = 1;
        layout_reflow(s, n->child[0], x, y, left_w, h);
        layout_reflow(s, n->child[1], x + left_w + 1, y, right_w, h);
    } else {
        int avail = h - 1; /* 1 row for divider */
        if (avail < 2) avail = 2;
        int top_h = (int)(avail * n->split_ratio);
        if (top_h < 1) top_h = 1;
        int bot_h = avail - top_h;
        if (bot_h < 1) bot_h = 1;
        layout_reflow(s, n->child[0], x, y, w, top_h);
        layout_reflow(s, n->child[1], x, y + top_h + 1, w, bot_h);
    }
}

static int find_leaf_for_pane(struct session *s, int pane_idx)
{
    for (int i = 0; i < MAX_LAYOUT_NODES; i++)
        if (s->layout[i].used && s->layout[i].is_leaf &&
            s->layout[i].pane_idx == pane_idx)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Session / PTY / Pane management                                   */
/* ------------------------------------------------------------------ */

int session_create(struct ttabmux *t, const char *shell)
{
    if (t->num_sessions >= MAX_SESSIONS) return -1;

    int idx = t->num_sessions;
    struct session *s = &t->sessions[idx];
    memset(s, 0, sizeof(*s));

    /* Determine terminal size for the PTY */
    int pty_rows = t->term_rows;
    int pty_cols = t->term_cols - t->sidebar_width;
    if (pty_cols < 10) pty_cols = 10;
    if (pty_rows < 2) pty_rows = 2;

    /* Wide mode: use wide_cols for PTY column count */
    int vt_cols = pty_cols;
    if (t->wide_cols > 0)
        vt_cols = t->wide_cols;

    struct winsize ws = {
        .ws_row = (unsigned short)pty_rows,
        .ws_col = (unsigned short)vt_cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    struct pane *p0 = &s->panes[0];
    memset(p0, 0, sizeof(*p0));

    pid_t pid = forkpty(&p0->pty_fd, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        return -1;
    }

    if (pid == 0) {
        /* Child process */
        signal(SIGWINCH, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        setenv("TERM", "xterm-256color", 1);
        unsetenv("TMUX");

        const char *sh = shell;
        if (!sh) sh = getenv("SHELL");
        if (!sh) sh = "/bin/sh";

        if (strchr(sh, ' ')) {
            execl("/bin/sh", "sh", "-c", sh, (char *)NULL);
        } else {
            execlp(sh, sh, (char *)NULL);
        }
        perror("exec");
        _exit(1);
    }

    /* Parent */
    p0->pid = pid;
    p0->alive = 1;
    p0->x = 0;
    p0->y = 0;
    p0->w = pty_cols;
    p0->h = pty_rows;
    s->num_panes = 1;
    s->active_pane = 0;

    snprintf(s->name, sizeof(s->name), "bash");

    /* Build tab label: basename of program + arguments */
    {
        const char *cmd = shell ? shell : getenv("SHELL");
        if (cmd) {
            const char *space = strchr(cmd, ' ');
            const char *first_end = space ? space : cmd + strlen(cmd);
            const char *slash = NULL;
            for (const char *pp = cmd; pp < first_end; pp++)
                if (*pp == '/') slash = pp;
            const char *base = slash ? slash + 1 : cmd;
            if (space) {
                snprintf(s->name, sizeof(s->name), "%.*s%s",
                         (int)(first_end - base), base, space);
            } else {
                snprintf(s->name, sizeof(s->name), "%.*s",
                         (int)(first_end - base), base);
            }
        }
    }

    /* Set PTY non-blocking */
    int flags = fcntl(p0->pty_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(p0->pty_fd, F_SETFL, flags | O_NONBLOCK);

    /* Initialize virtual terminal */
    vterm_init(&p0->vt, pty_rows, vt_cols);

    /* Initialize layout: single leaf node at root */
    memset(s->layout, 0, sizeof(s->layout));
    s->root_node = 0;
    s->layout[0].used = 1;
    s->layout[0].is_leaf = 1;
    s->layout[0].pane_idx = 0;
    s->layout[0].parent = -1;
    s->layout[0].child[0] = -1;
    s->layout[0].child[1] = -1;
    s->layout[0].split_ratio = 0.5f;

    t->num_sessions++;
    t->active = idx;

    return idx;
}

void session_close(struct ttabmux *t, int idx)
{
    if (idx < 0 || idx >= t->num_sessions) return;

    struct session *s = &t->sessions[idx];

    /* Close all panes */
    for (int j = 0; j < s->num_panes; j++) {
        struct pane *p = &s->panes[j];
        if (p->pty_fd >= 0) {
            close(p->pty_fd);
            p->pty_fd = -1;
        }
        if (p->pid > 0 && p->alive) {
            kill(p->pid, SIGTERM);
            p->alive = 0;
        }
        vterm_free(&p->vt);
    }

    /* Remove session by shifting */
    for (int i = idx; i < t->num_sessions - 1; i++)
        t->sessions[i] = t->sessions[i + 1];
    t->num_sessions--;

    /* Adjust active index */
    if (t->num_sessions == 0) {
        t->active = 0;
    } else if (t->active >= t->num_sessions) {
        t->active = t->num_sessions - 1;
    } else if (t->active > idx) {
        t->active--;
    }
}

/* Resize all panes within a single session to their layout dimensions */
static void resize_session_panes(struct ttabmux *t, struct session *s)
{
    for (int j = 0; j < s->num_panes; j++) {
        struct pane *p = &s->panes[j];
        int pty_rows, pty_cols;

        if (s->num_panes == 1) {
            /* Single pane: account for gutter and scrollbar */
            int gw = pane_gutter_width(p);
            int sb = pane_has_scrollbar(p);
            pty_cols = p->w - gw - sb;
            pty_rows = p->h;
        } else {
            int sb = pane_has_scrollbar(p);
            pty_cols = p->w - sb;
            pty_rows = p->h;
        }

        /* Account for horizontal scrollbar in wide mode */
        if (t->wide_cols > 0 && t->wide_cols > pty_cols)
            pty_rows -= 1;

        if (pty_cols < 2) pty_cols = 2;
        if (pty_rows < 2) pty_rows = 2;

        /* Wide mode: keep PTY cols at wide_cols, only resize rows */
        if (t->wide_cols > 0)
            pty_cols = t->wide_cols;

        if (p->alive && p->pty_fd >= 0) {
            struct winsize ws = {
                .ws_row = (unsigned short)pty_rows,
                .ws_col = (unsigned short)pty_cols,
                .ws_xpixel = 0,
                .ws_ypixel = 0
            };
            ioctl(p->pty_fd, TIOCSWINSZ, &ws);
            kill(p->pid, SIGWINCH);
        }
        vterm_resize(&p->vt, pty_rows, pty_cols);
    }
}

static void resize_all_ptys(struct ttabmux *t)
{
    int content_w = t->term_cols - t->sidebar_width;
    int content_h = t->term_rows;
    if (content_w < 2) content_w = 2;
    if (content_h < 2) content_h = 2;

    for (int i = 0; i < t->num_sessions; i++) {
        struct session *s = &t->sessions[i];
        int h = content_h;
        /* Reserve 1 row for action bar (search/jump-to-line) on active session */
        if (t->action_mode && i == t->active)
            h -= 1;
        if (h < 2) h = 2;
        layout_reflow(s, s->root_node, 0, 0, content_w, h);
        resize_session_panes(t, s);
    }
}

/* ------------------------------------------------------------------ */
/*  Split pane operations                                             */
/* ------------------------------------------------------------------ */

static void split_pane(struct ttabmux *t, int split_type, const char *cmd)
{
    if (t->num_sessions == 0) return;
    struct session *s = &t->sessions[t->active];
    if (s->num_panes >= MAX_PANES) return;

    int old_pane_idx = s->active_pane;
    int old_leaf = find_leaf_for_pane(s, old_pane_idx);
    if (old_leaf < 0) return;

    struct pane *old_p = &s->panes[old_pane_idx];
    int new_pane_idx = s->num_panes;
    struct pane *np = &s->panes[new_pane_idx];
    memset(np, 0, sizeof(*np));

    /* Estimate initial pane size */
    int rows = old_p->h;
    int cols = old_p->w;
    if (split_type == SPLIT_VERT) {
        cols = (cols - 1) / 2;
    } else {
        rows = (rows - 1) / 2;
    }
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;

    /* Wide mode: use wide_cols for PTY column count */
    int vt_cols = cols;
    if (t->wide_cols > 0)
        vt_cols = t->wide_cols;

    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)vt_cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    pid_t pid = forkpty(&np->pty_fd, NULL, NULL, &ws);
    if (pid < 0) return;

    if (pid == 0) {
        signal(SIGWINCH, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        setenv("TERM", "xterm-256color", 1);
        unsetenv("TMUX");
        const char *sh = cmd ? cmd : t->default_shell;
        if (!sh) sh = getenv("SHELL");
        if (!sh) sh = "/bin/sh";
        if (strchr(sh, ' '))
            execl("/bin/sh", "sh", "-c", sh, (char *)NULL);
        else
            execlp(sh, sh, (char *)NULL);
        _exit(1);
    }

    np->pid = pid;
    np->alive = 1;
    int flags = fcntl(np->pty_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(np->pty_fd, F_SETFL, flags | O_NONBLOCK);
    vterm_init(&np->vt, rows, vt_cols);
    s->num_panes++;

    /* Create two new leaf nodes */
    int leaf_old = layout_make_leaf(s, old_pane_idx);
    int leaf_new = layout_make_leaf(s, new_pane_idx);
    if (leaf_old < 0 || leaf_new < 0) return;

    /* Convert old leaf into a split node */
    struct layout_node *split = &s->layout[old_leaf];
    int old_parent = split->parent;
    split->is_leaf = 0;
    split->split_type = split_type;
    split->split_ratio = 0.5f;
    split->child[0] = leaf_old;
    split->child[1] = leaf_new;
    split->pane_idx = -1;

    s->layout[leaf_old].parent = old_leaf;
    s->layout[leaf_new].parent = old_leaf;
    /* Preserve parent's child pointer to old_leaf (unchanged) */
    (void)old_parent;

    /* Focus new pane */
    s->active_pane = new_pane_idx;

    /* Reflow layout and resize all panes */
    int content_w = t->term_cols - t->sidebar_width;
    int content_h = t->term_rows;
    if (content_w < 2) content_w = 2;
    if (content_h < 2) content_h = 2;
    layout_reflow(s, s->root_node, 0, 0, content_w, content_h);
    resize_session_panes(t, s);
}

/* ------------------------------------------------------------------ */
/*  Close pane / remove from layout                                   */
/* ------------------------------------------------------------------ */

static void remove_pane_from_layout(struct session *s, int pane_idx)
{
    int leaf = find_leaf_for_pane(s, pane_idx);
    if (leaf < 0) return;

    int parent = s->layout[leaf].parent;
    if (parent < 0) {
        /* Root leaf — just free it */
        layout_free(s, leaf);
        return;
    }

    /* Find sibling */
    int sibling = (s->layout[parent].child[0] == leaf)
                  ? s->layout[parent].child[1]
                  : s->layout[parent].child[0];

    /* Replace parent node with sibling content */
    int grandparent = s->layout[parent].parent;
    struct layout_node saved = s->layout[sibling];
    s->layout[parent] = saved;
    s->layout[parent].parent = grandparent;

    /* Update children's parent pointers if sibling was a split */
    if (!saved.is_leaf) {
        if (s->layout[parent].child[0] >= 0)
            s->layout[s->layout[parent].child[0]].parent = parent;
        if (s->layout[parent].child[1] >= 0)
            s->layout[s->layout[parent].child[1]].parent = parent;
    }

    /* Free old leaf and sibling nodes */
    layout_free(s, leaf);
    layout_free(s, sibling);
}

static void compact_panes(struct session *s, int removed_idx)
{
    /* Shift panes after removed_idx */
    for (int i = removed_idx; i < s->num_panes - 1; i++)
        s->panes[i] = s->panes[i + 1];
    s->num_panes--;
    memset(&s->panes[s->num_panes], 0, sizeof(struct pane));

    /* Update layout pane_idx references */
    for (int i = 0; i < MAX_LAYOUT_NODES; i++) {
        if (s->layout[i].used && s->layout[i].is_leaf) {
            if (s->layout[i].pane_idx > removed_idx)
                s->layout[i].pane_idx--;
        }
    }

    /* Fix active_pane index */
    if (s->active_pane == removed_idx) {
        if (s->active_pane >= s->num_panes)
            s->active_pane = s->num_panes - 1;
        if (s->active_pane < 0)
            s->active_pane = 0;
    } else if (s->active_pane > removed_idx) {
        s->active_pane--;
    }
}

/* ------------------------------------------------------------------ */
/*  Pane navigation                                                   */
/* ------------------------------------------------------------------ */

static void navigate_pane(struct ttabmux *t, int dx, int dy)
{
    if (t->num_sessions == 0) return;
    struct session *s = &t->sessions[t->active];
    if (s->num_panes <= 1) return;

    struct pane *cur = &s->panes[s->active_pane];
    int cx = cur->x + cur->w / 2;
    int cy = cur->y + cur->h / 2;

    int best = -1;
    int best_dist = INT_MAX;

    for (int i = 0; i < s->num_panes; i++) {
        if (i == s->active_pane) continue;
        if (!s->panes[i].alive) continue;
        struct pane *p = &s->panes[i];
        int px = p->x + p->w / 2;
        int py = p->y + p->h / 2;

        /* Check direction */
        if (dx > 0 && px <= cx) continue;
        if (dx < 0 && px >= cx) continue;
        if (dy > 0 && py <= cy) continue;
        if (dy < 0 && py >= cy) continue;

        int dist = abs(px - cx) + abs(py - cy);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }

    if (best >= 0)
        s->active_pane = best;
}

static int find_pane_at(struct ttabmux *t, int screen_col, int screen_row)
{
    if (t->num_sessions == 0) return -1;
    struct session *s = &t->sessions[t->active];
    int cx = screen_col - t->sidebar_width;
    int cy = screen_row;

    for (int i = 0; i < s->num_panes; i++) {
        struct pane *p = &s->panes[i];
        if (p->alive && cx >= p->x && cx < p->x + p->w &&
            cy >= p->y && cy < p->y + p->h)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Split divider hit testing                                         */
/* ------------------------------------------------------------------ */

struct divider_hit {
    int node_idx;
    int x, y, w, h;  /* region of the split node in content coords */
};

static int hit_test_divider(struct session *s, int node_idx,
                            int x, int y, int w, int h,
                            int cx, int cy, struct divider_hit *hit)
{
    if (node_idx < 0 || !s->layout[node_idx].used) return 0;
    struct layout_node *n = &s->layout[node_idx];
    if (n->is_leaf) return 0;

    if (n->split_type == SPLIT_VERT) {
        int avail = w - 1;
        if (avail < 2) avail = 2;
        int left_w = (int)(avail * n->split_ratio);
        if (left_w < 1) left_w = 1;
        int right_w = avail - left_w;
        if (right_w < 1) right_w = 1;
        int div_x = x + left_w;

        if (cx == div_x && cy >= y && cy < y + h) {
            hit->node_idx = node_idx;
            hit->x = x; hit->y = y;
            hit->w = w; hit->h = h;
            return 1;
        }
        if (hit_test_divider(s, n->child[0], x, y, left_w, h, cx, cy, hit))
            return 1;
        return hit_test_divider(s, n->child[1], x + left_w + 1, y,
                                right_w, h, cx, cy, hit);
    } else {
        int avail = h - 1;
        if (avail < 2) avail = 2;
        int top_h = (int)(avail * n->split_ratio);
        if (top_h < 1) top_h = 1;
        int bot_h = avail - top_h;
        if (bot_h < 1) bot_h = 1;
        int div_y = y + top_h;

        if (cy == div_y && cx >= x && cx < x + w) {
            hit->node_idx = node_idx;
            hit->x = x; hit->y = y;
            hit->w = w; hit->h = h;
            return 1;
        }
        if (hit_test_divider(s, n->child[0], x, y, w, top_h, cx, cy, hit))
            return 1;
        return hit_test_divider(s, n->child[1], x, y + top_h + 1,
                                w, bot_h, cx, cy, hit);
    }
}

/* ------------------------------------------------------------------ */
/*  Dead pane cleanup                                                 */
/* ------------------------------------------------------------------ */

static void cleanup_dead_panes(struct ttabmux *t)
{
    for (int i = 0; i < t->num_sessions; i++) {
        struct session *s = &t->sessions[i];
        int changed = 0;
        for (int j = s->num_panes - 1; j >= 0; j--) {
            if (!s->panes[j].alive && s->num_panes > 1) {
                struct pane *p = &s->panes[j];
                if (p->pty_fd >= 0) {
                    close(p->pty_fd);
                    p->pty_fd = -1;
                }
                vterm_free(&p->vt);
                remove_pane_from_layout(s, j);
                compact_panes(s, j);
                changed = 1;
            }
        }
        if (changed) {
            int cw = t->term_cols - t->sidebar_width;
            int ch = t->term_rows;
            if (cw < 2) cw = 2;
            if (ch < 2) ch = 2;
            layout_reflow(s, s->root_node, 0, 0, cw, ch);
            resize_session_panes(t, s);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Reap dead children                                                */
/* ------------------------------------------------------------------ */

static void reap_children(struct ttabmux *t)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < t->num_sessions; i++) {
            struct session *s = &t->sessions[i];
            for (int j = 0; j < s->num_panes; j++) {
                if (s->panes[j].pid == pid) {
                    s->panes[j].alive = 0;
                    goto found;
                }
            }
        }
        found:;
    }
}

/* ------------------------------------------------------------------ */
/*  Action mode helpers                                               */
/* ------------------------------------------------------------------ */

/* action_mode values:
 *   0 = off
 *   1 = search input  (typing query, live search)
 *   2 = jump-to-line  (typing line number)
 *   3 = search nav    (query confirmed; n/N to navigate)
 */
static void action_mode_start(struct ttabmux *t, int mode)
{
    t->action_mode = mode;
    t->action_len = 0;
    memset(t->action_buf, 0, sizeof(t->action_buf));
    if (mode == 1)
        t->search_match_line = -1;
    resize_all_ptys(t);
}

static void action_mode_end(struct ttabmux *t)
{
    t->action_mode = 0;
    resize_all_ptys(t);
}

/* ------------------------------------------------------------------ */
/*  Handle prefix key commands                                        */
/* ------------------------------------------------------------------ */

static void handle_prefix_cmd(struct ttabmux *t, unsigned char ch)
{
    t->prefix_mode = 0;

    switch (ch) {
    case 'c': /* Create new terminal */
        session_create(t, t->default_shell);
        break;

    case 'n': /* Next terminal */
        if (t->num_sessions > 1) {
            t->active = (t->active + 1) % t->num_sessions;
        }
        break;

    case 'p': /* Previous terminal */
        if (t->num_sessions > 1) {
            t->active = (t->active - 1 + t->num_sessions) % t->num_sessions;
        }
        break;

    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9':
        {
            int idx = ch - '1';
            if (idx < t->num_sessions) {
                t->active = idx;
            }
        }
        break;

    case 'x': /* Close current pane or terminal */
        if (t->num_sessions > 0) {
            struct session *s = &t->sessions[t->active];
            if (s->num_panes > 1) {
                /* Close active pane */
                int pi = s->active_pane;
                struct pane *p = &s->panes[pi];
                if (p->pty_fd >= 0) {
                    close(p->pty_fd);
                    p->pty_fd = -1;
                }
                if (p->pid > 0) {
                    kill(p->pid, SIGTERM);
                }
                vterm_free(&p->vt);
                remove_pane_from_layout(s, pi);
                compact_panes(s, pi);
                /* Reflow */
                int cw = t->term_cols - t->sidebar_width;
                int ch = t->term_rows;
                if (cw < 2) cw = 2;
                if (ch < 2) ch = 2;
                layout_reflow(s, s->root_node, 0, 0, cw, ch);
                resize_session_panes(t, s);
            } else {
                /* Close session */
                session_close(t, t->active);
                if (t->num_sessions == 0) {
                    t->running = 0;
                }
            }
        }
        break;

    case 'v': /* Vertical split */
        split_pane(t, SPLIT_VERT, NULL);
        break;

    case 's': /* Horizontal split */
        split_pane(t, SPLIT_HORIZ, NULL);
        break;

    case 'o': /* Cycle pane focus */
        if (t->num_sessions > 0) {
            struct session *s = &t->sessions[t->active];
            if (s->num_panes > 1) {
                s->active_pane = (s->active_pane + 1) % s->num_panes;
            }
        }
        break;

    case 0x1B: /* ESC after prefix: start arrow key parsing */
        t->prefix_mode = 2;
        return;  /* Don't clear prefix_mode */

    case 'd': /* Detach (quit) */
        t->running = 0;
        break;

    case ',': /* Rename */
        t->rename_mode = 1;
        t->rename_len = 0;
        memset(t->rename_buf, 0, sizeof(t->rename_buf));
        break;

    case '?': /* Help */
        t->show_help = !t->show_help;
        break;

    case '/': /* Search */
        action_mode_start(t, 1);
        break;

    case 'g': /* Jump to line */
        action_mode_start(t, 2);
        break;

    case PREFIX_KEY: /* Send literal Ctrl+B to terminal */
        {
            struct pane *p = cur_pane(t);
            if (p && p->alive) {
                char c = PREFIX_KEY;
                IGNORE_RESULT(write(p->pty_fd, &c, 1));
            }
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Handle rename mode input                                          */
/* ------------------------------------------------------------------ */

static void handle_rename_input(struct ttabmux *t, unsigned char ch)
{
    if (ch == '\r' || ch == '\n') {
        /* Accept the rename */
        if (t->rename_len > 0 && t->active < t->num_sessions) {
            t->rename_buf[t->rename_len] = '\0';
            snprintf(t->sessions[t->active].name,
                     sizeof(t->sessions[t->active].name),
                     "%s", t->rename_buf);
            t->sessions[t->active].renamed = 1;
        }
        t->rename_mode = 0;
    } else if (ch == 0x1B) {
        /* Cancel */
        t->rename_mode = 0;
    } else if (ch == 0x7F || ch == '\b') {
        /* Backspace */
        if (t->rename_len > 0)
            t->rename_len--;
    } else if (ch >= 0x20 && ch < 0x7F) {
        /* Printable character */
        if (t->rename_len < (int)sizeof(t->rename_buf) - 1)
            t->rename_buf[t->rename_len++] = (char)ch;
    }
}

/* ------------------------------------------------------------------ */
/*  Search helper                                                     */
/* ------------------------------------------------------------------ */

/* Case-insensitive ASCII tolower */
static char ci_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Case-insensitive strstr */
static const char *ci_strstr(const char *haystack, const char *needle)
{
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && ci_lower(*h) == ci_lower(*n)) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

/*
 * Extract text from a terminal line into buf (null-terminated).
 * Returns the number of characters written.
 */
static int extract_line_text(struct vterm *vt, int abs_line, char *buf, int buf_sz)
{
    int total_lines = vt->sb_len + vt->rows;
    if (abs_line < 0 || abs_line >= total_lines) return 0;

    struct cell *line = NULL;
    if (abs_line < vt->sb_len) {
        int depth = vt->sb_len - abs_line;
        line = vterm_sb_line(vt, depth);
    } else {
        int screen_row = abs_line - vt->sb_len;
        if (screen_row >= 0 && screen_row < vt->rows)
            line = &vt->cells[screen_row * vt->cols];
    }
    if (!line) return 0;

    int pos = 0;
    for (int col = 0; col < vt->cols && pos < buf_sz - 1; col++) {
        uint32_t ch = line[col].ch;
        if (ch == 0) ch = ' ';
        if (ch < 0x80) {
            buf[pos++] = (char)ch;
        } else {
            buf[pos++] = '?';
        }
    }
    buf[pos] = '\0';
    return pos;
}

/*
 * Search for query in the vterm, starting at start_line in direction dir.
 * start_col: on the first line searched, skip columns before this (fwd)
 *            or only consider matches starting before this column (bwd).
 */
static int search_in_vterm(struct ttabmux *t, const char *query,
                           int start_line, int start_col, int dir)
{
    if (!query || !*query) return 0;
    struct pane *p = cur_pane(t);
    if (!p) return 0;

    struct vterm *vt = &p->vt;
    int total_lines = vt->sb_len + vt->rows;
    if (total_lines == 0) return 0;

    char linebuf[4096];

    for (int i = 0; i < total_lines; i++) {
        int line = start_line + i * dir;
        line = ((line % total_lines) + total_lines) % total_lines;

        int len = extract_line_text(vt, line, linebuf, (int)sizeof(linebuf));
        if (len == 0) continue;

        const char *match = NULL;
        if (dir > 0) {
            /* Forward: on the first line, search from start_col */
            int offset = (i == 0) ? start_col : 0;
            if (offset < 0) offset = 0;
            if (offset < len)
                match = ci_strstr(linebuf + offset, query);
        } else {
            /* Backward: find the last match on this line;
               on the first line, only before start_col */
            int max_col = (i == 0) ? start_col : len;
            const char *cursor = linebuf;
            while (1) {
                const char *found = ci_strstr(cursor, query);
                if (!found) break;
                if ((int)(found - linebuf) >= max_col) break;
                match = found;
                cursor = found + 1;
            }
        }

        if (match) {
            t->search_match_line = line;
            t->search_match_col = (int)(match - linebuf);
            t->search_match_len = (int)strlen(query);

            int target_offset = vt->sb_len - line + vt->rows / 2;
            if (target_offset > vt->sb_len)
                target_offset = vt->sb_len;
            if (target_offset < 0)
                target_offset = 0;
            vt->scroll_offset = target_offset;

            /* Auto-scroll horizontally to make the match visible */
            {
                struct session *ss = &t->sessions[t->active];
                int visible_w;
                if (ss->num_panes == 1) {
                    int gw = pane_gutter_width(p);
                    int sb = pane_has_scrollbar(p);
                    visible_w = p->w - gw - sb;
                } else {
                    int sb = pane_has_scrollbar(p);
                    visible_w = p->w - sb;
                }
                if (pane_needs_hscroll(p, visible_w)) {
                    int match_end = t->search_match_col + t->search_match_len;
                    int max_off = vt->cols - visible_w;
                    if (max_off < 0) max_off = 0;
                    if (t->search_match_col < vt->col_offset ||
                        match_end > vt->col_offset + visible_w) {
                        int new_off = t->search_match_col - visible_w / 4;
                        if (new_off < 0) new_off = 0;
                        if (new_off > max_off) new_off = max_off;
                        vt->col_offset = new_off;
                    }
                }
            }
            return 1;
        }
    }
    return 0;
}

/*
 * Count all occurrences of the current search query and determine the
 * 1-based index of the current match.  Populates search_match_total
 * and search_match_index.
 */
static void search_count_matches(struct ttabmux *t)
{
    t->search_match_total = 0;
    t->search_match_index = 0;

    if (t->action_len == 0) return;
    struct pane *p = cur_pane(t);
    if (!p) return;

    struct vterm *vt = &p->vt;
    int total_lines = vt->sb_len + vt->rows;
    char linebuf[4096];
    int count = 0;
    int cur_index = 0;

    for (int line = 0; line < total_lines; line++) {
        int len = extract_line_text(vt, line, linebuf, (int)sizeof(linebuf));
        if (len == 0) continue;

        const char *cursor = linebuf;
        while (1) {
            const char *found = ci_strstr(cursor, t->action_buf);
            if (!found) break;
            count++;
            if (t->search_match_line >= 0 &&
                line == t->search_match_line &&
                (int)(found - linebuf) == t->search_match_col) {
                cur_index = count;
            }
            cursor = found + 1;
        }
    }

    t->search_match_total = count;
    t->search_match_index = cur_index;
}

/* ------------------------------------------------------------------ */
/*  Handle action bar input (search / jump-to-line / search nav)      */
/* ------------------------------------------------------------------ */

static void handle_action_input(struct ttabmux *t, unsigned char ch)
{
    if (ch == 0x1B) {
        t->search_match_line = -1;
        action_mode_end(t);
        return;
    }

    if (t->action_mode == 1) {
        if (ch == 0x7F || ch == '\b') {
            if (t->action_len > 0) {
                t->action_len--;
                t->action_buf[t->action_len] = '\0';
                if (t->action_len > 0) {
                    struct pane *p = cur_pane(t);
                    if (p) {
                        struct vterm *vt = &p->vt;
                        int start = vt->sb_len - vt->scroll_offset;
                        if (start < 0) start = 0;
                        search_in_vterm(t, t->action_buf, start, 0, 1);
                    }
                    search_count_matches(t);
                } else {
                    t->search_match_line = -1;
                    t->search_match_total = 0;
                    t->search_match_index = 0;
                }
            }
        } else if (ch == '\r' || ch == '\n') {
            if (t->action_len > 0) {
                t->action_mode = 3;
            } else {
                action_mode_end(t);
            }
        } else if (ch >= 0x20 && ch < 0x7F) {
            if (t->action_len < (int)sizeof(t->action_buf) - 1) {
                t->action_buf[t->action_len++] = (char)ch;
                t->action_buf[t->action_len] = '\0';
                struct pane *p = cur_pane(t);
                if (p) {
                    struct vterm *vt = &p->vt;
                    int start = vt->sb_len - vt->scroll_offset;
                    if (start < 0) start = 0;
                    search_in_vterm(t, t->action_buf, start, 0, 1);
                }
                search_count_matches(t);
            }
        }
    } else if (t->action_mode == 2) {
        if (ch == 0x7F || ch == '\b') {
            if (t->action_len > 0) {
                t->action_len--;
                t->action_buf[t->action_len] = '\0';
            }
        } else if (ch == '\r' || ch == '\n') {
            if (t->action_len > 0) {
                struct pane *p = cur_pane(t);
                if (p) {
                    int line_number = atoi(t->action_buf);
                    struct vterm *vt = &p->vt;
                    int total_lines = vt->sb_len + vt->rows;
                    if (line_number < 1) line_number = 1;
                    if (line_number > total_lines) line_number = total_lines;
                    int offset = vt->sb_len - (line_number - 1);
                    if (offset < 0) offset = 0;
                    if (offset > vt->sb_len) offset = vt->sb_len;
                    vt->scroll_offset = offset;
                }
            }
            action_mode_end(t);
        } else if (ch >= '0' && ch <= '9') {
            if (t->action_len < (int)sizeof(t->action_buf) - 1) {
                t->action_buf[t->action_len++] = (char)ch;
                t->action_buf[t->action_len] = '\0';
            }
        }
    } else if (t->action_mode == 3) {
        if (ch == 'n') {
            if (t->action_len > 0) {
                int start = t->search_match_line >= 0
                    ? t->search_match_line : 0;
                int scol = t->search_match_line >= 0
                    ? t->search_match_col + 1 : 0;
                search_in_vterm(t, t->action_buf, start, scol, 1);
                search_count_matches(t);
            }
        } else if (ch == 'N') {
            if (t->action_len > 0) {
                struct pane *p = cur_pane(t);
                if (p) {
                    int start = t->search_match_line >= 0
                        ? t->search_match_line
                        : (p->vt.sb_len + p->vt.rows - 1);
                    int scol = t->search_match_line >= 0
                        ? t->search_match_col
                        : p->vt.cols;
                    search_in_vterm(t, t->action_buf, start, scol, -1);
                    search_count_matches(t);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Handle mouse click                                                */
/* ------------------------------------------------------------------ */

/* Compute line number gutter width for current active pane */
static int gutter_width(struct ttabmux *t)
{
    struct pane *p = cur_pane(t);
    if (!p) return 0;
    if (t->num_sessions > 0) {
        struct session *s = &t->sessions[t->active];
        if (s->num_panes > 1) return 0;
    }
    return pane_gutter_width(p);
}

/* ------------------------------------------------------------------ */
/*  Text selection helpers                                            */
/* ------------------------------------------------------------------ */

/*
 * Convert mouse position (0-indexed screen col/row) to absolute line
 * and cell column. Returns 1 if the position is in the terminal content
 * area, 0 otherwise.
 */
static int screen_to_cell(struct ttabmux *t, int mouse_col, int mouse_row,
                          int *abs_line, int *cell_col)
{
    struct pane *p = cur_pane(t);
    if (!p) return 0;
    struct session *s = &t->sessions[t->active];
    struct vterm *vt = &p->vt;

    int content_left, content_right, content_top, vis_rows;

    if (s->num_panes == 1) {
        int gw = pane_gutter_width(p);
        int sb = pane_has_scrollbar(p);
        content_left = t->sidebar_width + gw;
        content_right = t->term_cols - sb;
        content_top = 0;
        vis_rows = vt->rows;
    } else {
        int sb = pane_has_scrollbar(p);
        content_left = t->sidebar_width + p->x;
        content_right = content_left + p->w - sb;
        content_top = p->y;
        vis_rows = p->h;
    }

    if (mouse_col < content_left || mouse_col >= content_right) return 0;
    if (mouse_row < content_top || mouse_row >= content_top + vis_rows) return 0;

    *cell_col = mouse_col - content_left;
    if (*cell_col < 0) *cell_col = 0;
    if (*cell_col >= vt->cols) *cell_col = vt->cols - 1;

    int vis_row = mouse_row - content_top;
    int so = vt->scroll_offset;
    *abs_line = vt->sb_len - so + vis_row;
    int total_lines = vt->sb_len + vt->rows;
    if (*abs_line < 0) *abs_line = 0;
    if (*abs_line >= total_lines) *abs_line = total_lines - 1;

    return 1;
}

static int is_word_char(uint32_t ch)
{
    if (ch >= 'a' && ch <= 'z') return 1;
    if (ch >= 'A' && ch <= 'Z') return 1;
    if (ch >= '0' && ch <= '9') return 1;
    if (ch == '_') return 1;
    return 0;
}

static struct cell *get_abs_cell(struct vterm *vt, int abs_line, int col)
{
    if (col < 0 || col >= vt->cols) return NULL;
    int total = vt->sb_len + vt->rows;
    if (abs_line < 0 || abs_line >= total) return NULL;

    struct cell *line;
    if (abs_line < vt->sb_len) {
        int depth = vt->sb_len - abs_line;
        line = vterm_sb_line(vt, depth);
    } else {
        int screen_row = abs_line - vt->sb_len;
        if (screen_row < 0 || screen_row >= vt->rows) return NULL;
        line = &vt->cells[screen_row * vt->cols];
    }
    return line ? &line[col] : NULL;
}

static void expand_to_word(struct vterm *vt, int line, int col,
                           int *start_col, int *end_col)
{
    struct cell *c = get_abs_cell(vt, line, col);
    uint32_t ch = (c && c->ch) ? c->ch : ' ';

    if (!is_word_char(ch)) {
        *start_col = col;
        *end_col = col;
        return;
    }

    *start_col = col;
    while (*start_col > 0) {
        struct cell *prev = get_abs_cell(vt, line, *start_col - 1);
        uint32_t pch = (prev && prev->ch) ? prev->ch : ' ';
        if (!is_word_char(pch)) break;
        (*start_col)--;
    }

    *end_col = col;
    while (*end_col < vt->cols - 1) {
        struct cell *next = get_abs_cell(vt, line, *end_col + 1);
        uint32_t nch = (next && next->ch) ? next->ch : ' ';
        if (!is_word_char(nch)) break;
        (*end_col)++;
    }
}

static void expand_to_line(struct vterm *vt, int line,
                           int *start_col, int *end_col)
{
    *start_col = 0;
    *end_col = vt->cols - 1;
    while (*end_col > 0) {
        struct cell *c = get_abs_cell(vt, line, *end_col);
        uint32_t ch = (c && c->ch) ? c->ch : 0;
        if (ch != 0 && ch != ' ') break;
        (*end_col)--;
    }
}

/* Encode UTF-8 codepoint into buf, return number of bytes */
static int sel_encode_utf8(uint32_t cp, char *buf)
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

/*
 * Extract selected text into buf (null-terminated). Returns length.
 */
static int extract_selection_text(struct ttabmux *t, char *buf, int buf_size)
{
    struct pane *p = cur_pane(t);
    if (!p) return 0;
    struct vterm *vt = &p->vt;

    /* Normalize: ensure start <= end */
    int sl = t->sel_start_line, sc = t->sel_start_col;
    int el = t->sel_end_line, ec = t->sel_end_col;
    if (sl > el || (sl == el && sc > ec)) {
        int tmp;
        tmp = sl; sl = el; el = tmp;
        tmp = sc; sc = ec; ec = tmp;
    }

    int pos = 0;
    for (int line = sl; line <= el && pos < buf_size - 2; line++) {
        int col_start = (line == sl) ? sc : 0;
        int col_end   = (line == el) ? ec : vt->cols - 1;

        int last_nonspace = col_start - 1;
        for (int col = col_start; col <= col_end && col < vt->cols; col++) {
            struct cell *c = get_abs_cell(vt, line, col);
            uint32_t ch = (c && c->ch) ? c->ch : 0;
            if (ch != 0 && ch != ' ')
                last_nonspace = col;
        }

        for (int col = col_start; col <= last_nonspace && pos < buf_size - 2; col++) {
            struct cell *c = get_abs_cell(vt, line, col);
            if (!c || c->width == 0) continue;
            uint32_t ch = c->ch;
            if (ch == 0) ch = ' ';
            char utf8[4];
            int n = sel_encode_utf8(ch, utf8);
            if (pos + n >= buf_size - 1) break;
            memcpy(buf + pos, utf8, (size_t)n);
            pos += n;
        }

        if (line < el && pos < buf_size - 2)
            buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    return pos;
}

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const char *in, int in_len, char *out, int out_size)
{
    int i = 0, o = 0;
    while (i < in_len && o + 4 < out_size) {
        unsigned int b0 = (unsigned char)in[i++];
        unsigned int b1 = (i < in_len) ? (unsigned char)in[i++] : 0;
        unsigned int b2 = (i < in_len) ? (unsigned char)in[i++] : 0;
        int have = (i <= in_len + 2) ? 3 : 0;
        (void)have;

        unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
        out[o++] = b64_table[(triple >> 18) & 0x3F];
        out[o++] = b64_table[(triple >> 12) & 0x3F];
        out[o++] = b64_table[(triple >> 6) & 0x3F];
        out[o++] = b64_table[triple & 0x3F];
    }
    int pad = (3 - (in_len % 3)) % 3;
    for (int pp = 0; pp < pad; pp++)
        out[o - 1 - pp] = '=';
    out[o] = '\0';
    return o;
}

static void copy_to_clipboard(struct ttabmux *t)
{
    char text[16384];
    int tlen = extract_selection_text(t, text, (int)sizeof(text));
    if (tlen <= 0) return;

    char b64[32768];
    int b64len = base64_encode(text, tlen, b64, (int)sizeof(b64));
    if (b64len <= 0) return;

    char osc[32800];
    int olen = snprintf(osc, sizeof(osc), "\033]52;c;%s\a", b64);
    if (olen > 0)
        IGNORE_RESULT(write(STDOUT_FILENO, osc, (size_t)olen));
}

static void clear_selection(struct ttabmux *t)
{
    t->sel_active = 0;
    t->sel_dragging = 0;
}

static void handle_mouse(struct ttabmux *t, int button, int col, int row,
                         int is_press)
{
    /* Convert from 1-indexed to 0-indexed */
    col -= 1;
    row -= 1;

#ifdef TTABMUX_DEBUG
    /* Debug: log mouse event with pane info */
    {
        int si = -1, pi = -1, px = 0, py = 0, pw = 0, ph = 0;
        int vr = 0, vc = 0, mm = 0;
        if (t->num_sessions > 0) {
            si = t->active;
            struct session *s = &t->sessions[si];
            pi = s->active_pane;
            struct pane *p = &s->panes[pi];
            px = p->x; py = p->y; pw = p->w; ph = p->h;
            vr = p->vt.rows; vc = p->vt.cols; mm = p->vt.mouse_mode;
        }
        debug_mouse(button, col, row, is_press, si, pi,
                     px, py, pw, ph, vr, vc, mm);
    }
#endif

    /* Mouse release — end any drag in progress */
    if (!is_press) {
        if (t->sel_dragging) {
            t->sel_dragging = 0;
            if (t->sel_start_line != t->sel_end_line ||
                t->sel_start_col != t->sel_end_col) {
                copy_to_clipboard(t);
            }
        }
        int was_drag = t->sidebar_drag || t->scrollbar_drag ||
                       t->sidebar_sb_drag || t->split_drag ||
                       t->hscrollbar_drag;
        t->sidebar_drag = 0;
        t->scrollbar_drag = 0;
        t->sidebar_sb_drag = 0;
        t->split_drag = 0;
        t->hscrollbar_drag = 0;
        /* Forward release to child app if it has mouse tracking */
        if (!was_drag && t->num_sessions > 0) {
            struct pane *p = cur_pane(t);
            if (p && p->alive && p->vt.mouse_mode && p->vt.scroll_offset == 0) {
                struct session *s = &t->sessions[t->active];
                int adj_col, adj_row;
                if (s->num_panes == 1) {
                    int gw = gutter_width(t);
                    adj_col = col - t->sidebar_width - gw + 1;
                    adj_row = row + 1;
                } else {
                    adj_col = col - (t->sidebar_width + p->x) + 1;
                    adj_row = row - p->y + 1;
                }
                if (adj_col < 1) adj_col = 1;
                if (adj_row < 1) adj_row = 1;
                debug_mouse_forward(t->active, s->active_pane,
                                     button, adj_col, adj_row, 0);
                char mouse_seq[64];
                int n = snprintf(mouse_seq, sizeof(mouse_seq),
                                 "\033[<%d;%d;%dm", button, adj_col, adj_row);
                IGNORE_RESULT(write(p->pty_fd, mouse_seq, (size_t)n));
            }
        }
        return;
    }

    /* Hover tracking: button 35 = motion with no button (SGR encoding) */
    if (button == 35) {
        /* Cancel rename mode on any mouse movement */
        if (t->rename_mode)
            t->rename_mode = 0;

        int on_border = (col >= t->sidebar_width - 1 && col <= t->sidebar_width);
        if (on_border != t->sidebar_hover) {
            t->sidebar_hover = on_border;
        }
        int newbtn_hover = 0;
        if (row == 0 && col < t->sidebar_width - 1 &&
            t->num_sessions < MAX_SESSIONS) {
            int nblen = 3;
            int btn_start = t->sidebar_width - 1 - nblen;
            if (col >= btn_start)
                newbtn_hover = 1;
        }
        if (newbtn_hover != t->sidebar_newbtn_hover)
            t->sidebar_newbtn_hover = newbtn_hover;
        int btn_hover = 0;
        if (row == t->term_rows - 2 && col < t->sidebar_width - 1) {
            int btn_w = (t->sidebar_width - 1) / 4;
            int region = col / btn_w;
            if (region > 3) region = 3;
            btn_hover = 3 + region;  /* 3=split-v, 4=split-h, 5=search, 6=close */
        } else if (row == t->term_rows - 1 && col < t->sidebar_width - 1) {
            int btn_w = (t->sidebar_width - 1) / 2;
            if (col < btn_w)
                btn_hover = 1;
            else
                btn_hover = 2;
        }
        if (btn_hover != t->sidebar_btn_hover)
            t->sidebar_btn_hover = btn_hover;
        /* Check if hovering over a split divider */
        {
            int new_hover = -1;
            if (t->num_sessions > 0) {
                struct session *s = &t->sessions[t->active];
                if (s->num_panes > 1) {
                    int cx = col - t->sidebar_width;
                    int cy = row;
                    int cw = t->term_cols - t->sidebar_width;
                    int ch2 = t->term_rows;
                    struct divider_hit dh;
                    if (hit_test_divider(s, s->root_node, 0, 0,
                                         cw, ch2, cx, cy, &dh))
                        new_hover = dh.node_idx;
                }
            }
            t->split_hover_node = new_hover;
        }
        return;
    }

    /* Forward mouse motion/drag to child app if it has mouse tracking */
    if ((button == 32 || button == 35) && !t->sel_dragging &&
        !t->sidebar_drag && !t->scrollbar_drag &&
        !t->sidebar_sb_drag && !t->split_drag) {
        struct pane *p = cur_pane(t);
        int need_mode = (button == 35) ? 1003 : 1002;
        if (p && p->alive && p->vt.mouse_mode >= need_mode &&
            p->vt.scroll_offset == 0) {
            struct session *s = &t->sessions[t->active];
            int adj_col, adj_row;
            if (s->num_panes == 1) {
                int gw = gutter_width(t);
                adj_col = col - t->sidebar_width - gw + 1;
                adj_row = row + 1;
            } else {
                adj_col = col - (t->sidebar_width + p->x) + 1;
                adj_row = row - p->y + 1;
            }
            if (adj_col < 1) adj_col = 1;
            if (adj_row < 1) adj_row = 1;
            debug_mouse_forward(t->active, s->active_pane,
                                 button, adj_col, adj_row, 1);
            char mouse_seq[64];
            int n = snprintf(mouse_seq, sizeof(mouse_seq),
                             "\033[<%d;%d;%dM", button, adj_col, adj_row);
            IGNORE_RESULT(write(p->pty_fd, mouse_seq, (size_t)n));
            return;
        }
    }

    /* Text selection drag motion */
    if (button == 32 && t->sel_dragging) {
        int abs_line, cell_col;
        if (screen_to_cell(t, col, row, &abs_line, &cell_col)) {
            struct pane *p = cur_pane(t);
            if (!p) return;
            struct vterm *vt = &p->vt;
            if (t->sel_click_count == 2) {
                int ws, we;
                expand_to_word(vt, abs_line, cell_col, &ws, &we);
                if (abs_line > t->sel_start_line ||
                    (abs_line == t->sel_start_line && cell_col >= t->sel_start_col)) {
                    t->sel_end_line = abs_line;
                    t->sel_end_col = we;
                } else {
                    t->sel_end_line = abs_line;
                    t->sel_end_col = ws;
                }
            } else if (t->sel_click_count == 3) {
                int ls, le;
                expand_to_line(vt, abs_line, &ls, &le);
                if (abs_line >= t->sel_start_line) {
                    t->sel_end_line = abs_line;
                    t->sel_end_col = le;
                } else {
                    t->sel_end_line = abs_line;
                    t->sel_end_col = ls;
                }
            } else {
                t->sel_end_line = abs_line;
                t->sel_end_col = cell_col;
            }
        }
        return;
    }

    /* Split divider drag motion */
    if (button == 32 && t->split_drag) {
        if (t->num_sessions > 0) {
            struct session *s = &t->sessions[t->active];
            struct layout_node *n = &s->layout[t->split_drag_node];
            int cx = col - t->sidebar_width;
            int cy = row;
            float new_ratio;
            if (n->split_type == SPLIT_VERT) {
                int avail = t->split_drag_w - 1;
                if (avail < 2) avail = 2;
                new_ratio = (float)(cx - t->split_drag_x) / avail;
            } else {
                int avail = t->split_drag_h - 1;
                if (avail < 2) avail = 2;
                new_ratio = (float)(cy - t->split_drag_y) / avail;
            }
            if (new_ratio < 0.1f) new_ratio = 0.1f;
            if (new_ratio > 0.9f) new_ratio = 0.9f;
            if (new_ratio != n->split_ratio) {
                n->split_ratio = new_ratio;
                int cw = t->term_cols - t->sidebar_width;
                int ch2 = t->term_rows;
                if (cw < 2) cw = 2;
                if (ch2 < 2) ch2 = 2;
                layout_reflow(s, s->root_node, 0, 0, cw, ch2);
                resize_session_panes(t, s);
            }
        }
        return;
    }

    /* Drag motion: button 32 = motion with left button held (SGR encoding) */
    if (button == 32 && t->sidebar_drag) {
        int new_width = col + 1;
        if (new_width < 10) new_width = 10;
        if (new_width > t->term_cols - 20) new_width = t->term_cols - 20;
        if (new_width != t->sidebar_width) {
            t->sidebar_width = new_width;
            resize_all_ptys(t);
        }
        return;
    }

    /* Sidebar scrollbar drag motion */
    if (button == 32 && t->sidebar_sb_drag) {
        int list_rows = t->term_rows - 3;
        if (list_rows < 1) list_rows = 1;
        int total_items = t->num_sessions;
        int max_scroll = total_items - list_rows;
        if (max_scroll > 0 && list_rows > 0) {
            int slot = row - 1;
            int new_s = (slot * max_scroll) / (list_rows - 1);
            if (new_s < 0) new_s = 0;
            if (new_s > max_scroll) new_s = max_scroll;
            t->sidebar_scroll = new_s;
        }
        return;
    }

    /* Scrollbar drag motion */
    if (button == 32 && t->scrollbar_drag) {
        struct pane *p = cur_pane(t);
        if (p && p->vt.sb_len > 0) {
            struct session *s = &t->sessions[t->active];
            int max_off = p->vt.sb_len;
            int track_h, local_row;
            if (s->num_panes > 1) {
                track_h = p->h;
                local_row = row - p->y;
            } else {
                track_h = t->term_rows;
                local_row = row;
            }
            if (track_h > 1) {
                int new_off = ((track_h - 1 - local_row) * max_off) / (track_h - 1);
                if (new_off < 0) new_off = 0;
                if (new_off > max_off) new_off = max_off;
                p->vt.scroll_offset = new_off;
            }
        }
        return;
    }

    /* Horizontal scrollbar drag motion */
    if (button == 32 && t->hscrollbar_drag) {
        if (t->num_sessions > 0) {
            struct session *s = &t->sessions[t->active];
            int pi = t->hscrollbar_drag_pane;
            if (pi >= 0 && pi < s->num_panes) {
                struct pane *p = &s->panes[pi];
                struct vterm *vt = &p->vt;
                int visible_w, track_left;
                if (s->num_panes == 1) {
                    int gw = pane_gutter_width(p);
                    int sb = pane_has_scrollbar(p);
                    visible_w = p->w - gw - sb;
                    track_left = t->sidebar_width + gw;
                } else {
                    int sb = pane_has_scrollbar(p);
                    visible_w = p->w - sb;
                    track_left = t->sidebar_width + p->x;
                }
                if (visible_w < 1) visible_w = 1;
                int track_w = visible_w;
                int max_off = vt->cols - visible_w;
                if (max_off < 1) max_off = 1;
                int local_col = col - track_left;
                if (local_col < 0) local_col = 0;
                if (local_col >= track_w) local_col = track_w - 1;
                int new_off = (local_col * max_off) / (track_w - 1 > 0 ? track_w - 1 : 1);
                if (new_off < 0) new_off = 0;
                if (new_off > max_off) new_off = max_off;
                vt->col_offset = new_off;
            }
        }
        return;
    }

    /* Scroll wheel: button 64 = up, 65 = down */
    if (button == 64 || button == 65) {
        if (col < t->sidebar_width) {
            int has_new = (t->num_sessions < MAX_SESSIONS) ? 1 : 0;
            int total_items = t->num_sessions + has_new;
            int list_rows = t->term_rows - 3;
            if (list_rows < 1) list_rows = 1;
            if (total_items > list_rows) {
                if (button == 64) {
                    t->sidebar_scroll -= 3;
                    if (t->sidebar_scroll < 0)
                        t->sidebar_scroll = 0;
                } else {
                    t->sidebar_scroll += 3;
                    int max_s = total_items - list_rows;
                    if (t->sidebar_scroll > max_s)
                        t->sidebar_scroll = max_s;
                }
            }
            return;
        }
        /* Find the pane under the mouse cursor (scroll non-active panes) */
        int target_idx = find_pane_at(t, col, row);
        struct pane *p;
        if (target_idx >= 0) {
            struct session *s = &t->sessions[t->active];
            p = &s->panes[target_idx];
        } else {
            p = cur_pane(t);
        }
        if (p) {
            struct vterm *vt = &p->vt;
            struct session *ss = &t->sessions[t->active];

            /* Check if mouse is on the horizontal scrollbar row */
            {
                int visible_w, hbar_screen_row;
                if (ss->num_panes == 1) {
                    int gw = pane_gutter_width(p);
                    int sb = pane_has_scrollbar(p);
                    visible_w = p->w - gw - sb;
                    hbar_screen_row = vt->rows - 1;  /* 0-indexed */
                } else {
                    int sb = pane_has_scrollbar(p);
                    visible_w = p->w - sb;
                    hbar_screen_row = p->y + p->h - 1;  /* 0-indexed */
                }
                if (pane_needs_hscroll(p, visible_w) && row == hbar_screen_row) {
                    /* Convert vertical scroll to horizontal scroll */
                    int step = 8;
                    int max_off = vt->cols - visible_w;
                    if (max_off < 0) max_off = 0;
                    if (button == 64) {
                        vt->col_offset -= step;
                        if (vt->col_offset < 0)
                            vt->col_offset = 0;
                    } else {
                        vt->col_offset += step;
                        if (vt->col_offset > max_off)
                            vt->col_offset = max_off;
                    }
                    return;
                }
            }

            int adj_col, adj_row;
            int in_content;
            if (ss->num_panes == 1) {
                int gw = gutter_width(t);
                int sb = pane_has_scrollbar(p);
                int left = t->sidebar_width + gw;
                int right = t->term_cols - sb;
                in_content = (col >= left && col < right);
                adj_col = col - left + 1;
                adj_row = row + 1;
            } else {
                int sb = pane_has_scrollbar(p);
                int left = t->sidebar_width + p->x;
                int right = left + p->w - sb;
                in_content = (col >= left && col < right &&
                              row >= p->y && row < p->y + p->h);
                adj_col = col - left + 1;
                adj_row = row - p->y + 1;
            }
            if (vt->mouse_mode && in_content) {
                if (adj_col < 1) adj_col = 1;
                if (adj_row < 1) adj_row = 1;
                debug_mouse_forward(t->active,
                                     (target_idx >= 0) ? target_idx
                                     : ss->active_pane,
                                     button, adj_col, adj_row, 1);
                char mouse_seq[64];
                int sn = snprintf(mouse_seq, sizeof(mouse_seq),
                                  "\033[<%d;%d;%dM", button, adj_col, adj_row);
                IGNORE_RESULT(write(p->pty_fd, mouse_seq, (size_t)sn));
            } else if (!vt->alt_active) {
                int step = 3;
                if (button == 64) {
                    vt->scroll_offset += step;
                    if (vt->scroll_offset > vt->sb_len)
                        vt->scroll_offset = vt->sb_len;
                } else {
                    vt->scroll_offset -= step;
                    if (vt->scroll_offset < 0)
                        vt->scroll_offset = 0;
                }
            }
        }
        return;
    }

    /* Horizontal scroll: button 66 = left, 67 = right
     * Also Shift+scroll: button 68 = shift+up (left), 69 = shift+down (right) */
    if (button == 66 || button == 67 || button == 68 || button == 69) {
        if (col >= t->sidebar_width) {
            int target_idx = find_pane_at(t, col, row);
            struct pane *p;
            if (target_idx >= 0) {
                struct session *s = &t->sessions[t->active];
                p = &s->panes[target_idx];
            } else {
                p = cur_pane(t);
            }
            if (p) {
                struct vterm *vt = &p->vt;
                struct session *ss = &t->sessions[t->active];
                int visible_w;
                if (ss->num_panes == 1) {
                    int gw = pane_gutter_width(p);
                    int sb = pane_has_scrollbar(p);
                    visible_w = p->w - gw - sb;
                } else {
                    int sb = pane_has_scrollbar(p);
                    visible_w = p->w - sb;
                }
                int hscroll = pane_needs_hscroll(p, visible_w);
                if (hscroll) {
                    /* Reserve 1 row for horizontal scrollbar */
                    int step = 8;
                    int max_off = vt->cols - visible_w;
                    if (max_off < 0) max_off = 0;
                    if (button == 66 || button == 68) {
                        /* Scroll left */
                        vt->col_offset -= step;
                        if (vt->col_offset < 0)
                            vt->col_offset = 0;
                    } else {
                        /* Scroll right */
                        vt->col_offset += step;
                        if (vt->col_offset > max_off)
                            vt->col_offset = max_off;
                    }
                }
            }
        }
        return;
    }

    /* Non-left-click buttons: forward to child if it has mouse tracking */
    if (button != 0) {
        if (col >= t->sidebar_width && t->num_sessions > 0) {
            struct pane *p = cur_pane(t);
            if (p && p->alive && p->vt.mouse_mode && p->vt.scroll_offset == 0) {
                struct session *s = &t->sessions[t->active];
                int adj_col, adj_row;
                if (s->num_panes == 1) {
                    int gw = gutter_width(t);
                    adj_col = col - t->sidebar_width - gw + 1;
                    adj_row = row + 1;
                } else {
                    adj_col = col - (t->sidebar_width + p->x) + 1;
                    adj_row = row - p->y + 1;
                }
                if (adj_col < 1) adj_col = 1;
                if (adj_row < 1) adj_row = 1;
                debug_mouse_forward(t->active, s->active_pane,
                                     button, adj_col, adj_row, 1);
                char mouse_seq[64];
                int n = snprintf(mouse_seq, sizeof(mouse_seq),
                                 "\033[<%d;%d;%dM", button, adj_col, adj_row);
                IGNORE_RESULT(write(p->pty_fd, mouse_seq, (size_t)n));
            }
        }
        return;
    }

    /* Click on horizontal scrollbar row */
    if (t->num_sessions > 0 && col >= t->sidebar_width) {
        struct session *s = &t->sessions[t->active];
        for (int pi2 = 0; pi2 < s->num_panes; pi2++) {
            struct pane *pp = &s->panes[pi2];
            struct vterm *vt2 = &pp->vt;
            int visible_w, hbar_screen_row, track_left;
            if (s->num_panes == 1) {
                int gw = pane_gutter_width(pp);
                int sb = pane_has_scrollbar(pp);
                visible_w = pp->w - gw - sb;
                hbar_screen_row = vt2->rows - 1;  /* 0-indexed */
                track_left = t->sidebar_width + gw;
            } else {
                int sb = pane_has_scrollbar(pp);
                visible_w = pp->w - sb;
                hbar_screen_row = pp->y + pp->h - 1;  /* 0-indexed */
                track_left = t->sidebar_width + pp->x;
            }
            if (pane_needs_hscroll(pp, visible_w) && row == hbar_screen_row &&
                col >= track_left && col < track_left + visible_w) {
                /* Start h-scrollbar drag */
                t->hscrollbar_drag = 1;
                t->hscrollbar_drag_pane = pi2;
                if (s->num_panes > 1)
                    s->active_pane = pi2;
                /* Jump to click position */
                int track_w = visible_w;
                int max_off = vt2->cols - visible_w;
                if (max_off < 1) max_off = 1;
                int local_col = col - track_left;
                if (local_col < 0) local_col = 0;
                if (local_col >= track_w) local_col = track_w - 1;
                int new_off = (local_col * max_off) / (track_w - 1 > 0 ? track_w - 1 : 1);
                if (new_off < 0) new_off = 0;
                if (new_off > max_off) new_off = max_off;
                vt2->col_offset = new_off;
                return;
            }
        }
    }

    /* Click on scrollbar column (right edge) — single-pane mode */
    if (t->num_sessions > 0) {
        struct session *s = &t->sessions[t->active];
        struct pane *p = cur_pane(t);
        if (p && s->num_panes == 1 && p->vt.sb_len > 0 &&
            !p->vt.alt_active && col == t->term_cols - 1) {
            t->scrollbar_drag = 1;
            if (t->term_rows > 1) {
                int max_off = p->vt.sb_len;
                int new_off = ((t->term_rows - 1 - row) * max_off) / (t->term_rows - 1);
                if (new_off < 0) new_off = 0;
                if (new_off > max_off) new_off = max_off;
                p->vt.scroll_offset = new_off;
            }
            return;
        }
        /* Click on per-pane scrollbar in multi-pane mode */
        if (s->num_panes > 1) {
            int cx = col - t->sidebar_width;
            for (int pi2 = 0; pi2 < s->num_panes; pi2++) {
                struct pane *pp = &s->panes[pi2];
                int pp_sb = pane_has_scrollbar(pp);
                if (pp_sb && cx == pp->x + pp->w - 1 &&
                    row >= pp->y && row < pp->y + pp->h) {
                    s->active_pane = pi2;
                    t->scrollbar_drag = 1;
                    if (pp->h > 1) {
                        int max_off = pp->vt.sb_len;
                        int local_row = row - pp->y;
                        int new_off = ((pp->h - 1 - local_row) * max_off) / (pp->h - 1);
                        if (new_off < 0) new_off = 0;
                        if (new_off > max_off) new_off = max_off;
                        pp->vt.scroll_offset = new_off;
                    }
                    return;
                }
            }
        }
    }

    /* Click on sidebar scrollbar column */
    {
        int total_items = t->num_sessions;
        int list_rows = t->term_rows - 3;
        if (list_rows < 1) list_rows = 1;
        int sb_needed = (total_items > list_rows);
        if (sb_needed && col == t->sidebar_width - 2 && row >= 1 && row < t->term_rows - 2) {
            t->sidebar_sb_drag = 1;
            int max_scroll = total_items - list_rows;
            if (max_scroll > 0) {
                int slot = row - 1;
                int new_s = (slot * max_scroll) / (list_rows - 1);
                if (new_s < 0) new_s = 0;
                if (new_s > max_scroll) new_s = max_scroll;
                t->sidebar_scroll = new_s;
            }
            return;
        }
    }

    /* Click on the sidebar border to start drag */
    if (col >= t->sidebar_width - 1 && col < t->sidebar_width) {
        t->sidebar_drag = 1;
        return;
    }

    if (col < t->sidebar_width - 1) {
        /* [+] button on title bar row */
        if (row == 0 && t->num_sessions < MAX_SESSIONS) {
            int nblen = 3;
            int btn_start = t->sidebar_width - 1 - nblen;
            if (col >= btn_start) {
                session_create(t, t->default_shell);
                return;
            }
        }
        /* Action button row */
        if (row == t->term_rows - 2) {
            int btn_w = (t->sidebar_width - 1) / 4;
            int region = col / btn_w;
            if (region > 3) region = 3;
            switch (region) {
            case 0: /* Split Vertical */
                split_pane(t, SPLIT_VERT, NULL);
                break;
            case 1: /* Split Horizontal */
                split_pane(t, SPLIT_HORIZ, NULL);
                break;
            case 2: /* Search */
                action_mode_start(t, 1);
                break;
            case 3: /* Close */
                if (t->num_sessions > 0) {
                    struct session *cs = &t->sessions[t->active];
                    if (cs->num_panes > 1) {
                        int pi = cs->active_pane;
                        struct pane *cp = &cs->panes[pi];
                        if (cp->pty_fd >= 0) {
                            close(cp->pty_fd);
                            cp->pty_fd = -1;
                        }
                        if (cp->pid > 0)
                            kill(cp->pid, SIGTERM);
                        vterm_free(&cp->vt);
                        remove_pane_from_layout(cs, pi);
                        compact_panes(cs, pi);
                        int cw2 = t->term_cols - t->sidebar_width;
                        int ch2 = t->term_rows;
                        if (cw2 < 2) cw2 = 2;
                        if (ch2 < 2) ch2 = 2;
                        layout_reflow(cs, cs->root_node, 0, 0, cw2, ch2);
                        resize_session_panes(t, cs);
                    } else {
                        session_close(t, t->active);
                        if (t->num_sessions == 0)
                            t->running = 0;
                    }
                }
                break;
            }
            return;
        }
        /* Bottom button bar */
        if (row == t->term_rows - 1) {
            int btn_w = (t->sidebar_width - 1) / 2;
            if (col < btn_w) {
                t->show_help = !t->show_help;
            } else {
                t->running = 0;
            }
            return;
        }
        /* Click in sidebar session list area */
        int list_end = t->term_rows - 2;
        if (row >= 1 && row < list_end) {
            int item_idx = (row - 1) + t->sidebar_scroll;
            if (item_idx >= 0 && item_idx < t->num_sessions) {
                t->active = item_idx;
            }
        }
    } else {
        /* Click in terminal area */
        if (t->num_sessions > 0) {
            struct session *s = &t->sessions[t->active];

            /* Check if click is on a split divider to start drag */
            if (s->num_panes > 1) {
                int cx = col - t->sidebar_width;
                int cy = row;
                int cw = t->term_cols - t->sidebar_width;
                int ch2 = t->term_rows;
                struct divider_hit dh;
                if (hit_test_divider(s, s->root_node, 0, 0,
                                     cw, ch2, cx, cy, &dh)) {
                    t->split_drag = 1;
                    t->split_drag_node = dh.node_idx;
                    t->split_drag_x = dh.x;
                    t->split_drag_y = dh.y;
                    t->split_drag_w = dh.w;
                    t->split_drag_h = dh.h;
                    return;
                }
            }

            /* In multi-pane mode, switch focus to clicked pane */
            if (s->num_panes > 1) {
                int pane_idx = find_pane_at(t, col, row);
                if (pane_idx >= 0 && pane_idx != s->active_pane) {
                    s->active_pane = pane_idx;
                    clear_selection(t);
                }
            }

            struct pane *p = cur_pane(t);
            if (p && p->alive) {
                struct vterm *vt = &p->vt;
                if (vt->mouse_mode && vt->scroll_offset == 0) {
                    /* Forward to child app */
                    int adj_col, adj_row;
                    if (s->num_panes == 1) {
                        int gw = gutter_width(t);
                        adj_col = col - t->sidebar_width - gw + 1;
                        adj_row = row + 1;
                    } else {
                        adj_col = col - (t->sidebar_width + p->x) + 1;
                        adj_row = row - p->y + 1;
                    }
                    if (adj_col < 1) adj_col = 1;
                    if (adj_row < 1) adj_row = 1;
                    debug_mouse_forward(t->active, s->active_pane,
                                         button, adj_col, adj_row, 1);
                    char mouse_seq[64];
                    int n = snprintf(mouse_seq, sizeof(mouse_seq),
                                     "\033[<%d;%d;%dM", button, adj_col, adj_row);
                    IGNORE_RESULT(write(p->pty_fd, mouse_seq, (size_t)n));
                } else {
                    /* Text selection */
                    int abs_line, cell_col;
                    if (screen_to_cell(t, col, row, &abs_line, &cell_col)) {
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        long ms = (now.tv_sec - t->sel_last_click.tv_sec) * 1000 +
                                  (now.tv_nsec - t->sel_last_click.tv_nsec) / 1000000;
                        int same_pos = (abs(abs_line - t->sel_last_click_line) <= 0 &&
                                        abs(cell_col - t->sel_last_click_col) <= 1);

                        int click_count = 1;
                        if (ms < 400 && same_pos) {
                            click_count = t->sel_click_count + 1;
                            if (click_count > 3) click_count = 1;
                        }

                        t->sel_last_click = now;
                        t->sel_last_click_line = abs_line;
                        t->sel_last_click_col = cell_col;
                        t->sel_click_count = click_count;

                        if (click_count == 1) {
                            t->sel_start_line = abs_line;
                            t->sel_start_col = cell_col;
                            t->sel_end_line = abs_line;
                            t->sel_end_col = cell_col;
                        } else if (click_count == 2) {
                            int ws, we;
                            expand_to_word(vt, abs_line, cell_col, &ws, &we);
                            t->sel_start_line = abs_line;
                            t->sel_start_col = ws;
                            t->sel_end_line = abs_line;
                            t->sel_end_col = we;
                        } else {
                            int ls, le;
                            expand_to_line(vt, abs_line, &ls, &le);
                            t->sel_start_line = abs_line;
                            t->sel_start_col = ls;
                            t->sel_end_line = abs_line;
                            t->sel_end_col = le;
                        }

                        t->sel_active = 1;
                        t->sel_dragging = 1;
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Flush input escape buffer to active PTY                           */
/* ------------------------------------------------------------------ */

static void inp_flush(struct ttabmux *t)
{
    struct pane *p = cur_pane(t);
    if (t->inp_len > 0 && p && p->alive) {
        IGNORE_RESULT(write(p->pty_fd, t->inp_buf, (size_t)t->inp_len));
    }
    t->inp_len = 0;
    t->inp_state = 0;
}

/* ------------------------------------------------------------------ */
/*  Parse SGR mouse parameters from inp_buf                           */
/* ------------------------------------------------------------------ */

static void parse_sgr_mouse(struct ttabmux *t, int is_press)
{
    int button = 0, col = 0, row = 0;
    int field = 0;
    for (int i = 0; i < t->inp_len; i++) {
        char c = t->inp_buf[i];
        if (c >= '0' && c <= '9') {
            int *target = (field == 0) ? &button : (field == 1) ? &col : &row;
            *target = *target * 10 + (c - '0');
        } else if (c == ';') {
            field++;
        }
    }
    t->inp_len = 0;
    t->inp_state = 0;

    handle_mouse(t, button, col, row, is_press);
}

/* ------------------------------------------------------------------ */
/*  Handle user input                                                 */
/* ------------------------------------------------------------------ */

static void handle_input(struct ttabmux *t, const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];

        /* Help overlay: any non-ESC key dismisses (ESC starts mouse seqs) */
        if (t->show_help && t->inp_state == 0 && ch != 0x1B) {
            t->show_help = 0;
            continue;
        }

        /* Action bar mode (search / jump-to-line / search nav) */
        if (t->action_mode && t->inp_state == 0 && ch != 0x1B) {
            handle_action_input(t, ch);
            continue;
        }

        /* Rename mode */
        if (t->rename_mode && t->inp_state == 0) {
            handle_rename_input(t, ch);
            continue;
        }

        /* Prefix mode state 2: got ESC in prefix mode, expect '[' */
        if (t->prefix_mode == 2 && t->inp_state == 0) {
            if (ch == '[') {
                t->prefix_mode = 3;
            } else {
                t->prefix_mode = 0;
            }
            continue;
        }

        /* Prefix mode state 3: got ESC[ in prefix mode, expect arrow letter */
        if (t->prefix_mode == 3 && t->inp_state == 0) {
            t->prefix_mode = 0;
            switch (ch) {
            case 'A': navigate_pane(t, 0, -1); break;  /* Up */
            case 'B': navigate_pane(t, 0, 1); break;   /* Down */
            case 'C': navigate_pane(t, 1, 0); break;   /* Right */
            case 'D': navigate_pane(t, -1, 0); break;  /* Left */
            default: break;
            }
            continue;
        }

        /* Prefix mode */
        if (t->prefix_mode == 1 && t->inp_state == 0) {
            handle_prefix_cmd(t, ch);
            continue;
        }

        /* Input escape sequence state machine */
        switch (t->inp_state) {
        case 0: /* Normal */
            if (ch == 0x1B) {
                t->inp_buf[0] = (char)ch;
                t->inp_len = 1;
                t->inp_state = 1;
                t->esc_count++;
                if (t->esc_count >= 3) {
                    t->running = 0;
                    return;
                }
            } else if (ch == PREFIX_KEY) {
                t->prefix_mode = 1;
                t->esc_count = 0;
            } else {
                t->esc_count = 0;
                struct pane *p = cur_pane(t);
                if (p && p->alive) {
                    int remaining = len - i;
                    int chunk = 0;
                    while (chunk < remaining) {
                        unsigned char b = (unsigned char)data[i + chunk];
                        if (b == PREFIX_KEY || b == 0x1B)
                            break;
                        chunk++;
                    }
                    if (chunk > 0) {
                        if (t->sel_active)
                            clear_selection(t);
                        p->vt.scroll_offset = 0;
                        IGNORE_RESULT(write(p->pty_fd, data + i, (size_t)chunk));
                        i += chunk - 1;
                    }
                }
            }
            break;

        case 1: /* Got ESC */
            if (ch == '[') {
                t->inp_buf[t->inp_len++] = (char)ch;
                t->inp_state = 2;
                t->esc_count = 0;
            } else if (t->action_mode) {
                /* Lone ESC in action mode: cancel search/action */
                t->inp_len = 0;
                t->inp_state = 0;
                t->search_match_line = -1;
                action_mode_end(t);
            } else {
                t->inp_buf[t->inp_len++] = (char)ch;
                inp_flush(t);
            }
            break;

        case 2: /* Got ESC[ (CSI) */
            if (ch == '<') {
                t->inp_len = 0;
                t->inp_state = 3;
            } else {
                t->inp_buf[t->inp_len++] = (char)ch;
                if (ch >= 0x40 && ch <= 0x7E) {
                    /* Intercept Ctrl+Up/Down (ESC[1;5A / ESC[1;5B)
                       to navigate sessions in the sidebar */
                    if (t->inp_len == 6 &&
                        t->inp_buf[2] == '1' && t->inp_buf[3] == ';' &&
                        t->inp_buf[4] == '5' && (ch == 'A' || ch == 'B')) {
                        t->inp_len = 0;
                        t->inp_state = 0;
                        if (t->num_sessions > 1) {
                            if (ch == 'A') /* Ctrl+Up: previous session */
                                t->active = (t->active - 1 + t->num_sessions) % t->num_sessions;
                            else           /* Ctrl+Down: next session */
                                t->active = (t->active + 1) % t->num_sessions;
                        }
                    } else {
                        inp_flush(t);
                    }
                }
                else if (t->inp_len >= (int)sizeof(t->inp_buf) - 1) {
                    inp_flush(t);
                }
            }
            break;

        case 3: /* Got ESC[< (SGR mouse params) */
            if (ch == 'M') {
                parse_sgr_mouse(t, 1);
            } else if (ch == 'm') {
                parse_sgr_mouse(t, 0);
            } else if ((ch >= '0' && ch <= '9') || ch == ';') {
                if (t->inp_len < (int)sizeof(t->inp_buf) - 1) {
                    t->inp_buf[t->inp_len++] = (char)ch;
                } else {
                    t->inp_len = 0;
                    t->inp_state = 0;
                }
            } else {
                t->inp_len = 0;
                t->inp_state = 0;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Main event loop                                                   */
/* ------------------------------------------------------------------ */

static void event_loop(struct ttabmux *t)
{
    char buf[4096];

    while (t->running) {
        /* Handle pending signals */
        if (g_winch) {
            g_winch = 0;
            get_term_size(t);
            resize_all_ptys(t);
            render_screen(t);
        }

        if (g_child) {
            g_child = 0;
            reap_children(t);
            cleanup_dead_panes(t);
            render_screen(t);
        }

        /* Build poll set: stdin + all pane PTY fds */
        struct pollfd fds[MAX_SESSIONS * MAX_PANES + 1];
        int fd_map_session[MAX_SESSIONS * MAX_PANES + 1];
        int fd_map_pane[MAX_SESSIONS * MAX_PANES + 1];
        int nfds = 0;

        fds[nfds].fd = STDIN_FILENO;
        fds[nfds].events = POLLIN;
        nfds++;

        for (int i = 0; i < t->num_sessions; i++) {
            struct session *s = &t->sessions[i];
            for (int j = 0; j < s->num_panes; j++) {
                struct pane *p = &s->panes[j];
                if (p->alive && p->pty_fd >= 0) {
                    fd_map_session[nfds] = i;
                    fd_map_pane[nfds] = j;
                    fds[nfds].fd = p->pty_fd;
                    fds[nfds].events = POLLIN;
                    nfds++;
                }
            }
        }

        int ret = poll(fds, (nfds_t)nfds, 100 /* ms */);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        int need_render = 0;

        /* Lone ESC timeout: if we're waiting for a follow-up byte after ESC
         * and poll returned without stdin data, treat it as a bare ESC press */
        if (t->inp_state == 1 && !(fds[0].revents & POLLIN)) {
            if (t->action_mode) {
                t->inp_len = 0;
                t->inp_state = 0;
                t->search_match_line = -1;
                action_mode_end(t);
                need_render = 1;
            } else {
                inp_flush(t);
            }
        }

        /* Check stdin */
        if (fds[0].revents & POLLIN) {
            int n = (int)read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                debug_stdin(buf, n);
                handle_input(t, buf, n);
                need_render = 1;
            }
        }

        /* Check PTY outputs */
        for (int k = 1; k < nfds; k++) {
            if (!(fds[k].revents & (POLLIN | POLLHUP)))
                continue;

            int si = fd_map_session[k];
            int pi = fd_map_pane[k];
            if (si >= t->num_sessions) continue;
            struct session *s = &t->sessions[si];
            if (pi >= s->num_panes) continue;
            struct pane *p = &s->panes[pi];

            int n = (int)read(p->pty_fd, buf, sizeof(buf));
            if (n > 0) {
                debug_pty_output(si, pi, buf, n);
                /* Track scrollbar changes before processing */
                int old_gw = 0, old_sb = 0;
                old_sb = pane_has_scrollbar(p);
                if (s->num_panes == 1)
                    old_gw = pane_gutter_width(p);

                vterm_process(&p->vt, buf, n);

                /* Flush any responses (DA, DSR) back to the child */
                if (p->vt.resp_len > 0 && p->alive && p->pty_fd >= 0) {
                    IGNORE_RESULT(write(p->pty_fd, p->vt.resp_buf,
                                        (size_t)p->vt.resp_len));
                    p->vt.resp_len = 0;
                }

                /* Resize PTY if gutter/scrollbar width changed */
                if (t->wide_cols <= 0) {
                if (s->num_panes == 1) {
                    int new_gw = pane_gutter_width(p);
                    int new_sb = pane_has_scrollbar(p);
                    if (new_gw != old_gw || new_sb != old_sb) {
                        int pr = p->h;
                        int pc = p->w - new_gw - new_sb;
                        if (pr < 2) pr = 2;
                        if (pc < 2) pc = 2;
                        struct winsize rws = {
                            .ws_row = (unsigned short)pr,
                            .ws_col = (unsigned short)pc,
                            .ws_xpixel = 0, .ws_ypixel = 0
                        };
                        if (p->alive && p->pty_fd >= 0) {
                            ioctl(p->pty_fd, TIOCSWINSZ, &rws);
                            kill(p->pid, SIGWINCH);
                        }
                        vterm_resize(&p->vt, pr, pc);
                    }
                } else {
                    /* Multi-pane: resize if scrollbar appeared/disappeared */
                    int new_sb = pane_has_scrollbar(p);
                    if (new_sb != old_sb) {
                        int pr = p->h;
                        int pc = p->w - new_sb;
                        if (pr < 2) pr = 2;
                        if (pc < 2) pc = 2;
                        struct winsize rws = {
                            .ws_row = (unsigned short)pr,
                            .ws_col = (unsigned short)pc,
                            .ws_xpixel = 0, .ws_ypixel = 0
                        };
                        if (p->alive && p->pty_fd >= 0) {
                            ioctl(p->pty_fd, TIOCSWINSZ, &rws);
                            kill(p->pid, SIGWINCH);
                        }
                        vterm_resize(&p->vt, pr, pc);
                    }
                }
                } /* wide_cols <= 0 */

                /* Update tab name on OSC title change (skip if user renamed) */
                if (p->vt.title_changed) {
                    if (pi == s->active_pane && !s->renamed) {
                        snprintf(s->name, sizeof(s->name),
                                 "%s", p->vt.title);
                    }
                    p->vt.title_changed = 0;
                    need_render = 1;
                }

                if (si == t->active) {
                    if (pi == s->active_pane)
                        p->vt.scroll_offset = 0;
                    need_render = 1;
                }
            } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
                p->alive = 0;
                if (p->pty_fd >= 0) { close(p->pty_fd); p->pty_fd = -1; }
                need_render = 1;
            }
        }

        /* Clean up dead panes after processing output */
        {
            int did_cleanup = 0;
            for (int i = 0; i < t->num_sessions; i++) {
                struct session *s = &t->sessions[i];
                int changed = 0;
                for (int j = s->num_panes - 1; j >= 0; j--) {
                    if (!s->panes[j].alive && s->num_panes > 1) {
                        struct pane *p = &s->panes[j];
                        if (p->pty_fd >= 0) {
                            close(p->pty_fd);
                            p->pty_fd = -1;
                        }
                        vterm_free(&p->vt);
                        remove_pane_from_layout(s, j);
                        compact_panes(s, j);
                        changed = 1;
                    }
                }
                if (changed) {
                    int cw = t->term_cols - t->sidebar_width;
                    int ch = t->term_rows;
                    if (cw < 2) cw = 2;
                    if (ch < 2) ch = 2;
                    layout_reflow(s, s->root_node, 0, 0, cw, ch);
                    resize_session_panes(t, s);
                    did_cleanup = 1;
                }
            }
            if (did_cleanup) {
                need_render = 1;
            }
        }

        if (need_render) {
            render_screen(t);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Usage                                                             */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Startup action types for CLI argument processing                  */
/* ------------------------------------------------------------------ */

#define MAX_STARTUP_ACTIONS 256
enum startup_action_type { ACT_SESSION, ACT_PROGRAM, ACT_VSPLIT, ACT_HSPLIT, ACT_RENAME };
struct startup_action {
    enum startup_action_type type;
    const char *arg;
};

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] [program ...]\n"
        "\n"
        "Options:\n"
        "  -s WIDTH          Sidebar width (default: %d)\n"
        "  -e SHELL          Default shell for new tabs (default: $SHELL)\n"
        "  -S, --session NAME  Create a new session named NAME\n"
        "  -V, --vsplit CMD    Split current pane vertically, run CMD\n"
        "  -H, --hsplit CMD    Split current pane horizontally, run CMD\n"
        "  -w, --wide COLS     Wide mode: PTY columns (enables horizontal scroll)\n"
        "  -r, --rename NAME   Rename the current (last created) session\n"
        "  -h, --help          Show this help\n"
        "\n"
        "Options -S, -V, -H, -r are processed left-to-right to build\n"
        "the initial layout. Positional args each create a session.\n"
        "\n"
        "Key bindings (prefix: Ctrl+B):\n"
        "  c          Create new terminal\n"
        "  n / p      Next / previous terminal\n"
        "  1-9        Switch to terminal N\n"
        "  x          Close current pane (or tab)\n"
        "  v          Vertical split\n"
        "  s          Horizontal split\n"
        "  o          Cycle pane focus\n"
        "  Arrows     Navigate panes\n"
        "  ,          Rename current terminal\n"
        "  d          Detach (quit)\n"
        "  ?          Show help overlay\n"
        "  Ctrl+B     Send literal Ctrl+B\n"
        "\n"
        "  ESC x3     Quick quit (press ESC 3 times)\n"
        "\n"
        "Examples:\n"
        "  %s htop\n"
        "    Open a single session running htop\n"
        "\n"
        "  %s -S dev -V vim -H make\n"
        "    Create session \"dev\" with 3 panes (shell + vim vsplit + make hsplit)\n"
        "\n"
        "  %s -S logs -V 'tail -f /var/log/syslog' -S build -r build make\n"
        "    Two sessions: \"logs\" with a vertical split, and \"build\" running make\n",
        prog, SIDEBAR_WIDTH, prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    const char *shell = NULL;
    int sidebar_w = SIDEBAR_WIDTH;

    struct startup_action actions[MAX_STARTUP_ACTIONS];
    int num_actions = 0;

    int wide_cols = 0;

    static struct option long_options[] = {
        {"session", required_argument, NULL, 'S'},
        {"vsplit",  required_argument, NULL, 'V'},
        {"hsplit",  required_argument, NULL, 'H'},
        {"rename",  required_argument, NULL, 'r'},
        {"wide",    required_argument, NULL, 'w'},
        {"help",    no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:e:w:S:V:H:r:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 's':
            sidebar_w = atoi(optarg);
            if (sidebar_w < 25) sidebar_w = 25;
            if (sidebar_w > 60) sidebar_w = 60;
            break;
        case 'e':
            shell = optarg;
            break;
        case 'w':
            wide_cols = atoi(optarg);
            if (wide_cols < 0) wide_cols = 0;
            break;
        case 'S':
            if (num_actions < MAX_STARTUP_ACTIONS) {
                actions[num_actions].type = ACT_SESSION;
                actions[num_actions].arg = optarg;
                num_actions++;
            }
            break;
        case 'V':
            if (num_actions < MAX_STARTUP_ACTIONS) {
                actions[num_actions].type = ACT_VSPLIT;
                actions[num_actions].arg = optarg;
                num_actions++;
            }
            break;
        case 'H':
            if (num_actions < MAX_STARTUP_ACTIONS) {
                actions[num_actions].type = ACT_HSPLIT;
                actions[num_actions].arg = optarg;
                num_actions++;
            }
            break;
        case 'r':
            if (num_actions < MAX_STARTUP_ACTIONS) {
                actions[num_actions].type = ACT_RENAME;
                actions[num_actions].arg = optarg;
                num_actions++;
            }
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* Convert positional args into ACT_PROGRAM actions */
    for (int i = optind; i < argc && num_actions < MAX_STARTUP_ACTIONS; i++) {
        actions[num_actions].type = ACT_PROGRAM;
        actions[num_actions].arg = argv[i];
        num_actions++;
    }

    debug_open();

    /* Initialize app state on the heap (struct is large with panes) */
    struct ttabmux *t = calloc(1, sizeof(*t));
    if (!t) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    t->sidebar_width = sidebar_w;
    t->default_shell = shell;
    t->wide_cols = wide_cols;
    t->running = 1;
    t->split_hover_node = -1;

    /* Get terminal size */
    get_term_size(t);

    if (t->term_cols < t->sidebar_width + 20) {
        fprintf(stderr, "Terminal too narrow (need at least %d columns)\n",
                t->sidebar_width + 20);
        free(t);
        return 1;
    }

    /* Enter raw mode */
    if (term_raw_mode(t) < 0) {
        fprintf(stderr, "Failed to set raw mode\n");
        free(t);
        return 1;
    }

    /* Set up signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = handle_sigwinch;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    sa.sa_handler = handle_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* Switch to alternate screen buffer */
    IGNORE_RESULT(write(STDOUT_FILENO, "\033[?1049h", 8));
    /* Enable mouse tracking (X11 button + SGR extended mode) */
    IGNORE_RESULT(write(STDOUT_FILENO, "\033[?1003h\033[?1006h", 16));
    /* Clear screen */
    IGNORE_RESULT(write(STDOUT_FILENO, "\033[2J", 4));

    /* Process startup actions */
    if (num_actions > 0) {
        for (int i = 0; i < num_actions; i++) {
            switch (actions[i].type) {
            case ACT_SESSION: {
                session_create(t, actions[i].arg);
                break;
            }
            case ACT_PROGRAM:
                session_create(t, actions[i].arg);
                break;
            case ACT_VSPLIT:
                if (t->num_sessions > 0)
                    split_pane(t, SPLIT_VERT, actions[i].arg);
                break;
            case ACT_HSPLIT:
                if (t->num_sessions > 0)
                    split_pane(t, SPLIT_HORIZ, actions[i].arg);
                break;
            case ACT_RENAME:
                if (t->num_sessions > 0) {
                    struct session *s = &t->sessions[t->active];
                    snprintf(s->name, sizeof(s->name), "%s", actions[i].arg);
                    s->renamed = 1;
                }
                break;
            }
        }
        if (t->num_sessions > 0)
            t->active = 0;
    } else {
        session_create(t, shell);
    }
    if (t->num_sessions == 0) {
        IGNORE_RESULT(write(STDOUT_FILENO, "\033[?1006l\033[?1003l", 16));
        IGNORE_RESULT(write(STDOUT_FILENO, "\033[?1049l", 8));
        term_restore(t);
        fprintf(stderr, "Failed to create terminal session\n");
        free(t);
        return 1;
    }

    /* Initial render */
    render_screen(t);

    /* Run event loop */
    event_loop(t);

    /* Cleanup */
    for (int i = t->num_sessions - 1; i >= 0; i--)
        session_close(t, i);

    render_free(t);

    /* Disable mouse tracking */
    IGNORE_RESULT(write(STDOUT_FILENO, "\033[?1006l\033[?1003l", 16));
    /* Leave alternate screen buffer */
    IGNORE_RESULT(write(STDOUT_FILENO, "\033[?1049l", 8));

    /* Restore terminal */
    term_restore(t);

    debug_close();

    free(t);
    return 0;
}
