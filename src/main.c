/*
 * main.c - Ttabmux entry point and event loop
 *
 * Manages terminal sessions via PTYs, handles user input (including
 * the Ctrl+B prefix key), multiplexes I/O with poll(), and
 * coordinates rendering.
 */

#define _XOPEN_SOURCE 600
#define _GNU_SOURCE

#include "ttabmux.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
/*  Session / PTY management                                          */
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

    struct winsize ws = {
        .ws_row = (unsigned short)pty_rows,
        .ws_col = (unsigned short)pty_cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    pid_t pid = forkpty(&s->pty_fd, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        return -1;
    }

    if (pid == 0) {
        /* Child process */
        /* Restore signals */
        signal(SIGWINCH, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        /* Set TERM */
        setenv("TERM", "xterm-256color", 1);

        /* Unset TMUX to avoid nested detection issues */
        unsetenv("TMUX");

        /* Execute shell */
        const char *sh = shell;
        if (!sh) sh = getenv("SHELL");
        if (!sh) sh = "/bin/sh";

        execlp(sh, sh, (char *)NULL);
        perror("exec");
        _exit(1);
    }

    /* Parent */
    s->pid = pid;
    s->alive = 1;
    snprintf(s->name, sizeof(s->name), "bash");

    /* Try to get a better name from the shell path */
    const char *sh = shell ? shell : getenv("SHELL");
    if (sh) {
        const char *base = strrchr(sh, '/');
        snprintf(s->name, sizeof(s->name), "%s", base ? base + 1 : sh);
    }

    /* Set PTY non-blocking */
    int flags = fcntl(s->pty_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(s->pty_fd, F_SETFL, flags | O_NONBLOCK);

    /* Initialize virtual terminal */
    vterm_init(&s->vt, pty_rows, pty_cols);

    t->num_sessions++;
    t->active = idx;

    return idx;
}

void session_close(struct ttabmux *t, int idx)
{
    if (idx < 0 || idx >= t->num_sessions) return;

    struct session *s = &t->sessions[idx];

    if (s->pty_fd >= 0) {
        close(s->pty_fd);
        s->pty_fd = -1;
    }

    if (s->pid > 0 && s->alive) {
        kill(s->pid, SIGTERM);
        s->alive = 0;
    }

    vterm_free(&s->vt);

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

static void resize_all_ptys(struct ttabmux *t)
{
    int pty_rows = t->term_rows;
    int pty_cols = t->term_cols - t->sidebar_width;
    if (pty_cols < 10) pty_cols = 10;
    if (pty_rows < 2) pty_rows = 2;

    struct winsize ws = {
        .ws_row = (unsigned short)pty_rows,
        .ws_col = (unsigned short)pty_cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    for (int i = 0; i < t->num_sessions; i++) {
        struct session *s = &t->sessions[i];
        if (s->alive && s->pty_fd >= 0) {
            ioctl(s->pty_fd, TIOCSWINSZ, &ws);
            kill(s->pid, SIGWINCH);
        }
        vterm_resize(&s->vt, pty_rows, pty_cols);
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
            if (t->sessions[i].pid == pid) {
                t->sessions[i].alive = 0;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Handle prefix key commands                                        */
/* ------------------------------------------------------------------ */

static void handle_prefix_cmd(struct ttabmux *t, unsigned char ch)
{
    t->prefix_mode = 0;

    switch (ch) {
    case 'c': /* Create new terminal */
        session_create(t, NULL);
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

    case 'x': /* Close current terminal */
        if (t->num_sessions > 0) {
            session_close(t, t->active);
            if (t->num_sessions == 0) {
                t->running = 0;
            }
        }
        break;

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

    case PREFIX_KEY: /* Send literal Ctrl+B to terminal */
        if (t->num_sessions > 0 && t->sessions[t->active].alive) {
            char c = PREFIX_KEY;
            IGNORE_RESULT(write(t->sessions[t->active].pty_fd, &c, 1));
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
/*  Handle user input                                                 */
/* ------------------------------------------------------------------ */

static void handle_input(struct ttabmux *t, const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];

        /* Help overlay: any key dismisses */
        if (t->show_help) {
            t->show_help = 0;
            continue;
        }

        /* Rename mode */
        if (t->rename_mode) {
            handle_rename_input(t, ch);
            continue;
        }

        /* Prefix mode */
        if (t->prefix_mode) {
            handle_prefix_cmd(t, ch);
            continue;
        }

        /* Check for prefix key */
        if (ch == PREFIX_KEY) {
            t->prefix_mode = 1;
            continue;
        }

        /* Forward to active terminal */
        if (t->num_sessions > 0 && t->sessions[t->active].alive) {
            /* Write remaining input directly to PTY */
            int remaining = len - i;
            /* Check if there's a PREFIX_KEY in the remaining data */
            int chunk = 0;
            while (chunk < remaining) {
                if ((unsigned char)data[i + chunk] == PREFIX_KEY)
                    break;
                chunk++;
            }
            if (chunk > 0) {
                IGNORE_RESULT(write(t->sessions[t->active].pty_fd, data + i, (size_t)chunk));
                i += chunk - 1; /* -1 because loop will i++ */
            } else {
                /* PREFIX_KEY at current position, let loop handle it */
                /* Already checked above, shouldn't reach here */
                IGNORE_RESULT(write(t->sessions[t->active].pty_fd, &data[i], 1));
            }
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
            /* Check if all sessions are dead */
            int any_alive = 0;
            for (int i = 0; i < t->num_sessions; i++) {
                if (t->sessions[i].alive) {
                    any_alive = 1;
                    break;
                }
            }
            if (!any_alive && t->num_sessions > 0) {
                /* Clean up dead sessions */
                while (t->num_sessions > 0 && !t->sessions[t->num_sessions - 1].alive)
                    session_close(t, t->num_sessions - 1);
            }
            if (t->num_sessions == 0) {
                t->running = 0;
                break;
            }
            render_screen(t);
        }

        /* Build poll set: stdin + all PTY fds */
        struct pollfd fds[MAX_SESSIONS + 1];
        int nfds = 0;

        fds[nfds].fd = STDIN_FILENO;
        fds[nfds].events = POLLIN;
        nfds++;

        for (int i = 0; i < t->num_sessions; i++) {
            if (t->sessions[i].alive && t->sessions[i].pty_fd >= 0) {
                fds[nfds].fd = t->sessions[i].pty_fd;
                fds[nfds].events = POLLIN;
                nfds++;
            }
        }

        int ret = poll(fds, (nfds_t)nfds, 100 /* ms */);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        int need_render = 0;

        /* Check stdin */
        if (fds[0].revents & POLLIN) {
            int n = (int)read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                handle_input(t, buf, n);
                need_render = 1;
            }
        }

        /* Check PTY outputs */
        int fd_idx = 1;
        for (int i = 0; i < t->num_sessions; i++) {
            if (!t->sessions[i].alive || t->sessions[i].pty_fd < 0)
                continue;

            if (fds[fd_idx].revents & (POLLIN | POLLHUP)) {
                int n = (int)read(t->sessions[i].pty_fd, buf, sizeof(buf));
                if (n > 0) {
                    vterm_process(&t->sessions[i].vt, buf, n);
                    if (i == t->active) need_render = 1;
                } else if (n == 0 || (n < 0 && errno != EAGAIN)) {
                    t->sessions[i].alive = 0;
                    need_render = 1;
                }
            }
            fd_idx++;
        }

        if (need_render) {
            render_screen(t);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Usage                                                             */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -s WIDTH   Sidebar width (default: %d)\n"
        "  -e SHELL   Shell to use (default: $SHELL or /bin/sh)\n"
        "  -h         Show this help\n"
        "\n"
        "Key bindings (prefix: Ctrl+B):\n"
        "  c          Create new terminal\n"
        "  n / p      Next / previous terminal\n"
        "  1-9        Switch to terminal N\n"
        "  x          Close current terminal\n"
        "  ,          Rename current terminal\n"
        "  d          Detach (quit)\n"
        "  ?          Show help overlay\n"
        "  Ctrl+B     Send literal Ctrl+B\n",
        prog, SIDEBAR_WIDTH);
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    const char *shell = NULL;
    int sidebar_w = SIDEBAR_WIDTH;

    int opt;
    while ((opt = getopt(argc, argv, "s:e:h")) != -1) {
        switch (opt) {
        case 's':
            sidebar_w = atoi(optarg);
            if (sidebar_w < 10) sidebar_w = 10;
            if (sidebar_w > 60) sidebar_w = 60;
            break;
        case 'e':
            shell = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* Initialize app state */
    struct ttabmux t;
    memset(&t, 0, sizeof(t));
    t.sidebar_width = sidebar_w;
    t.running = 1;

    /* Get terminal size */
    get_term_size(&t);

    if (t.term_cols < t.sidebar_width + 20) {
        fprintf(stderr, "Terminal too narrow (need at least %d columns)\n",
                t.sidebar_width + 20);
        return 1;
    }

    /* Enter raw mode */
    if (term_raw_mode(&t) < 0) {
        fprintf(stderr, "Failed to set raw mode\n");
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
    /* Clear screen */
    IGNORE_RESULT(write(STDOUT_FILENO, "\033[2J", 4));

    /* Create first terminal session */
    if (session_create(&t, shell) < 0) {
        IGNORE_RESULT(write(STDOUT_FILENO, "\033[?1049l", 8));
        term_restore(&t);
        fprintf(stderr, "Failed to create terminal session\n");
        return 1;
    }

    /* Initial render */
    render_screen(&t);

    /* Run event loop */
    event_loop(&t);

    /* Cleanup */
    for (int i = t.num_sessions - 1; i >= 0; i--)
        session_close(&t, i);

    render_free(&t);

    /* Leave alternate screen buffer */
    IGNORE_RESULT(write(STDOUT_FILENO, "\033[?1049l", 8));

    /* Restore terminal */
    term_restore(&t);

    return 0;
}
