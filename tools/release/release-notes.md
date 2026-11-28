Runs SSI's *Fantasy General* (1996, DOS) on Linux and Windows from your own GOG installation, without DOSBox. No game files are included.

**Install the game first** from your GOG.com library (Windows or Linux installer, GOG Galaxy, Heroic or Lutris). Then:

- **Windows** (64-bit Windows 10 1903+ or 11): unpack `fantasy-general-runner-*-windows-x64.zip` and start `fg-runner.exe`.
- **Linux** (64-bit, about 2022 or newer: Ubuntu 22.04+, Debian 12+, Fedora, SteamOS, ...): unpack `fantasy-general-runner-*-linux-x86_64.tar.gz` and start `./fg-runner`; `./install-desktop-entry.sh` adds a menu entry.

Started this way, the runner opens a launcher: it shows where the game is (found by itself, or chosen once with Browse) and its settings (display, music volume, sound, scrolling, the computer's speed); Launch starts the game and remembers them. Started with command-line options, it skips the launcher. In the game, Alt+Enter switches between fullscreen and a window. The `README.txt` in each package lists keys and options; problems are logged to `fg-runner.log` in `%APPDATA%\fg-runner\` or `~/.local/share/fg-runner/`.

**New in 0.7.0**

- A CRT filter: DOSBox Staging's VGA monitor shaders, chosen by screen size like DOSBox Staging does (1080p, 1440p, 4K). Turn it on in the launcher (Display), with `--crt`, or with Ctrl+F9 while playing. It needs Vulkan graphics drivers.
- Fixed: on Windows the game could stop with an error message when you quit it.
- The runner is a 64-bit program only (the 32-bit Linux variant is gone).

`SHA256SUMS-*.txt` holds the packages' checksums. Licence: GPL-2.0-or-later; the packages contain Unicorn (GPL-2.0), SDL3 (zlib) and the fonts Inter and Metamorphous (SIL Open Font License 1.1) and DOSBox Staging's CRT shaders (MIT).
