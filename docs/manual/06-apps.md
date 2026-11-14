# 6. Apps

> One section per app that ships with FreeLFS. Each section gives the name you see in
> the start menu, the program's command name in brackets, what it does, and tips.
> Keys are in [05-keyboard-shortcuts.md](05-keyboard-shortcuts.md).

## Contents

- [Files (flfiles)](#files-flfiles)
- [Settings (flsettings)](#settings-flsettings)
- [Themes (fltheme)](#themes-fltheme)
- [Software Store (flstore)](#software-store-flstore)
- [Packages (flpackages), hidden](#packages-flpackages-hidden)
- [FreeBWS (freebws), web browser](#freebws-freebws-web-browser)
- [Terminal (flterm), with the shell flsh](#terminal-flterm-with-the-shell-flsh)
- [Text Editor (fledit)](#text-editor-fledit)
- [Images (flview)](#images-flview)
- [Videos (flvideo)](#videos-flvideo)
- [Music (flmusic)](#music-flmusic)
- [Documents (flpdf)](#documents-flpdf)
- [Archives (flarchive)](#archives-flarchive)
- [General Manager (general-manager)](#general-manager-general-manager)
- [Snappel (snappel)](#snappel-snappel)
- [FreeLFS install (flinstall)](#freelfs-install-flinstall)
- [Tools without a window](#tools-without-a-window)
- [Apps that are planned](#apps-that-are-planned)

---

## Files (flfiles)

**Files: the file manager.** Browse folders and drives, open, copy, move, rename,
and put things in the trash.

- **Layout.** A toolbar with back, forward, up, an editable path field and a
  new-folder button. On the left, **Places** (Home, your usual folders, Trash) and
  **Drives** (the system, USB sticks and the boot stick). The main area is a grid of
  icons. A status bar at the bottom.
- **Open.** Enter or a double click. A folder opens in place. A file opens in the
  default app for its type.
- **Right click** on a file or folder: Open, Open with..., Cut, Copy, Paste, Rename,
  Move to Trash, Delete permanently, Extract here (archives), Compress, Properties,
  and Open terminal here. On empty space: New folder, New empty file, Paste, Open
  terminal here, Show hidden files. In the Trash: Restore, Copy, Empty Trash.
- **Copy, move and delete run in the background.** A progress card sits at the bottom
  right, with a Stop button. Stop ends after the file in progress. If a file with
  the same name exists, the window asks to replace it, keep both, or skip it, and
  you can answer once for all the rest.
- **Drag and drop** inside the window: drag items onto a folder or a sidebar entry
  to move them. Hold Ctrl to copy. Drop on Trash in the sidebar to trash them.
- **Hidden files.** Names with a leading dot, and names listed in a folder's
  `.hidden` file. Ctrl+H shows them.
- **Trash** follows the freedesktop standard (`~/.local/share/Trash`). Restore puts
  an item back where it came from.
- **Drives.** Click a drive to mount it and open it. The eject button ejects it. See
  [07-files-drives-usb.md](07-files-drives-usb.md).
- **Default apps.** Enter on a file uses the default app for its type (images to
  Images, PDF and e-books to Documents, audio to Music, video to Videos, text to the
  Text Editor, archives to Archives). The choice can be changed in the
  "Open with..." list.

**Tips**

- Ctrl+L types a path. Use it to jump straight to a folder.
- Delete moves to the Trash. Shift+Delete deletes for good, after asking.
- Deleting or trashing a system folder (such as `/usr` or `/boot`) brings up a
  warning screen, "You can't do that." Nothing is deleted when you choose "Never mind".

**Known bug (round 17):** opening media from Files failed on the laptop. Round 18
is working on it. See [09-troubleshooting.md](09-troubleshooting.md).

## Settings (flsettings)

**Settings: system settings.** Display, mouse, keyboard, Wi-Fi, Bluetooth, printers,
time and date, workspaces, snapshots and the About page.

| Page | What it is for |
|---|---|
| General | Dark or light, accent colour, panel labels, system sounds, and "More in Themes" |
| Display | Screens (resolution, scale, arrangement), night light, brightness, wallpaper |
| Mouse & touchpad | Speed, acceleration, scrolling, tap to click |
| Keyboard | Layout, repeat speed, Num Lock, a try-it field, and the shortcut list |
| Workspaces | The grid of workspaces |
| Date & time | Time zone (map and search), 24 or 12 hour clock, automatic time |
| Wi-Fi | Networks, connect, forget |
| Bluetooth | Adapter, pairing, connected devices |
| Printers | Add, set default, test page, cancel jobs |
| Snapshots | Take a snapshot, go back to one, delete one |
| About | System summary, Save a report, Keep my files on the live stick |

Every setting and what it writes is in [08-settings-reference.md](08-settings-reference.md).

Tips:

- Open a page directly from a terminal: `flsettings --page wifi`, `--page display`,
  and so on. The page names are listed in the reference.
- Settings writes to `~/.config/freelfs/desktop.conf`. The desktop follows at once.

## Themes (fltheme)

**Themes: the look of the desktop.** One place for presets, colours, fonts,
window buttons, the panel and animations.

- **Presets.** FreeLFS Dark (the default), FreeLFS Light, Compact and Large. Save
  the current look as "My presets", apply or delete them. Reset puts every themed
  setting back to its default.
- **Colours.** Dark or light, and the accent colour (six swatches or six hex digits).
- **Fonts.** The interface font (IBM Plex Sans, IBM Plex Mono, DejaVu Sans or DejaVu
  Sans Mono, the ones installed) and a size of 11 to 16 pixels.
- **Windows.** Buttons on the right or the left. Title bar normal or compact.
- **Panel.** The bottom or top edge, a height from 36 to 60 pixels, and the icon size.
- **Animations.** On, reduced (fades only) or off, and a speed from 0.5 to 2 times.

Tips:

- There is no Apply button. The change is shown on the miniature at the top of the
  page and on the real windows at once.
- Flatpak and GTK programs follow the dark or light setting through the GTK
  settings files. Qt programs are not covered yet.

## Software Store (flstore)

**Software Store: find, install and update apps.** One window with three tabs.

- **Explore.** A home page with featured apps, recently updated apps, eight
  categories, search, and FreeLFS packages.
- **Installed.** Everything you installed, from either source.
- **Updates.** What can be updated, with "Update all".

There are two sources:

| Source | What it is | Needs |
|---|---|---|
| **Flathub** | Apps packaged with Flatpak, installed for you alone (no administrator password) | An installed system. The Flathub entry is added at your first login |
| **FreeLFS** | The project's own packages (the ones the system is built from) | A package repository set up in `/etc/flpkg.conf`, and an index synced |

Installing a FreeLFS package asks for your password (through flauth), because it
changes the system. The password is asked again after five minutes.

Tips:

- **Firefox** is not in the FreeLFS repository. On an installed system, search for
  Firefox in the Store (Flathub) and install it. The project's plan names Firefox as the
  browser for sites that FreeBWS cannot handle (see "FreeBWS" below).
- **On the live system** the Store does not load the Flathub catalogue, to save
  memory. It offers "Install FreeLFS". Its "Try one small app anyway" loads the
  catalogue and uses about 100 MB.
- A progress bar shows the steps of a Flathub install. Cancel stops a Flathub change.
- The Updates tab is also what the update notifier opens.

**Known limit:** the public FreeLFS package repository is not published yet, so the
FreeLFS tab has nothing to install from for most people. Flathub works on an
installed system.

## Packages (flpackages), hidden

**Packages: the earlier package list.** It is hidden from the start menu, because the
Software Store replaces it. The program stays in the system because the Store uses
its helper for installing and removing FreeLFS packages.

If you need the window for some reason, run `flpackages` from a terminal. It has
Installed and Available tabs for FreeLFS packages, and a Flathub tab.

## FreeBWS (freebws), web browser

**FreeBWS: the web browser.** It is FreeLFS's own browser, built on WPE WebKit.
It is the light browser for computers with 1 GB of memory.

- **Address bar.** Type an address or a search. An address without a scheme gets
  https://. Anything else is a search, with **Brave Search** as the default. Other
  engines are in Settings (`freebws://settings`).
- **Tabs.** Ctrl+T, Ctrl+W, Ctrl+Tab. Middle click on a tab closes it. Background
  tabs that were not used for 30 minutes are discarded to save memory. Their page
  loads again when you select them.
- **Bookmarks.** Ctrl+D, the bookmarks bar (Ctrl+Shift+B), and the bookmarks page
  (`freebws://bookmarks`). You can import bookmarks from Firefox or Chromium HTML files.
- **Downloads** go to `~/downloads`. The toolbar button shows the progress. The
  downloads page is `freebws://downloads` (Ctrl+J).
- **Ad blocker** is on by default, with a shield in the toolbar that shows the count
  and a switch for the site you are on.
- **Dark mode** for web pages that are light. Set it per site from the menu.
- **Sandbox.** The web page processes run in bubblewrap, a sandbox. `freebws
  --sandbox-check` prints whether it works on your system.

**Limits (honest list):**

- **Logging in to some sites does not work.** Google sign-in and some anti-bot
  checks (Cloudflare Turnstile) are known problems. For those sites use **Firefox**
  from Flathub in the Software Store.
- Find in page, history, zoom and a private window are not there yet.
- Video and audio play in software, not on the graphics card.
- On the laptop, FreeBWS was blank on the real graphics card. It was fixed in
  release 9 and has not been re-checked on the laptop since.

## Terminal (flterm), with the shell flsh

**Terminal: type commands.** It runs the shell **flsh** by default.

- Colours and suggestions in the shell. Commands are green when they exist, red
  when they do not. Grey suggestions come from your history: press Right to take one.
- Tab completes commands, file names, variables and options. A menu opens when
  several things fit.
- Ctrl+R searches the history.
- A typo gets a hint: "grpe: no such command. Did you mean grep?"
- The prompt shows your user, the folder and the git branch. After a failed
  command, a red mark shows the exit status. Commands that take over 5 seconds
  show their time.
- The scrollback keeps 2000 lines. Select with the mouse to copy (Ctrl+Shift+C) and
  paste (Ctrl+Shift+V or the middle button).
- Configuration: `/etc/flshrc` and `~/.config/flsh/flshrc`. The profile is
  `~/.profile`. History is kept in `~/.local/share/flsh/history`.
- The shell is POSIX: commands from tutorials and most shell scripts work.

Terminal opens in your home folder. The rest of the shell is described in its
README (`src/flsh/README`).

## Text Editor (fledit)

**Text Editor: write and edit plain text.**

- Tabs, line numbers, find and replace (plain text or regular expressions), go to line.
- **Syntax colours** for C, C++, Java, C#, shell, Python, JavaScript, TypeScript,
  JSON, HTML, XML, CSS, DSSL, Markdown, and the config formats (conf, ini, desktop,
  YAML, TOML). In the FreeLFS palette, in dark and light.
- **Auto-indent** and bracket matching. Markdown lists and quotes continue on the
  next line.
- **Remembers your session.** The open files, the active tab and the cursor
  position of the last 100 files are kept. A file opened from Files does not
  replace the list.
- **Safe saving.** It writes a temporary file and renames it over the old one. A
  failed save never leaves half a file. Unsaved text is not remembered.
- UTF-8 text. A byte order mark and CRLF line endings are kept on save. Files with
  NUL bytes (binary files) are refused.

Files over 16 MB are shown without colours.

## Images (flview)

**Images: view pictures.** It opens PNG, JPEG and WebP. An animated WebP shows its
first frame.

- Opens a picture and keeps its whole folder at hand. Without a file, it opens
  `~/Pictures`, or the current folder.
- **Zoom and pan**: wheel or + and -, drag, double click for fit and 100 %.
- **Rotate** with R, Shift+R, [ and ].
- **Thumbnail strip** (T) to jump around the folder.
- **Set as wallpaper** (W). The picture is copied to your wallpapers folder and the
  desktop uses it. Deleting the original is then safe.
- **Delete** moves the picture to the Trash.
- **Print** (Ctrl+P) sends the picture to the printer through CUPS. Set up the
  printer first in Settings > Printers.

Pictures from a camera turn the right way up (EXIF orientation is used).

## Videos (flvideo)

**Videos: play video files.** It uses mpv's engine, drawn in its own window.

- Controls at the bottom: play and pause, back and forward 10 seconds, a seek bar,
  speed (0.5 to 2 times), Audio and Subtitles menus, mute, volume, full screen.
- Subtitles: tracks inside the file, and `.srt`, `.ass` or `.sub` files with a
  similar name next to the video.
- **Resume.** The position is saved every 15 seconds and used the next time the
  same file opens.
- **Open** (Ctrl+O) uses the desktop's file dialog.

**Decoding.** Since round 18, video decodes in software by default. On the laptop a
Ryzen machine played nothing with the graphics card's decoder, so the decoder is
turned off by default. You can try it with `FLVIDEO_HWDEC=auto-copy-safe`. The log
is `~/.cache/flvideo/mpv.log`.

**Not done:** playlists, DVD and network streams, chapters in the controls, subtitle
delay.

## Music (flmusic)

**Music: play your music.**

- The library is `~/music`, or `~/Music` when that does not exist. It is read by the file tags.
- Three views: **Albums**, **Artists** and **Songs**. A search box filters all three.
- **Queue.** Double click plays a song with the rest of the list as the queue. Add to
  queue appends it. The queue panel shows "Up next".
- Shuffle, repeat (off, one, all), a seek bar and a volume slider.
- Formats: MP3, FLAC, AAC and M4A, WAV, Ogg Vorbis and Opus, through GStreamer.
- **Media keys.** The Play, Stop, Next and Previous keys control it. Any program
  that speaks the standard MPRIS interface can control it too.

**Limit:** embedded cover art is not shown yet. Albums have a flat colour tile with
the first letter of the album name.

## Documents (flpdf)

**Documents: read PDF and e-book files.** It opens PDF, XPS, EPUB and CBZ.

- Pages in one continuous column, with zoom and fit-to-width or fit-to-page.
- **Search** (Ctrl+F or /) highlights every match on the pages you see.
- **Contents** (C or F9) when the document has a table of contents. A click goes to
  the page.
- Links: a click on an internal link jumps to the page. A click on a web address
  copies it to the clipboard.
- Idle memory for a small PDF is about 17 MB.

**Not done:** password-protected files (they are reported, not opened), selecting and
copying text, annotations and forms, and printing from this app.

## Archives (flarchive)

**Archives: open, extract and create archives.** It reads zip, tar (with gzip,
bzip2, xz, zstd, lz4 or lzma), 7z, rar (read only), cab, cpio, deb, rpm and iso.
It creates **zip** and **tar.zst**.

- **Browse** an archive: double click folders, Backspace goes up.
- **Extract** everything (Ctrl+E) or the selected item (Ctrl+Shift+E). Extraction
  never overwrites anything. Items go to a free name. A second extraction gets a
  number ("project 2").
- **Create** a new archive: add files and folders, choose the format and the name.
- From **Files**: right click an archive for "Extract here", or select files and
  choose "Compress".
- Extraction shows its progress and can be cancelled.

**Limits:** archives with passwords are refused. You cannot add to or remove from an
existing archive, and 7z and rar can only be read.

## General Manager (general-manager)

**General Manager: the task manager.** Open it with Ctrl+Shift+Esc, or from the start
menu.

| Tab | What it shows |
|---|---|
| **Processes** | Apps (with their child processes), your background processes and system processes. CPU, memory, disk and network columns |
| **Performance** | CPU, memory, each disk, each network adapter and each graphics card, with a 60 second graph |
| **Startup apps** | Programs that start when you log in. A switch turns each one on or off |
| **Users** | One row per user, with the totals |
| **Details** | A flat table of every process, sortable |

- **End task** (button or Delete) asks the program to stop (SIGTERM). Shift+Delete
  forces it (SIGKILL). Use the forced stop only when the normal one does not work.
- Memory is shown as private memory (the memory used only by that program).
- Disk figures for another user's processes show a dash, because you cannot read them.
- The window refreshes once a second.

## Snappel (snappel)

**Snappel: take and mark up screenshots.**

- **Print** saves the whole screen to `~/pictures/screenshots`, with a notification.
  Click the notification to open the picture.
- **Shift+Print** lets you select a region. Then Save (Enter), Copy, Edit or Esc.
- **Super+Shift+S** opens the Snappel window: Full screen, Window, Region, with a
  delay of none, 3, 5 or 10 seconds.
- Capturing a single window (`snappel --window`) gives the window alone, even when it is covered.

**The editor** has Arrow, Box, Text, Highlight, Blur, Pixelate and Crop. Six colours and
widths from 1 to 6. Undo takes back the last mark exactly. From the editor you can
save, copy the image to the clipboard, open it in the viewer or Files, or start a
new capture.

## FreeLFS install (flinstall)

**FreeLFS install: install FreeLFS on this computer.** It is on the live desktop as
an icon, and in the start menu under System. It does not appear on an installed
system.

The installer is described step by step in [03-installing.md](03-installing.md).
Read that chapter before you press any button that erases a disk.

## Tools without a window

These programs have no window and no start-menu entry. They are used by the other
apps, or from a terminal.

| Command | What it is for |
|---|---|
| `flreport` | Collects logs and device details into one file for a bug report. Settings > About > Save a report runs it. See [09-troubleshooting.md](09-troubleshooting.md) |
| `flnotify` | Shows a notification from a script: `flnotify "Title" "Text"`. Options for an icon, urgency, timeout and buttons. `--dnd on` or `off` switches Do not disturb |
| `flpower` | Logs out, restarts or shuts down. The panel's buttons use it |
| `flpkg` | The package manager (see the developer and admin chapters) |
| `flsnap` | Takes, lists and restores btrfs snapshots. Settings > Snapshots uses it |
| `flmount` | Mounts, unmounts and ejects USB drives. Files uses it |
| `flpersist` | Makes and checks the persistence file of the live stick |
| `flauth` | Asks for your password before a system change, with a dialog |
| `flsound` | Plays the system sounds (login, notification, error, volume tick, screenshot) |
| `fldisplay` | Lists and sets screens from a script. Settings > Display does the same in a window |
| `fltime-set`, `fl-sntp` | Time zone and automatic time. Settings > Date & time uses them |
| `flsh` | The shell (see Terminal above) |
| `fllock` | The lock screen (see [04-the-desktop.md](04-the-desktop.md)) |
| `flgreet` and `flgreet-ui` | The login screen |
| `flcrash` | The "Something went wrong" screen |
| `flsplash` | The boot screen |
| `flsetup` | The first-boot keyboard choice on a system without `/etc/keyboard.conf` |
| `flupdate-notify` | Checks for updates once a day and shows a notification. The Store is where you install them |
| `flportal` | The backend that lets Flatpak apps show the desktop's file dialog and notifications. Not started by hand |
| `flgaming` | A helper for Steam (Flatpak): `flgaming check` shows what works. Not tested on the laptop |

## Apps that are planned

These are in the plan but not built yet:

- **Hardware** (`flhw`): an overview of CPU, graphics driver, memory, disks and
  Wi-Fi, with power profiles, memory (zram and swappiness) and disk health.
  See `docs/PLAN-installer.md`.
- **Welcome app**: a short tour on the first login, reopenable from the menu.
- **FreeBWS** community guide (`docs/CONTRIBUTING-freebws.md`).
- **Translations** of the desktop and apps into the 12 installer languages.
- **GenericName** lines in every menu file, so that the start menu shows a short
  description under each name.

[Back to the table of contents](README.md) · [Next: 07, files, drives and USB](07-files-drives-usb.md)
