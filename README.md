# Ttabmux

A lightweight terminal multiplexer with a sidebar for session navigation, written in C99 (~4700 LOC).

## Features

- Sidebar showing named sessions with mouse support
- Split panes — vertical and horizontal, drag to resize
- Scrollback buffer (up to 100K lines) with search
- Wide mode for horizontal scrolling (e.g. wide log output)
- INI config file for persistent sessions and settings
- Mouse tracking with click, drag, and hover support
- True color and 256-color support

## Build

```sh
make              # Release binary → ./ttabmux
make debug        # Debug build (logs to /tmp/ttabmux_debug.log)
make clean
make install      # Install to /usr/local/bin (PREFIX= to override)
```

Requires: C99 compiler, POSIX PTY (`libutil` on Linux).

## Usage

```sh
./ttabmux                          # Start with default shell
./ttabmux htop                     # Start running htop
./ttabmux -s 30                    # Custom sidebar width
./ttabmux -e /bin/zsh              # Custom default shell
./ttabmux -S dev -V vim -H make    # Named session with splits
./ttabmux -c ~/.config/ttabmux/config.ini  # Explicit config file
```

## Options

| Flag | Description |
|------|-------------|
| `-c, --config FILE` | Load INI config file (default: `~/.config/ttabmux/config.ini`) |
| `-s WIDTH` | Sidebar width (default: 25, range 25–60) |
| `-e SHELL` | Default shell for new tabs |
| `-S, --session NAME` | Create a named session |
| `-V, --vsplit CMD` | Vertical split, run CMD |
| `-H, --hsplit CMD` | Horizontal split, run CMD |
| `-r, --rename NAME` | Rename the last created session |
| `-w, --wide COLS` | Wide mode: set PTY column count |
| `-t, --title TITLE` | Sidebar header title (default: `Ttabmux`) |
| `-f, --fg COLOR` | Title foreground color (name or 0–255) |
| `-b, --bg COLOR` | Title background color (name or 0–255) |
| `-M, --no-hover` | Disable hover highlights (recommended over SSH) |
| `-h, --help` | Show help |

`-S`, `-V`, `-H`, `-r`, `-w` are processed left-to-right to build the initial layout. Positional args each create a session.

## Config file

Persistent settings and sessions can be stored in `~/.config/ttabmux/config.ini` (respects `$XDG_CONFIG_HOME`). CLI flags override config values. If any session-related CLI flag or positional arg is given, config sessions are ignored.

```ini
[settings]
sidebar = 30
shell = /bin/zsh
title = Dev
fg = bright-white
bg = 24
no_hover = true

# Sessions: 'command' must appear before vsplit/hsplit/wide
[session:editor]
command = vim

[session:shell]
command = /bin/zsh
vsplit = htop
hsplit = tail -f /var/log/syslog
wide = 200

[session:build]
command = make
```

Use `-c FILE` to load a different config path. If the file doesn't exist when specified with `-c`, ttabmux exits with an error. Auto-detected paths are silently skipped if missing.

## Key bindings

Prefix key: `Ctrl+B`

| Keys | Action |
|------|--------|
| `c` | New terminal |
| `n` / `p` | Next / previous session |
| `1`–`9` | Switch to session N |
| `x` | Close current pane (or session) |
| `v` | Vertical split |
| `s` | Horizontal split |
| `o` | Cycle pane focus |
| Arrow keys | Navigate panes |
| `,` | Rename current session |
| `/` | Search scrollback |
| `g` | Jump to line |
| `d` | Detach (quit) |
| `?` | Help overlay |
| `Ctrl+B` | Send literal Ctrl+B |
| ESC ×10 | Quick quit |

Mouse: click to switch sessions/panes, drag sidebar border or split dividers to resize, scroll to navigate scrollback.

## License

MIT © Mickael Blet
