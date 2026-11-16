---
name: Bug report
about: Create a report to help us improve
labels: bug

---

**Read the README.md and search for similar issues before posting a bug report!**

<!-- Bugs in parts fbktop didn't change (CPU, memory, disk and network boxes) are likely btop bugs too: check https://github.com/aristocratos/btop/issues first. -->

**Describe the bug**

<!-- A clear and concise description of what the bug is. -->

**To Reproduce**

<!-- Steps to reproduce the behavior. -->

**Expected behavior**

<!-- A clear and concise description of what you expected to happen. -->

**Screenshots**

<!-- If applicable, add screenshots to help explain your problem. -->

**Info (please complete the following information):**
 - fbktop version: `fbktop --version`
 - Capabilities: `getcap $(which fbktop)`
 - Architecture: [x86_64, aarch64, etc.] `uname -m`
 - Kernel: `uname -r`
 - Distribution and libbpf version:
 - GPU and driver (for the Gpu% column):
 - Terminal used:
 - Font used:

**Additional context**

<!-- Contents of `~/.local/state/fbktop.log` (set log_level = "DEBUG" in ~/.config/fbktop/fbktop.conf or run `fbktop --debug` if it is empty) -->

**GDB Backtrace**

<!-- If fbktop is crashing the following steps could be helpful: -->

<!-- (Extra helpful if compiled with `make DEBUG=true`) -->

<!-- 1. run `gdb fbktop` -->

<!-- 2. `r` to run, wait for crash and press enter if prompted, CTRL+L to clear screen if needed. -->

<!-- 3. `thread apply all bt` to get a backtrace for all threads -->

<!-- 4. Copy and paste the backtrace here: -->
