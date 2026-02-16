/*
 * debug.h - Debug logging for ttabmux
 *
 * Logs every input event (stdin, mouse, PTY output) with pane
 * information to /tmp/ttabmux_debug.log.
 *
 * Enabled at compile time with -DTTABMUX_DEBUG.
 */

#ifndef TTABMUX_DEBUG_H
#define TTABMUX_DEBUG_H

#ifdef TTABMUX_DEBUG

void debug_open(void);
void debug_close(void);

/* Log raw stdin bytes (user keyboard input) */
void debug_stdin(const char *data, int len);

/* Log a parsed mouse event */
void debug_mouse(int button, int col, int row, int is_press,
                 int session_idx, int pane_idx,
                 int pane_x, int pane_y, int pane_w, int pane_h,
                 int vt_rows, int vt_cols, int mouse_mode);

/* Log raw PTY output from a child process */
void debug_pty_output(int session_idx, int pane_idx,
                      const char *data, int len);

/* Log a mouse sequence forwarded to a child */
void debug_mouse_forward(int session_idx, int pane_idx,
                          int button, int adj_col, int adj_row,
                          int is_press);

/* Log arbitrary debug message */
void debug_log(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#else /* !TTABMUX_DEBUG */

#define debug_open()                          ((void)0)
#define debug_close()                         ((void)0)
#define debug_stdin(d, l)                     ((void)0)
#define debug_mouse(b, c, r, p, si, pi, px, py, pw, ph, vr, vc, mm) ((void)0)
#define debug_pty_output(si, pi, d, l)        ((void)0)
#define debug_mouse_forward(si, pi, b, c, r, p) ((void)0)
#define debug_log(...)                        ((void)0)

#endif /* TTABMUX_DEBUG */
#endif /* TTABMUX_DEBUG_H */
