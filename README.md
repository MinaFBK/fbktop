<p align="center"><img src="Img/logo.png" alt="fbktop" width="500"></p>

# fbktop

**fbktop** is a fork of [btop](https://github.com/aristocratos/btop) (v1.4.8) that adds **network and GPU usage per process**: how fast each program is downloading and uploading and how busy it keeps the GPU, right in the process list. Everything else is btop.

## What's added

- **Down / Up columns** in the process list (one combined **Net** column when the box is narrower than 90 columns). Idle programs show a dimmed `0`.
- **Gpu% column**: how busy each program keeps the GPU, as the busiest GPU engine (3D, compute, video) since the last update, like the GPU column in Windows Task Manager. Shown when the box is at least 70 columns wide.
- **`network` and `gpu` sort options**: cycle with `←` / `→`.
- **Clickable column headers**: the header row is drawn as a bar of buttons. Click a column name (Pid, Program, Command, Threads, User, Down/Up, Gpu%, Mem, Cpu%) to sort by it; click it again to reverse. The sorted column is tinted and shows `▼` (biggest or Z first) or `▲` (smallest or A first). Bar colors are mixed from the theme; tty mode shows plain labels with `v` / `^`.
- **Case-insensitive text sorts**: Program, Command and User sort as `alacritty`, `Bash`, `cron` instead of capitals first.
- **Startup sort stays put**: the sort and reverse set in the config file or options menu are what fbktop starts with. Changing them with the keys or the mouse only lasts until you quit.
- **Info box line** (select a process, press `Enter`): download and upload rate plus totals since fbktop started.
- **`proc_net` and `proc_gpu` options** in the options menu (both on by default).
- Two fixes backported from btop after v1.4.8: no more logo drawn over the list when only the process box is shown (upstream `00cbab2`), and shorter number formatting (upstream `116d129`).

## How it works

fbktop loads a small eBPF program (code that runs inside the kernel) on the kernel's `sock:sock_send_length` and `sock:sock_recv_length` tracepoints. These fire every time a program sends or receives on a socket, with the number of bytes moved, so TCP and UDP (including browser QUIC traffic and games) are counted. Localhost traffic is skipped so the columns add up to what your network card carries. The counters live only while fbktop runs.

GPU usage comes from the kernel's DRM fdinfo files (`/proc/<pid>/fdinfo/<fd>`), where amdgpu, i915 and other drivers report how long each program's GPU work ran on each engine. Finding a program's GPU handles means looking at every file it has open, so fbktop does that once when a program appears and afterwards only re-checks programs that used CPU time recently (a program can't send work to the GPU without running). Each update then reads one small file per GPU client. Programs of other users (for example a root Xorg) show 0 unless fbktop runs as root.

## Requirements

- **Linux 6.3 or newer** with BTF enabled (standard on Debian 13, Ubuntu 24.04, Fedora, Arch). Debian 12 (kernel 6.1) and Ubuntu 22.04 on kernel 5.15 are too old.
- **libbpf** development files to build (`libbpf-dev` on Debian/Ubuntu, `libbpf-devel` on Fedora, `libbpf` on Arch). Without them it still builds, just without the network columns.
- **Permission to load eBPF**: run as root, or give the binary its permissions once with `sudo make setcap` (`CAP_BPF` for the network counters, `CAP_PERFMON` for Intel GPU stats, `CAP_DAC_READ_SEARCH` for the root-only CPU power counter, as in btop).

On a PC shared by several user accounts, note that anyone who runs fbktop sees the network rates of every program, including other users' programs (Linux normally keeps per-process I/O counts private to their owner). The Gpu% column only covers your own programs.

If any of these is missing, fbktop runs like normal btop with the network columns hidden; the reason is written to `~/.local/state/fbktop.log`. The Gpu% column needs no extra permission or library; it appears once a GPU driver that reports engine usage is found: amdgpu, i915, xe, msm, panfrost, v3d, and panthor with its profiling switch on. nouveau and NVIDIA's proprietary driver don't report it. On PCs with two GPUs each GPU's engines are kept apart, so a process shows its busier GPU.

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

How the permissions are kept safe:
- fbktop uses `CAP_BPF` only to attach the network counters and `CAP_DAC_READ_SEARCH` only to open the CPU power counter, both at startup before it reads any config, theme or input, then gives them up. `CAP_PERFMON` is given up once the GPU box is set up (Intel GPU stats need it then). After that the process holds no capabilities, so a bug later on cannot be used to gain them. Because of this, turning `proc_net` on in the options menu takes effect at the next start.
- `make install` copies with `install`, so under `sudo` every installed file is owned by root and writable only by root. `make setcap` refuses to run on a binary that anyone else could change.
- If an older fbktop was installed with `cp -p` (files owned by your user), fix the leftover folders once: `sudo chown -R root:root /usr/local/share/fbktop /usr/local/share/doc/fbktop && sudo chmod -R go-w /usr/local/share/fbktop /usr/local/share/doc/fbktop`, then install again.

Uninstall: `sudo make uninstall`.

## Files

| What | Where |
|---|---|
| Command | `/usr/local/bin/fbktop` |
| Config | `~/.config/fbktop/fbktop.conf` |
| User themes | `~/.config/fbktop/themes/` |
| Log | `~/.local/state/fbktop.log` |
| eBPF source | `src/linux/netio/btop_netio.bpf.c` (rebuild the embedded object with `make netio-bpf`, needs clang + bpftool) |
| Maintainer notes | `docs/dev-notes.md` |

## Credits and license

Based on [btop](https://github.com/aristocratos/btop) by Jakob P. Liljenberg (aristocratos), licensed under the Apache License 2.0 (see `LICENSE`). Internal file and namespace names are kept from btop so upstream fixes can still be merged.

Files changed from btop v1.4.8: `Makefile`, `CMakeLists.txt`, `README.md`, `manpage.md`, `ansi_banner.utf8`, `fbktop.desktop` (was `btop.desktop`), `Img/icon.*`, `Img/logo.*`, `Img/gen_logo.py` (new), `src/btop.cpp`, `src/btop_cli.cpp`, `src/btop_config.cpp`, `src/btop_config.hpp`, `src/btop_draw.cpp`, `src/btop_input.cpp`, `src/btop_log.cpp`, `src/btop_menu.cpp`, `src/btop_shared.cpp`, `src/btop_shared.hpp`, `src/btop_tools.cpp`, `src/btop_tools.hpp`, `src/linux/btop_collect.cpp`, `src/linux/btop_gpuproc.cpp` (new), `src/linux/btop_netio.cpp` (new), `src/linux/netio/` (new), `tests/tools.cpp`. Removed: btop's GitHub workflows, CI path filters, Renovate config, funding file and snap recipe. `.github/ISSUE_TEMPLATE/bug_report.md` asks for fbktop's own details.
