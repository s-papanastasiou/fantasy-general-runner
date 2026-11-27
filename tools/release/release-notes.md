Runs SSI's *Fantasy General* (1996, DOS) on Linux and Windows from your own GOG installation, without DOSBox. No game files are included.

**Install the game first** from your GOG.com library (Windows or Linux installer, GOG Galaxy, Heroic or Lutris). Then:

- **Windows** (64-bit Windows 10 1903+ or 11): unpack `fantasy-general-runner-*-windows-x64.zip` and start `fg-runner.exe`.
- **Linux** (64-bit, about 2022 or newer: Ubuntu 22.04+, Debian 12+, Fedora, SteamOS, ...): unpack `fantasy-general-runner-*-linux-x86_64.tar.gz` and start `./fg-runner`; `./install-desktop-entry.sh` adds a menu entry.

The runner finds the game by itself, or asks for its folder once. It starts fullscreen (Alt+Enter for a window). The `README.txt` in each package lists keys and options; problems are logged to `fg-runner.log` in `%APPDATA%\fg-runner\` or `~/.local/share/fg-runner/`.

`SHA256SUMS-*.txt` holds the packages' checksums. Licence: GPL-2.0-or-later; the packages contain Unicorn (GPL-2.0) and SDL3 (zlib).
