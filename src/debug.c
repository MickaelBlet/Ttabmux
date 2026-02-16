/*
 * debug.c - Debug logging for ttabmux
 *
 * Writes human-readable event traces to /tmp/ttabmux_debug.log.
 * Only compiled when TTABMUX_DEBUG is defined.
 */

#define _XOPEN_SOURCE 600

#include "debug.h"

#ifdef TTABMUX_DEBUG

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *g_debug_fp;

static void timestamp(char *buf, int sz)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(buf, (size_t)sz, "%ld.%03ld",
             (long)ts.tv_sec, ts.tv_nsec / 1000000);
}

void debug_open(void)
{
    if (g_debug_fp) return;
    g_debug_fp = fopen("/tmp/ttabmux_debug.log", "a");
    if (g_debug_fp) {
        setvbuf(g_debug_fp, NULL, _IOLBF, 0);
        fprintf(g_debug_fp,
                "\n====== ttabmux debug session (pid %d) ======\n",
                (int)getpid());
    }
}

void debug_close(void)
{
    if (g_debug_fp) {
        fprintf(g_debug_fp, "====== session end ======\n\n");
        fclose(g_debug_fp);
        g_debug_fp = NULL;
    }
}

void debug_stdin(const char *data, int len)
{
    if (!g_debug_fp) return;
    char ts[32];
    timestamp(ts, (int)sizeof(ts));

    fprintf(g_debug_fp, "[%s] STDIN (%d bytes): ", ts, len);
    for (int i = 0; i < len && i < 128; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c >= 0x20 && c < 0x7F)
            fputc(c, g_debug_fp);
        else
            fprintf(g_debug_fp, "\\x%02x", c);
    }
    if (len > 128)
        fprintf(g_debug_fp, " ... (%d more)", len - 128);
    fputc('\n', g_debug_fp);
}

void debug_mouse(int button, int col, int row, int is_press,
                 int session_idx, int pane_idx,
                 int pane_x, int pane_y, int pane_w, int pane_h,
                 int vt_rows, int vt_cols, int mouse_mode)
{
    if (!g_debug_fp) return;
    char ts[32];
    timestamp(ts, (int)sizeof(ts));

    const char *type;
    switch (button) {
    case 0:  type = "LEFT";        break;
    case 1:  type = "MIDDLE";      break;
    case 2:  type = "RIGHT";       break;
    case 32: type = "DRAG";        break;
    case 35: type = "HOVER";       break;
    case 64: type = "SCROLL_UP";   break;
    case 65: type = "SCROLL_DOWN"; break;
    default: type = "OTHER";       break;
    }

    fprintf(g_debug_fp,
            "[%s] MOUSE %s btn=%d col=%d row=%d %s | "
            "session=%d pane=%d pos=(%d,%d) size=(%dx%d) "
            "vt=(%dx%d) mouse_mode=%d\n",
            ts, type, button, col, row,
            is_press ? "PRESS" : "RELEASE",
            session_idx, pane_idx,
            pane_x, pane_y, pane_w, pane_h,
            vt_cols, vt_rows, mouse_mode);
}

void debug_pty_output(int session_idx, int pane_idx,
                      const char *data, int len)
{
    if (!g_debug_fp) return;
    char ts[32];
    timestamp(ts, (int)sizeof(ts));

    fprintf(g_debug_fp,
            "[%s] PTY_OUT session=%d pane=%d (%d bytes): ",
            ts, session_idx, pane_idx, len);
    int show = len < 64 ? len : 64;
    for (int i = 0; i < show; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c >= 0x20 && c < 0x7F)
            fputc(c, g_debug_fp);
        else
            fprintf(g_debug_fp, "\\x%02x", c);
    }
    if (len > 64)
        fprintf(g_debug_fp, " ... (%d more)", len - 64);
    fputc('\n', g_debug_fp);
}

void debug_mouse_forward(int session_idx, int pane_idx,
                          int button, int adj_col, int adj_row,
                          int is_press)
{
    if (!g_debug_fp) return;
    char ts[32];
    timestamp(ts, (int)sizeof(ts));

    fprintf(g_debug_fp,
            "[%s] MOUSE_FWD session=%d pane=%d btn=%d "
            "adj_col=%d adj_row=%d %s\n",
            ts, session_idx, pane_idx,
            button, adj_col, adj_row,
            is_press ? "PRESS" : "RELEASE");
}

void debug_log(const char *fmt, ...)
{
    if (!g_debug_fp) return;
    char ts[32];
    timestamp(ts, (int)sizeof(ts));

    fprintf(g_debug_fp, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_debug_fp, fmt, ap);
    va_end(ap);
    fputc('\n', g_debug_fp);
}

#else /* !TTABMUX_DEBUG */

/* Avoid empty translation unit warning */
typedef int debug_unused;

#endif /* TTABMUX_DEBUG */
