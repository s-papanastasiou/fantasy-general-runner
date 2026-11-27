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
Double-click fg-runner.exe. The game starts fullscreen; Alt+Enter switches
to a window and back (--window starts in a window). It finds the game by
itself through the registry entry GOG's installer writes, next to
fg-runner.exe, one folder up, and in the usual GOG folders. If it cannot,
it asks you for the folder the game was installed to and remembers your
choice (%APPDATA%\fg-runner\game-dir.txt; delete it to choose again). To
pick the folder from a command prompt instead:

    fg-runner.exe --game-dir "D:\Games\Fantasy General"

Saved games go to the cloud_saves folder inside the game's install
folder, as with GOG's DOSBox version (so Galaxy's cloud saves and your
existing saves keep working). If that folder is missing or cannot be
written to, they go to %APPDATA%\fg-runner\drive-c instead.

On the first start it unpacks the game CD's data track (game.gog) into
%APPDATA%\fg-runner\cd; later starts reuse it. The console window shows
the runner's messages.

Keys
----
Alt+Enter   fullscreen on/off
Ctrl+F10    release / capture the mouse (a click into the game captures it)
F12         screenshot (fg-screenshot.png)
Arrow keys  scroll the battle map (two at once: diagonally)

Options
-------
--scale N              initial window size (640x480 x N)
--window               start in a window instead of fullscreen
--scaling integer|fit  exact pixels (default) or fill the window
--scroll-ms MS         battle-map scroll speed (default 120)
--original-scrolling   map edges scroll as in the original game (the
                       sides diagonally); default: sides straight,
                       corners diagonal
--ai-move-ms MS        speed of the computer's unit moves (default 10;
                       higher is slower, 0 = as fast as possible)
--ai-view-ms MS        on the computer's turn, how long each map view
                       stays on screen at least (default 200)
--mute                 no audio
--no-sound             no sound effects
--cd-dir DIR           use an already extracted CD data track as drive D:
--save-dir DIR         keep saved games in DIR

Licence
-------
GPL-2.0-or-later; see LICENSE.txt. This program contains the Unicorn CPU
emulator 2.1.4 (GPL-2.0, https://github.com/unicorn-engine/unicorn) and uses
SDL3 (zlib licence, https://libsdl.org). The complete source code of this
program is available from whoever gave you this copy, as the GPL requires.
