# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
make              # Build release binary (./ttabmux)
make debug        # Build with debug logging (-DTTABMUX_DEBUG), logs to /tmp/ttabmux_debug.log
make clean        # Remove build artifacts
make install      # Install to /usr/local/bin (override with PREFIX=)
```

No automated tests exist. Verify changes by building and running manually.

## Running

```bash
./ttabmux                        # Start with default shell
./ttabmux htop                   # Start with specific program
./ttabmux -s 30                  # Custom sidebar width
./ttabmux -e /bin/zsh            # Custom default shell
./ttabmux -S dev -V vim -H make  # Named session with splits
```

## Architecture

Single-threaded C99 terminal multiplexer (~4700 LOC) with four source files:

- **src/main.c** (~2644 lines) — Entry point, `poll()`-based event loop, input handling (keyboard + mouse), PTY management via `forkpty()`, layout tree operations (split/close/resize), signal handlers (SIGWINCH, SIGCHLD), CLI argument parsing.
- **src/vterm.c** (~1085 lines) — Virtual terminal emulator. State machine parser (VT_NORMAL → VT_ESC → VT_CSI/VT_OSC/VT_DCS) for ANSI/VT100/xterm escape sequences. Manages main + alternate screen buffers, scrollback ring buffer (100K lines), UTF-8 decoding, 256-color + true color, mouse tracking modes.
- **src/render.c** (~1004 lines) — Buffered screen rendering. Draws sidebar (session list, buttons, scrollbar) and content area (panes with dividers, line numbers, scrollbars). Accumulates output in a dynamic buffer, writes in a single `write()` call per frame.
- **src/ttabmux.h** — All struct definitions and constants. Key structures: `cell` (terminal cell), `vterm` (terminal emulator state), `pane` (PTY + vterm), `layout_node` (array-based binary tree for splits), `session` (tab with up to 16 panes), `ttabmux` (top-level app state with up to 100 sessions).
- **src/debug.c/h** — Optional debug logging, compiled only with `-DTTABMUX_DEBUG`.

### Key Patterns

- **Event loop**: `poll()` multiplexes stdin and all PTY fds with 100ms timeout.
- **Layout tree**: Array-based binary tree (not pointer-based) in `layout_node[]`. Each node is a leaf (pane index) or a split (vertical/horizontal) with a ratio. Recursive reflow recalculates positions on resize/split/close.
- **Scrollback**: Ring buffer (`scrollback[]` in vterm) with `sb_head`/`sb_count` tracking.
- **Buffered rendering**: Dynamic output buffer (starts 64KB) accumulated via `render_*` functions, flushed once per frame.
- **Modal input**: Normal → prefix mode (Ctrl+B) → command. Also rename mode, search mode (/), jump-to-line mode (g).
- **Mouse translation**: Parses SGR extended mouse events from host terminal, maps screen coords to pane-local coords, forwards to child processes that request mouse tracking.
- **Platform guards**: `#if defined(__linux__)` / `#if defined(__APPLE__)` for PTY headers (`<pty.h>` vs `<util.h>`).

### Limits (defined in ttabmux.h)

- `MAX_SESSIONS 100`, `MAX_PANES 16` per session, `SCROLLBACK_MAX 100000` lines
- Prefix key: `Ctrl+B` (`0x02`)
