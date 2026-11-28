fg-runner for Windows
=====================

Runs SSI's Fantasy General (1996, DOS) from your own GOG installation,
without DOSBox. No game files are included.

Requirements: 64-bit Windows 10 (1903 or later) or Windows 11, and the GOG
version of Fantasy General installed.

Getting the game
----------------
The game must be installed: fg-runner reads the files the GOG installer
puts on disk. Download the offline installer (setup_fantasy_general_*.exe)
from your GOG.com library and run it, or install the game with GOG Galaxy.
Any folder will do; the default is C:\GOG Games\Fantasy General.

Starting
--------
Double-click fg-runner.exe. The launcher opens: it shows where the game
is and its settings (display, music volume, sound, scrolling and the
computer's speed), each with the command-line option it stands for.
Launch starts the game with them and remembers them
(%APPDATA%\fg-runner\settings.ini); Quit or Escape closes it. Started
from a command prompt with any option, the runner skips the launcher and
its settings and starts the game directly (--launcher shows it anyway).

The game starts fullscreen unless you choose a window; Alt+Enter switches
while playing. It finds the game by itself through the registry entry
GOG's installer writes, next to fg-runner.exe, one folder up, and in the
usual GOG folders. If it cannot, choose the folder the game was installed
to with Browse in the launcher (without the launcher, the runner asks for
it); the choice is remembered (%APPDATA%\fg-runner\game-dir.txt; delete
it to choose again). To pick the folder from a command prompt instead:

    fg-runner.exe --game-dir "D:\Games\Fantasy General"

Saved games go to the cloud_saves folder inside the game's install
folder, as with GOG's DOSBox version (so Galaxy's cloud saves and your
existing saves keep working). If that folder is missing or cannot be
written to, they go to %APPDATA%\fg-runner\drive-c instead.

On the first start it unpacks the game CD's data track (game.gog) into
%APPDATA%\fg-runner\cd; later starts reuse it.

If something goes wrong, the runner shows a message and writes details to
%APPDATA%\fg-runner\fg-runner.log. Started from a command prompt, it also
prints its messages there.

Keys
----
Alt+Enter   fullscreen on/off
Ctrl+F10    release / capture the mouse (a click into the game captures it)
Ctrl+F11    integer / fit scaling
Ctrl+F9     CRT filter on / off
F12         screenshot (fg-screenshot.png)
Arrow keys  scroll the battle map (two at once: diagonally)

Options
-------
--scale N              initial window size (640x480 x N)
--window               start in a window instead of fullscreen
--crt                  CRT filter: DOSBox Staging's VGA monitor look (needs
                       Vulkan; best at 1440p or 4K, at least 1080p)
--scaling integer|fit  exact pixels (default) or fill the window
--scroll-ms MS         battle-map scroll speed (default 120)
--original-scrolling   map edges scroll as in the original game (the
                       sides diagonally); default: sides straight,
                       corners diagonal
--ai-move-ms MS        speed of the computer's unit moves (default 10;
                       higher is slower, 0 = as fast as possible)
--ai-view-ms MS        on the computer's turn, how long each map view
                       stays on screen at least (default 200)
--music-volume P       music volume in percent, 10-100 (default 100); to
                       silence it, switch music off in the game
--mute                 no audio
--no-sound             no sound effects
--cd-dir DIR           use an already extracted CD data track as drive D:
--save-dir DIR         keep saved games in DIR
--launcher             show the launcher (the default with no options)
--version              print the version (from a command prompt)

Licence
-------
GPL-2.0-or-later; see LICENSE.txt. This program contains the Unicorn CPU
emulator 2.1.4 (GPL-2.0, https://github.com/unicorn-engine/unicorn) and uses
SDL3 (zlib licence, https://libsdl.org). It contains the launcher's fonts
Inter and Metamorphous (SIL Open Font License 1.1, OFL-Inter.txt and
OFL-Metamorphous.txt), and DOSBox Staging's CRT shaders (MIT,
LICENSE-shaders.txt). The complete source code of this
program is available from whoever gave you this copy, as the GPL requires.
