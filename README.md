<p align="center"><img src="Img/logo.png" alt="fbktop" width="500"></p>

# fbktop

**fbktop** is a fork of [btop](https://github.com/aristocratos/btop) (v1.4.8) that adds **network usage per process**: how fast each program is downloading and uploading, right in the process list. Everything else is btop.

## What's added

- **Down / Up columns** in the process list (one combined **Net** column when the box is narrower than 90 columns). Idle programs show a dimmed `0`.
- **`network` sort option**: cycle with `←` / `→`. Programs using the network move to the top.
- **Info box line** (select a process, press `Enter`): download and upload rate plus totals since fbktop started.
- **`proc_net` option** in the options menu (on by default).
- Two fixes backported from btop after v1.4.8: no more logo drawn over the list when only the process box is shown (upstream `00cbab2`), and shorter number formatting (upstream `116d129`).

## How it works

fbktop loads a small eBPF program (code that runs inside the kernel) on the kernel's `sock:sock_send_length` and `sock:sock_recv_length` tracepoints. These fire every time a program sends or receives on a socket, with the number of bytes moved, so TCP and UDP (including browser QUIC traffic and games) are counted. Localhost traffic is skipped so the columns add up to what your network card carries. The counters live only while fbktop runs.

## Requirements

- **Linux 6.3 or newer** with BTF enabled (standard on Debian 13, Ubuntu 24.04, Fedora, Arch). Debian 12 (kernel 6.1) and Ubuntu 22.04 on kernel 5.15 are too old.
- **libbpf** development files to build (`libbpf-dev` on Debian/Ubuntu, `libbpf-devel` on Fedora, `libbpf` on Arch). Without them it still builds, just without the network columns.
- **Permission to load eBPF**: run as root, or give the binary `CAP_BPF` and `CAP_PERFMON` once with `sudo make setcap`.

If any of these is missing, fbktop runs like normal btop with the network columns hidden; the reason is written to `~/.local/state/fbktop.log`.

## Install

```bash
sudo apt install build-essential libbpf-dev      # Debian/Ubuntu
git clone <this repo> fbktop && cd fbktop
make
sudo make install
sudo make setcap
fbktop
```

Rebuilding or reinstalling replaces the binary and drops its permissions, so run `sudo make setcap` again after every `make install`.

Uninstall: `sudo make uninstall`.

## Files

| What | Where |
|---|---|
| Command | `/usr/local/bin/fbktop` |
| Config | `~/.config/fbktop/fbktop.conf` |
| User themes | `~/.config/fbktop/themes/` |
| Log | `~/.local/state/fbktop.log` |
| eBPF source | `src/linux/netio/btop_netio.bpf.c` (rebuild the embedded object with `make netio-bpf`, needs clang + bpftool) |

## Credits and license

Based on [btop](https://github.com/aristocratos/btop) by Jakob P. Liljenberg (aristocratos), licensed under the Apache License 2.0 (see `LICENSE`). Internal file and namespace names are kept from btop so upstream fixes can still be merged.

Files changed from btop v1.4.8: `Makefile`, `CMakeLists.txt`, `README.md`, `manpage.md`, `ansi_banner.utf8`, `fbktop.desktop` (was `btop.desktop`), `Img/icon.*`, `Img/logo.*`, `Img/gen_logo.py` (new), `src/btop.cpp`, `src/btop_cli.cpp`, `src/btop_config.cpp`, `src/btop_draw.cpp`, `src/btop_input.cpp`, `src/btop_log.cpp`, `src/btop_menu.cpp`, `src/btop_shared.cpp`, `src/btop_shared.hpp`, `src/btop_tools.cpp`, `src/btop_tools.hpp`, `src/linux/btop_collect.cpp`, `src/linux/btop_netio.cpp` (new), `src/linux/netio/` (new), `tests/tools.cpp`. Removed: btop's GitHub workflows and funding file.
