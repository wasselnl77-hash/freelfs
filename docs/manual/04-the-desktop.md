# 4. The desktop

> The desktop is drawn by **flcomp** (the compositor) and **flpanel** (the panel,
> start menu, tray and notifications). This chapter covers how they work together:
> the start menu, the tray, workspaces, windows, the desktop icons, notifications,
> the clipboard, locking and leaving the session.

## Contents

- [The screen at a glance](#the-screen-at-a-glance)
- [The start menu](#the-start-menu)
- [Search](#search)
- [The tray](#the-tray)
- [Workspaces](#workspaces)
- [Windows](#windows)
  - [Moving, snapping and maximizing](#moving-snapping-and-maximizing)
  - [The title-bar menu](#the-title-bar-menu)
  - [Switching windows](#switching-windows)
  - [Entries in the panel](#entries-in-the-panel)
- [Desktop icons and the desktop menu](#desktop-icons-and-the-desktop-menu)
- [Notifications](#notifications)
- [The clipboard history](#the-clipboard-history)
- [Lock screen](#lock-screen)
- [Log out, restart and shut down](#log-out-restart-and-shut-down)
- [Themes, animations and performance](#themes-animations-and-performance)
- [The "Something went wrong" screen](#the-something-went-wrong-screen)
- [Known limits](#known-limits)

## The screen at a glance

| Part | Where | What it is |
|---|---|---|
| Panel | Bottom edge (top edge if you choose) | Start button, one button per open window, tray, clock |
| Start menu | Opens from the start button or the Super key | Programs, search, power buttons |
| Tray | Right side of the panel | Keyboard layout, workspace pager, network, volume, battery, Bluetooth, night light, clock |
| Desktop | The rest of the screen | Wallpaper, desktop icons, windows |
| Notification cards | Top right corner | Short messages that go away by themselves |

The panel is 44 pixels high by default. You can move it to the top, and change its
height (36 to 64 pixels) and its icon size, in Settings > General and Themes
(see [08-settings-reference.md](08-settings-reference.md)).

## The start menu

Press the **Super** key on its own, or click the start button at the bottom left.
Pressing Super again closes it.

The menu has a search box and these categories on the left:

| Category | What is in it |
|---|---|
| **All programs** | Every program. Use it to search all of them |
| **Internet** | The web browser (FreeBWS) |
| **Office** | Text Editor and Documents (PDF and e-books) |
| **System** | Files, Terminal, General Manager, Software Store, Archives, Snappel, the installer, and similar |
| **Settings** | Settings and Themes |
| **Recent** | The last 8 files you opened that still exist |

Music, Videos and Images are in **All programs** (they have no category of their own in the menu).

Each program is a tile. Keys in the menu: type to search, Up and Down to move,
Enter to start, Esc to close.

At the bottom of the menu are the power buttons: **Lock**, **Log out**,
**Restart** and **Shut down**. See [Log out, restart and shut down](#log-out-restart-and-shut-down).

Only programs that can start are listed. A program that is missing is left out,
and so are hidden entries. The Packages app is hidden on purpose: the Software
Store replaces it.

## Search

Start typing in the start menu. The results show as you type. Search looks at the
program's name, its short description and its command. Typing "web" finds the browser.

Search only looks in the category that is open. To search everything, open
**All programs** first.

## The tray

From left to right, the tray shows only what your computer can use:

| Icon | Shown when | Click does | Other |
|---|---|---|---|
| Keyboard layout (for example `de`) | Always | Cycles through `de`, `us`, `uk`, `fr` | The change is for this session only. Settings > Keyboard saves a choice |
| Workspace pager | More than one workspace | Switches to that workspace | Scroll steps through the workspaces. Right click opens a menu (see [Workspaces](#workspaces)) |
| Network | Always | Opens Settings > Wi-Fi | Wi-Fi icon, or a wired icon. Slashed when there is no link |
| Volume | Always | Opens a flyout with a slider, a percentage and a mute button | Scroll over the icon changes the volume by 5 %. Right click mutes |
| Battery | When the computer has a battery | Opens a flyout with the level, the state, the time left and the power saving switch | Accent colour while charging, red at 15 % or less |
| Bluetooth | When a Bluetooth adapter is present | Opens a flyout with a switch and the connected devices | "Bluetooth settings..." opens Settings |
| Night light (moon) | Always | Opens a quick flyout with its switch and "Display settings..." | Warmer colours. Its schedule is in Settings > Display |
| Clock | Always | Opens the notification centre | Date and time. 24 or 12 hour format is a setting |

### Volume

- Click the icon, then drag or click the bar. The arrow keys change it by 5 %,
  Page Up and Page Down by 10 %, Home and End go to the ends, and M mutes.
- The laptop's volume keys work the same way, and show a small indicator on the
  screen.
- Changing the level of a muted output unmutes it.

### Battery and power saving

The battery flyout shows whether the computer runs on battery, is charging or
is full, and how much time is left. The **Power saving** switch:

- lowers the screen brightness to 70 % of its value,
- limits the screen to 30 frames a second,
- sets the processor's governor to save power.

At 10 % a notification offers to turn power saving on. At 5 % a warning stays on
the screen until the charger is connected. These warnings come once for each
discharge.

### Night light

The night light makes the colours warmer. Its switch is in the moon's flyout.
The warmth, the temperature and the times (for example, from 20:00 to 07:00) are
set in Settings > Display, under "Night light", where you can choose Off, On
or Auto.

## Workspaces

Workspaces are separate desktops in a grid. The default grid is **2 by 2**, so
four workspaces. You can choose any grid from 1 by 1 to 8 by 8 in Settings >
Workspaces. See [08-settings-reference.md](08-settings-reference.md).

| To | Use |
|---|---|
| Switch to another workspace | Super+Ctrl+arrows, or click its cell in the pager |
| Move the focused window to another workspace | Super+Shift+arrows (the view follows) |
| See all workspaces at once | Super+Tab. Click a workspace to go there, click a window to focus it. Esc closes the view |
| Move a window from its title-bar menu | Right click the window's title bar, then "Move to workspace N" |
| Move a window from the panel | Right click its panel button, then "Move to workspace N" |

**The pager** appears in the tray only when there is more than one workspace.
It is a small grid. The workspace you are on is filled in the accent colour. A cell
with windows has a mark.

The pager's right-click menu has: "Workspace overview", "Add workspace",
"Remove workspace" and "Workspace settings...". Adding a workspace adds a column
(and a row once there are eight columns). Removing takes one away. Windows on a
removed workspace move to the last one that is left.

New windows open on the workspace you are on. Dialogs open on the workspace of
their parent window.

## Windows

Windows draw their own title bars. The compositor does not add decorations to
them. The buttons are at the right, or at the left,
and the title bar is normal or compact (set in Themes).

Programs that still use X11 (for example old games) get a title bar from
flcomp. See [08-settings-reference.md](08-settings-reference.md).

### Moving, snapping and maximizing

| Action | How |
|---|---|
| Move a window | Drag its title bar |
| Resize a window | Drag an edge or corner |
| Snap to the left or right half | Drag it to the left or right edge of the screen, or press Super+Left or Super+Right. Pressing the opposite key on a snapped window puts it back to its old place |
| Maximize | Drag it to the top edge, press Super+Up, or use the title-bar button |
| Restore, or minimize | Super+Down. A maximized or snapped window goes back to its size. A normal window is minimized |
| Minimize, close | The buttons in the title bar |

A snapped window shows a faint outline of the place it will go while you drag it.

### The title-bar menu

Right click the title bar of a FreeLFS program (for example Files, Settings,
FreeBWS or General Manager). The menu has:

- Minimize (or Restore),
- Maximize (or Restore),
- Always on top,
- Move to workspace N (one line for each other workspace),
- Close.

A middle click on the title bar closes a FreeLFS window.

### Switching windows

- **Alt+Tab** moves to the next window, **Alt+Shift+Tab** to the previous one.
  Hold Alt while you press Tab. A row of window pictures shows the choice; the
  picked one is outlined in the accent colour.
- Clicking a window's button in the panel brings it to the front. If it is on
  another workspace, you go to that workspace. If it is already focused, the click
  minimizes it.
- A middle click on a panel button closes that window.

### Entries in the panel

Each open window has a button in the panel. It shows the window's icon. With
Settings > General > "Show names" (or `panel_labels=1`), the name appears next to
the icon.

## Desktop icons and the desktop menu

The desktop shows the files and folders in `~/Desktop`, and the programs that
have a desktop icon. On the live system that is the **FreeLFS install** icon.

| Action | Result |
|---|---|
| Click an icon | Selects it. Ctrl+click adds to the selection |
| Drag on empty desktop | Selects everything inside the rectangle |
| Drag icons | Moves them and snaps them to the grid |
| Double click | Opens the file or folder |
| Right click on an icon | Open, Rename, Move to Trash |
| Right click on empty desktop | New folder, New text file, Open terminal here, Change wallpaper, Settings, Show or Hide desktop icons |

Move to Trash puts the file in the freedesktop trash (`~/.local/share/Trash`).

Not yet: dragging a file from the Files app onto the desktop, the "Open with" and
"Properties" items on desktop icons. Use the Files app for those.

The icons' positions are kept in `~/.config/freelfs/desktop-icons.conf`.

## Notifications

Notifications are small cards in the top right corner. The panel is the
notification service. Programs send messages to it, and it shows them.

- At most **four** cards are stacked. The newest is on top.
- A normal card goes away after 6 seconds, a low-priority one after 4 seconds.
  A critical one stays until you close it. Cards wait while the pointer rests on them.
- Close a card with its cross, click it, or click one of its buttons (up to three).
- While the screen is locked, no cards are shown.

The **notification centre** opens when you click the clock. It shows:

- **Do not disturb**: a switch. While it is on, normal and low messages are not
  shown as cards (they still go to the history). Critical ones still appear. The
  bell in the clock area is slashed while it is on.
- **Clear all**.
- The **history**: the last 50 notifications, newest first. A click on an entry
  runs its default action. A badge with the number of unread ones sits next to the time.

The history survives a restart of the panel, but not a restart of the session.

## The clipboard history

The desktop remembers the last 20 pieces of text you copied, in memory only. It is
gone when you log out.

To paste from the history: **hold Ctrl+V** for about half a second. A list appears
in the middle of the screen. Pick an entry with the Up and Down keys and Enter,
or click it. Esc closes the list. A quick press of Ctrl+V pastes the newest
text, as always.

The history only holds text. It does not hold pictures or files.

## Lock screen

The screen locks when you:

- press **Super+L**,
- choose **Lock** in the start menu (it is greyed out when the lock program is missing),
- close the laptop's lid (unless `lock_on_lid=0` is set in the desktop settings).

The lock screen shows a large clock, the date, your name and a password field.
Type your password and press Enter. Esc or Ctrl+U clears the field.

While the screen is locked:

- nothing else is drawn, and no window can take input,
- the start menu, Alt+Tab, Print and Ctrl+Alt+Backspace do nothing,
- Ctrl+Alt+F1 to F12 (text consoles) and the media keys (volume, brightness) still work.

The password is checked against your account, the same way as at the login
screen. Each wrong password costs a second before the next check. If the lock
program crashes, the screen stays locked (on a plain colour), and the desktop
starts the lock program again. If that does not help, the screen says so: press
Enter to start the lock again, or switch to a console with Ctrl+Alt+F2, log in,
and end the session with `pkill flcomp`.

**Known limit:** an account whose password is locked or expired cannot be
unlocked on this screen. The screen says so. Ctrl+Alt+F2 is the way out.

## Log out, restart and shut down

Use the power buttons at the bottom of the start menu:

| Button | What happens |
|---|---|
| **Log out** | Ends your session. The login screen comes back |
| **Restart** | Restarts the computer |
| **Shut down** | Switches the computer off |

Each button opens a dialog with a **30 second countdown**. Cancel, Esc, or pressing
the main button again stops it.

Ctrl+Alt+Backspace ends the session at once, with no countdown and no question.
Anything not saved is lost. It is a shortcut for when the desktop is stuck.

The login screen has a **Shut down** button too. A second click within 4 seconds
confirms it.

**Known bug:** shutdown from the desktop failed on the laptop in round 17. The
fix is round 18 work. See [09-troubleshooting.md](09-troubleshooting.md).

## Themes, animations and performance

These are for looks and speed. The full list of settings is in
[08-settings-reference.md](08-settings-reference.md).

- **Dark or light.** Settings > General > Theme. Both use the same palette.
- **Accent colour.** Six colours (green, teal, blue, amber, orange, rose). It colours
  selections, the pager, the volume bar, the lock screen and the Alt+Tab frame.
- **Themes** (its own app): presets, fonts, the buttons' side, the title bar size,
  the panel's edge and height, and animations.
- **Animations** are short, flat fades and slides, 100 to 200 milliseconds. They
  can be `on`, `reduced` (fades only) or `off`, and sped up or slowed down.
- **Performance mode** switches on by itself while a window is full screen. Then
  only that window is drawn, which saves processor work. The battery flyout has
  a switch to keep it on.
- **Power saving** is described under the tray above.

## The "Something went wrong" screen

If the desktop ends badly (it crashes, or it runs out of memory), a screen with
the text "Something went wrong" shows instead of a login. It explains what
happened, with a technical line for a bug report.

| Key | What it does |
|---|---|
| Enter | Starts the desktop again |
| R | Restarts the computer |
| S | Shows the end of the session log. Up, Down, Page Up, Page Down scroll it; Esc or S goes back |
| Esc | Leaves to a text console (tty1) |

A report on this screen can be made from Settings > About. See
[09-troubleshooting.md](09-troubleshooting.md).

## Known limits

- Drag and drop between programs is not there yet. Inside the Files window you
  can drag items onto a folder or a sidebar entry to move them (hold Ctrl to copy).
- X11 programs (Xwayland) run, but they do not take part in drag and drop with
  Wayland programs, and there is no system tray for them.
- Touch screens and server-side window decorations are not supported.
- The right-click menu on a panel button has no Maximize item yet. Use Super+Up or
  the title bar.

Screenshots of the desktop, for comparison: `../m7/m10-desktop-1G.png`,
`../m7/m10-overview-1G.png` (the workspaces overview), `../m7/m10-snap-desktop-1G.png`
(snapped windows), `../m7/m10-accent-blue-settings-1G.png` (an accent colour change)
and `../m7/m10-print-1G.png` (the screenshot notification).

See also [05-keyboard-shortcuts.md](05-keyboard-shortcuts.md) for every key.
