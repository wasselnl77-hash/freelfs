# 5. Keyboard shortcuts

> Every shortcut in this chapter was checked against the source code (`seat.c` in
> the compositor, the key handlers of each app) or the README of the app. Where
> a README and the code disagree, the code is right and the difference is noted.

## Contents

- [How to read the tables](#how-to-read-the-tables)
- [Desktop (flcomp)](#desktop-flcomp)
- [Media keys](#media-keys)
- [While the screen is locked](#while-the-screen-is-locked)
- [Start menu and panel](#start-menu-and-panel)
- [Workspace overview](#workspace-overview)
- [Clipboard history](#clipboard-history)
- [Files](#files)
- [Terminal and shell](#terminal-and-shell)
- [Text Editor](#text-editor)
- [Images](#images)
- [Videos](#videos)
- [Music](#music)
- [Documents](#documents)
- [Archives](#archives)
- [General Manager](#general-manager)
- [Snappel](#snappel)
- [FreeBWS (web browser)](#freebws-web-browser)
- [Settings and Themes](#settings-and-themes)
- [Discrepancies](#discrepancies)

## How to read the tables

- **Super** is the Windows-logo key (or the Command key on some keyboards).
- **Ctrl**, **Alt** and **Shift** are the usual modifiers. Keys joined with `+` are
  pressed together. A `/` between two keys means either one.
- Arrow keys are the four cursor keys.
- "Hold" means keep the key down.

## Desktop (flcomp)

These work anywhere on the desktop. The compositor takes them before the program
you are using sees them.

| Keys | What it does |
|---|---|
| Super (tap it on its own) | Opens or closes the start menu |
| Alt+Tab | Next window (hold Alt). A row of pictures shows the choice |
| Alt+Shift+Tab | Previous window |
| Super+Left | Snap the window to the left half. On a window snapped to the right, it goes back to its old place |
| Super+Right | Snap the window to the right half. On a window snapped to the left, it goes back to its old place |
| Super+Up | Maximize the window |
| Super+Down | Restore a maximized or snapped window, or minimize it |
| Super+E | Open Files |
| Super+T | Open the Terminal |
| Ctrl+Alt+T | Open the Terminal |
| Super+L | Lock the screen (only where the lock program is installed) |
| Super+Ctrl+Left / Right / Up / Down | Switch to the workspace next to this one (no wrap-around) |
| Super+Shift+Left / Right / Up / Down | Move the focused window to the next workspace. The view follows |
| Super+Tab | Show or hide the workspace overview |
| Print | Screenshot of the whole screen, saved to `~/pictures/screenshots` (see Snappel) |
| Shift+Print | Screenshot of a region: select an area (Snappel) |
| Super+Shift+S | Open the Snappel window |
| Ctrl+Shift+Esc | Open General Manager (the task manager) |
| Ctrl+Alt+F1 to F12 | Switch to a text console |
| Ctrl+Alt+Backspace | End the desktop session at once. Unsaved work is lost |

Notes:

- Super+Ctrl and Super+Shift are told apart by the combination: Super+Ctrl switches
  the view, Super+Shift moves the window.
- Pressing the opposite snap key on a snapped window puts it back to its old place.
- Ctrl+Alt+Backspace has no confirmation. It is the way out of a stuck desktop.
- On the login screen, Ctrl+Alt+T, Super+T, Super+E and Ctrl+Shift+Esc do nothing.

## Media keys

Keys on the laptop or on the keyboard. The indicator (a small box on screen) shows
what changed.

| Key | What it does |
|---|---|
| Volume up / down | Raises or lowers the volume by 5 %. Hold the key to repeat |
| Mute | Mutes or unmutes the sound |
| Microphone mute | Mutes or unmutes the microphone |
| Brightness up / down | Changes the screen brightness in steps of 5 %, never below 5 %. Hold to repeat |
| Play / pause | Plays or pauses the music player (Music) |
| Stop | Stops the music player |
| Next track / previous track | Moves to the next or previous track in Music |

## While the screen is locked

Only these keys do anything while the screen is locked:

| Keys | What it does |
|---|---|
| Ctrl+Alt+F1 to F12 | Switch to a text console |
| Volume, mute, brightness, microphone keys | Work as above |
| Enter (on the "lock program is not running" screen) | Starts the lock program again |

Typing your password is the normal lock-screen action. Esc or Ctrl+U clears the
password field.

## Start menu and panel

| Keys | What it does |
|---|---|
| Super | Opens or closes the start menu |
| Type | Searches the programs in the open category |
| Up / Down | Moves the selection in the list |
| Enter | Starts the selected program |
| Esc | Closes the start menu or a flyout (volume, battery, notification centre, night light) |
| Left, Down / Right, Up, in the volume bar | Lower, or raise, the volume by 5 %. Page Down and Page Up change it by 10 %. Home and End go to the ends. M mutes |
| Esc in a dialog | Cancels the dialog (for example the power countdown) |

## Workspace overview

These work while the overview (Super+Tab) is open.

| Keys | What it does |
|---|---|
| Super+Tab | Shows or hides the overview |
| Tab, arrow keys | Move the focus between workspaces and windows |
| 1 to 9 | Go to that numbered workspace |
| Enter | Go to the focused workspace or window |
| Esc | Close the overview |

## Clipboard history

| Keys | What it does |
|---|---|
| Ctrl+V, held for about half a second | Opens the list of the last 20 pieces of copied text |
| Up / Down, Enter | Pick an entry and paste it |
| Esc | Closes the list |
| Ctrl+V, pressed and let go quickly | Pastes the newest text (as usual) |

## Files

The Files app (`flfiles`), taken from its code and README.

| Keys | What it does |
|---|---|
| Enter | Open the selected item (a folder opens in place, a file opens in its default app) |
| Backspace, Alt+Up | Go up one folder |
| Alt+Left, Alt+Right | Back and forward through the history |
| Ctrl+L | Edit the path field |
| Ctrl+H | Show or hide hidden files |
| Ctrl+A | Select all |
| Ctrl+C, Ctrl+X, Ctrl+V | Copy, cut, paste |
| Ctrl+Shift+N | New folder |
| F2 | Rename |
| F5 | Refresh |
| Delete | Move to the Trash |
| Shift+Delete | Delete for good (asks first) |
| Menu key | Open the context menu |
| Ctrl+click, Shift+click | Select several items |
| Letters | Jump to a name starting with that letter |

## Terminal and shell

The Terminal (`flterm`) runs the shell `flsh`.

| Keys | What it does |
|---|---|
| Ctrl+Shift+C or Ctrl+Insert | Copy the selection |
| Ctrl+Shift+V, Shift+Insert | Paste |
| Shift+Page Up, Shift+Page Down | Scroll the history (2000 lines) |
| Mouse wheel | Scrolls the history. On a full-screen program (such as `less`) it sends the arrow keys |
| Double click, triple click | Select a word, select a line |
| Middle click | Paste |

The shell (`flsh`):

| Keys | What it does |
|---|---|
| Ctrl+A, Ctrl+E | Go to the start, or the end, of the line |
| Ctrl+B, Ctrl+F or arrows | Move one character |
| Alt+B, Alt+F | Move one word |
| Ctrl+W | Delete the word before the cursor |
| Ctrl+U | Delete to the start of the line |
| Ctrl+K | Delete to the end of the line |
| Ctrl+Y | Paste what was deleted last |
| Ctrl+T | Swap the two characters before the cursor |
| Ctrl+L | Clear the screen |
| Ctrl+C | Cancel the line |
| Ctrl+D | Leave the shell (on an empty line) |
| Ctrl+R | Search the history (anywhere in it) |
| Up / Down | Search the history for lines that start with what you typed |
| Right, or End | Accept the grey suggestion. Ctrl+Right or Alt+F takes one word of it |
| Alt+. | Insert the last argument of the previous command |
| Tab | Complete a command, a file name, a variable or an option. When several fit, a menu opens: Tab moves, Enter picks, Esc cancels |

## Text Editor

The editor (`fledit`).

| Keys | What it does |
|---|---|
| Ctrl+N or Ctrl+T | New tab |
| Ctrl+O | Open a file (in a new tab) |
| Ctrl+S | Save |
| Ctrl+Shift+S | Save as |
| Ctrl+W | Close the tab |
| Ctrl+Q | Quit (asks about every tab with changes) |
| Ctrl+Tab, Ctrl+PgDn | Next tab |
| Ctrl+Shift+Tab, Ctrl+PgUp | Previous tab |
| Alt+1 to Alt+9 | Go to tab 1 to 9 |
| Ctrl+Z | Undo |
| Ctrl+Y or Ctrl+Shift+Z | Redo |
| Ctrl+C, Ctrl+X, Ctrl+V | Copy, cut, paste |
| Ctrl+A | Select all |
| Ctrl+F | Find (Enter or F3 for the next match, Shift+F3 for the previous, Esc closes) |
| Ctrl+H | Find and replace. Tab switches between the two fields. Enter replaces and moves on. Ctrl+Enter replaces all |
| Ctrl+G | Go to a line: `120`, or `120:8` for line and column |
| Tab, Shift+Tab | Indent or unindent the selected lines |
| Ctrl+Left, Ctrl+Right | Move by word |
| Home | First character of the line, then column 0 |

In the find bar, "Aa" makes the search case sensitive, and ".*" turns on regular
expressions.

## Images

The image viewer (`flview`).

| Keys | What it does |
|---|---|
| Right, Page Down | Next picture |
| Left, Page Up, Backspace | Previous picture |
| Home, End | First, last picture |
| + or =, - | Zoom in, zoom out (the mouse wheel zooms at the pointer) |
| 0 or F | Fit the picture to the window |
| 1 | 100 % |
| Up, Down | Pan, or zoom when the whole picture is visible |
| R, Shift+R | Rotate clockwise, counter-clockwise |
| [ and ] | Rotate counter-clockwise, clockwise |
| W | Use the picture as the desktop wallpaper |
| T | Show or hide the thumbnail strip |
| Delete | Move the picture to the Trash |
| Ctrl+P | Print the picture |
| Q or Esc | Quit |
| Ctrl+W | Quit |

## Videos

The video player (`flvideo`).

| Keys | What it does |
|---|---|
| Space or K | Play or pause |
| Left, Right | Back or forward 5 seconds (Shift: 30 s; Ctrl: 60 s) |
| J, L | Back or forward 10 seconds |
| Up, Down | Volume up or down |
| M | Mute |
| F or F11 | Full screen |
| Escape | Leave full screen |
| V | Next subtitle track (after the last one, subtitles are off) |
| A | Next audio track |
| [ and ] | Slower, faster |
| Ctrl+O | Open a file (asks the desktop's file dialog) |
| Q | Quit |
| Click the picture | Pause |
| Double click | Full screen |

## Music

The music player (`flmusic`).

| Keys | What it does |
|---|---|
| Space | Play or pause |
| Left, Right | Back or forward 5 seconds |
| Alt+Left, Alt+Right | Previous or next track |
| N, P | Next or previous track |
| M | Mute |
| S | Shuffle |
| R | Repeat (off, one, all) |
| A | Add the selected song to the queue |
| Q | Show or hide the queue |
| 1, 2, 3 | Albums, Artists, Songs |
| Ctrl+F or / | Search |
| Ctrl+Q | Quit |

## Documents

The document viewer (`flpdf`), for PDF, XPS, EPUB and CBZ.

| Keys | What it does |
|---|---|
| Up, Down, mouse wheel | Scroll |
| Left, Right | Scroll sideways |
| Space, Page Down | One screen down |
| Page Up | One screen up |
| Home, End | Start, end of the document |
| N, P | Next, previous page |
| + or =, - | Zoom in, zoom out. Ctrl+wheel zooms too |
| 0 | Fit to the width |
| 9 | Fit the whole page |
| 1 | 100 % |
| Ctrl+F or / | Search |
| Enter, F3, Ctrl+G | Next match (Shift: previous) |
| C or F9 | Show or hide the contents |
| Escape | Clear the search, then quit |

## Archives

The archive tool (`flarchive`).

| Keys | What it does |
|---|---|
| Enter, double click | Open a folder, or offer to extract a file |
| Backspace, Alt+Up | Go up |
| Ctrl+E | Extract everything |
| Ctrl+Shift+E | Extract the selected row |
| Ctrl+O | Open an archive |
| Ctrl+N | New archive |
| Ctrl+Q | Quit |

## General Manager

The task manager (`general-manager`). It opens with Ctrl+Shift+Esc from anywhere on the desktop.

| Keys | What it does |
|---|---|
| Ctrl+F | Search (Esc clears it) |
| Ctrl+1 to Ctrl+5, or Ctrl+Page Up / Ctrl+Page Down | Switch tabs |
| Up, Down, Page Up, Page Down, Home, End | Move the selection |
| Right, Left | Expand or collapse a group |
| Enter | Expand or collapse |
| Delete | End the selected task (sends SIGTERM) |
| Shift+Delete | Force-end the selected task (sends SIGKILL) |

Use force-ending only when a program does not respond to the normal end.

## Snappel

The screenshot tool (`snappel`). Print and Shift+Print are handled by the desktop.

| Keys | What it does |
|---|---|
| Print | Whole screen, saved to `~/pictures/screenshots`, with a notification |
| Shift+Print | Region: drag a rectangle, then Save (Enter), Copy, Edit or Esc |
| Super+Shift+S | Opens the Snappel window |
| Ctrl+Z | Undo the last annotation (in the editor) |
| Ctrl+C | Copy the picture (in the editor) |
| Ctrl+S | Save (in the editor) |

## FreeBWS (web browser)

The web browser (`freebws`).

| Keys | What it does |
|---|---|
| Ctrl+L, F6, Alt+D | Select the address bar |
| Enter | Load the address (or search for it) |
| Escape | Put the page's address back. Press again to go back to the page |
| F5, Ctrl+R | Reload |
| Alt+Left, Alt+Right | Back, forward |
| Ctrl+T | New tab |
| Ctrl+W, Ctrl+F4 | Close the tab |
| Ctrl+Shift+T | Reopen the last closed tab |
| Ctrl+Tab, Ctrl+PgDn | Next tab |
| Ctrl+Shift+Tab, Ctrl+PgUp | Previous tab |
| Alt+1 to Alt+8 | Go to tab 1 to 8 |
| Alt+9 | Go to the last tab |
| Ctrl+1 to Ctrl+9 | The same as the Alt numbers |
| Ctrl+D | Bookmark the page (again removes it) |
| Ctrl+Shift+B | Show or hide the bookmarks bar |
| Ctrl+Shift+O | Open the bookmarks page |
| Ctrl+J | Open the downloads page |
| Escape | Closes a menu or a pop-up in the browser |

Middle click on a tab closes it. Ctrl+click and middle click on a link open it
in the background.

## Settings and Themes

Settings (`flsettings`) and Themes (`fltheme`) have no shortcuts of their own, apart
from the usual ones for the controls (Tab moves between fields, Enter confirms,
Esc closes a dialog). Settings > Keyboard shows a read-only list of the desktop
shortcuts (see the Discrepancies section below for one difference).

Settings > Workspaces lists the workspace keys: Super+Ctrl+arrows, Super+Shift+arrows
and Super+Tab.

Login and lock screen (`flgreet-ui` and `fllock`): Enter logs in or unlocks. Esc or
Ctrl+U clears the password field in the lock screen.

## Discrepancies

| What | Where | Note |
|---|---|---|
| Text consoles | Settings > Keyboard lists "Ctrl+Alt+F1 ... F6" | The compositor handles **F1 to F12**. The list in Settings is out of date |
| Missing from the list | Settings > Keyboard | Shift+Print (region screenshot) and Super+Shift+S (Snappel window) are not in the list |
| Maximize | The panel's button menu | There is no Maximize item in the panel's menu yet. Use Super+Up |

The shortcut list in Settings > Keyboard is read-only. The keys themselves cannot
be changed yet.

See [04-the-desktop.md](04-the-desktop.md) for what each shortcut does in context,
and [06-apps.md](06-apps.md) for the apps.
