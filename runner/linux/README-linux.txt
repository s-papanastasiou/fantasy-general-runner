Fantasy General runner for Linux
================================

Runs SSI's Fantasy General (1996, DOS) from your own GOG installation,
without DOSBox. No game files are included.

Requirements: a 64-bit Linux desktop from about 2022 on (Ubuntu 22.04,
Debian 12, Fedora, SteamOS or similar), and the GOG version of Fantasy
General installed.

Getting the game
----------------
Install Fantasy General from your GOG.com library: either GOG's Linux
installer (fantasy_general_*.sh, run it with "sh fantasy_general_*.sh";
default ~/GOG Games/Fantasy General), the Windows installer through
Heroic or Lutris, or by copying an installed game folder.

Starting
--------
Unpack this archive anywhere and run ./fg-runner (or double-click it). The
launcher opens: it shows where the game is and its settings (display,
music volume, sound, scrolling and the computer's speed), each with the
command-line option it stands for. Launch starts the game with them and
remembers them (~/.local/share/fg-runner/settings.ini); Quit or Escape
closes it. Started with any option, the runner skips the launcher and its
settings and starts the game directly (--launcher shows it anyway).

The game starts fullscreen unless you choose a window; Alt+Enter switches
while playing. The runner finds the game by itself in the usual places
(GOG's installer, Heroic, Lutris, Wine). If it cannot, choose the folder
the game was installed to with Browse in the launcher (without the
launcher, the runner asks for it); the choice is remembered
(~/.local/share/fg-runner/game-dir.txt; delete it to choose again). To
pick the folder on the command line instead:

    ./fg-runner --game-dir "$HOME/Games/Fantasy General"

To add Fantasy General to your applications menu (with an icon), run
./install-desktop-entry.sh once; ./install-desktop-entry.sh --remove takes
it out again. Run it again if you move this folder.

Saved games go where GOG's own setup puts them: the game's SAVES folder,
or ~/.local/share/fg-runner/drive-c if the game folder is read-only. On the
first start the runner unpacks the game CD's data track (game.gog) into
~/.local/share/fg-runner/cd; later starts reuse it.

If something goes wrong, the runner shows a message and writes details to
~/.local/share/fg-runner/fg-runner.log.

Keys
----
Alt+Enter   fullscreen on/off
Ctrl+F10    release / capture the mouse (a click into the game captures it)
Ctrl+F11    integer / fit scaling
F12         screenshot (fg-screenshot.png)
Arrow keys  scroll the battle map (two at once: diagonally)

Options
-------
--window               start in a window instead of fullscreen
--scale N              window size (640x480 x N)
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
--game-dir DIR         the game's folder (skips the search)
--save-dir DIR         keep saved games in DIR
--launcher             show the launcher (the default with no options)
--version              print the version

Licence
-------
GPL-2.0-or-later; see LICENSE.txt. This program contains the Unicorn CPU
emulator 2.1.4 (GPL-2.0, LICENSE-Unicorn.txt) and SDL 3.4 (zlib licence,
LICENSE-SDL3.txt, lib/libSDL3.so.0), and the launcher's fonts Inter and
Metamorphous (SIL Open Font License 1.1, OFL-Inter.txt and
OFL-Metamorphous.txt). The complete source code of this
program is available from whoever gave you this copy, as the GPL requires.
