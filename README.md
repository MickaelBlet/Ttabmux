# Ttabmux

A terminal multiplexer with a vertical sidebar for switching between multiple terminal sessions.

```
┌──────────┬──────────────────────────────────┐
│ Ttabmux  │                                  │
│──────────│  user@host:~$                    │
│> 1: bash │  ls -la                          │
│  2: vim  │  total 42                        │
│  3: htop │  drwxr-xr-x  5 user user 4096   │
│          │  -rw-r--r--  1 user user 1234   │
│          │                                  │
│          │                                  │
│──────────│                                  │
│ ^B ? help│                                  │
└──────────┴──────────────────────────────────┘
```

## Features

- **Sidebar navigation** - Visual sidebar showing all terminal sessions
- **Multiple terminals** - Up to 10 concurrent terminal sessions
- **VT100/xterm emulation** - Supports colors (256 + true color), cursor movement, scrolling, alternate screen buffer
- **Unicode support** - UTF-8 and wide character handling
- **Lightweight** - Pure C, minimal dependencies (only `libutil` for PTY)

## Building

```sh
make
```

### Install system-wide (optional)

```sh
sudo make install
```

## Usage

```sh
./ttabmux [options]
```

### Options

| Option | Description |
|--------|-------------|
| `-s WIDTH` | Sidebar width (default: 20, range: 10-60) |
| `-e SHELL` | Shell to use (default: `$SHELL` or `/bin/sh`) |
| `-h` | Show help |

## Key Bindings

All commands use the **Ctrl+B** prefix key.

| Key | Action |
|-----|--------|
| `Ctrl+B` `c` | Create a new terminal |
| `Ctrl+B` `n` | Switch to the next terminal |
| `Ctrl+B` `p` | Switch to the previous terminal |
| `Ctrl+B` `1-9` | Switch to terminal N |
| `Ctrl+B` `x` | Close the current terminal |
| `Ctrl+B` `,` | Rename the current terminal |
| `Ctrl+B` `d` | Detach (quit) |
| `Ctrl+B` `?` | Toggle help overlay |
| `Ctrl+B` `Ctrl+B` | Send literal Ctrl+B to the terminal |

## Requirements

- Linux (uses `forkpty` from `libutil`)
- C99 compiler (gcc or clang)
- POSIX environment

## License

MIT License - see [LICENSE](LICENSE) for details.
