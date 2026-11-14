# 7. Files, drives and USB sticks

> The Files app is where you browse, copy, move and delete. This chapter covers
> the drives it shows, how USB sticks are mounted and removed safely, copying and
> trash, and which app opens which kind of file.

## Contents

- [The Files window](#the-files-window)
- [Places and Drives](#places-and-drives)
- [USB sticks](#usb-sticks)
  - [What happens when you plug one in](#what-happens-when-you-plug-one-in)
  - [Which file systems work](#which-file-systems-work)
  - [Removing a stick safely](#removing-a-stick-safely)
  - [Mounting by hand](#mounting-by-hand)
- [Copy, move and paste](#copy-move-and-paste)
- [Trash and deleting](#trash-and-deleting)
- [Opening files and default apps](#opening-files-and-default-apps)
- [Folders on the desktop](#folders-on-the-desktop)
- [Your home folder](#your-home-folder)
- [Files on the live system](#files-on-the-live-system)
- [Known bugs and limits](#known-bugs-and-limits)

## The Files window

Start Files from the start menu (System), or with **Super+E**.

| Part | What it does |
|---|---|
| Toolbar | Back, forward, up, a path field (Ctrl+L), new folder |
| Places (left) | Home, your usual folders (from `~/.config/user-dirs.dirs`), and Trash |
| Drives (left) | The system, USB sticks, the boot stick, and mounts under `/drives` |
| Main area | Icons of the folder. It updates on its own when files change |
| Status bar | Information about the folder and the current job |
| Progress card (bottom right) | Copy, move, trash and restore, with a Stop button |

Files keeps working while a copy, move, trash or restore runs. Those run as
separate jobs, so the window does not freeze.

## Places and Drives

- **Places** are your home folder and its usual folders. Trash is there too.
- **Drives** show:
  - the system (the root of the FreeLFS install),
  - every removable USB stick that is plugged in, once it is mounted,
  - the boot stick, when you are on the live system,
  - other mounts under `/drives`.

A drive that is plugged in but not mounted is shown too. Click it to mount it and
open it. The eject button next to it unmounts and powers it off.

If mounting fails, the message appears in a dialog. Files can show the drives
only when the mount helper `flmount` is present. Without it, only the system and
the existing mounts are listed.

## USB sticks

### What happens when you plug one in

1. The system sees the new partition and mounts it at `/drives/LABEL`. `LABEL`
   is the stick's name, cleaned up so it is safe to use as a folder name.
2. A notification appears: **"USB stick 'LABEL' connected"**, with an **Open**
   button. Open starts Files at the stick.
3. The stick's folder shows in the Drives list.

Mounted sticks are set to `nosuid`, `nodev` and `noexec`. Programs on the stick
cannot run, and device files on it are ignored. That is a safety setting, not a fault.

The setting applies to the stick only. A copy of the same program in your home
folder is not affected by it.

### Which file systems work

| File system | Works | Notes |
|---|---|---|
| FAT32 (vfat) | Yes | Most sticks. Files on it appear as yours |
| exFAT | Yes | Large sticks, and Ventoy's data partition. Files on it appear as yours |
| ext4 | Yes | Linux sticks. The top folder is given to you when root owns it |
| btrfs | Yes | Same rule as ext4 |
| NTFS (Windows) | **No** | The kernel has no NTFS driver built in. Such a stick cannot be mounted |
| Others | Not tested | Ask in a bug report if you need one |


### Removing a stick safely

1. In Files, click the eject button next to the stick. The stick is unmounted and
   powered off. A stick that is ejected can be taken out.
2. Wait for the confirmation. Only then take the stick out.

If you pull a stick without ejecting it, the system cleans up the mount and forgets
it. Files you were writing may be lost. Always eject first.

The live system's own boot stick is special. Eject does not power it off, because
the live system runs from it.

### Mounting by hand

From a terminal, the helper is `flmount`. It is for people who want to see what
happens:

```sh
flmount list            # one line per partition: device, label, file system, size, mount point, class
flmount mount sdb1      # mounts it at /drives/LABEL and prints where
flmount unmount sdb1    # unmounts it
flmount eject sdb       # unmounts every partition and powers the disk off
```

Names are the kernel's device names, such as `sdb1`, not `/dev/sdb1`. Only
removable and boot partitions are listed by default. Internal disks need `--internal`,
and Files does not use that option. Be careful with internal disks: they hold your system.

## Copy, move and paste

| Action | Keys | Mouse |
|---|---|---|
| Copy | Ctrl+C | Right click, Copy |
| Cut (move later) | Ctrl+X | Right click, Cut |
| Paste | Ctrl+V | Right click on a folder, Paste |
| Move by dragging | Drag onto a folder or sidebar entry | |
| Copy by dragging | Hold Ctrl, then drag | |
| Move to Trash by dragging | Drop on Trash in the sidebar | |

Copy, cut and paste also work **between two Files windows**, for example from
your home folder to a USB stick.

**Name clashes.** If a file with the same name is already there, Files asks:

- **Replace**: the old file is overwritten. Use with care.
- **Keep both**: the copy gets a new name.
- **Skip**: the copy of this item is not made.

Tick "Do the same for the rest" to answer once for all of them.

**Stopping.** The **Stop** button on the progress card ends the copy after the file in
progress. Files already copied stay where they are.

## Trash and deleting

| Action | Result |
|---|---|
| Delete (key) or **Move to Trash** | Moves the item to the Trash in your home folder (`~/.local/share/Trash`) |
| Shift+Delete or **Delete permanently** | Deletes it for good, after asking |
| Restore (in the Trash) | Puts the item back where it came from |
| Empty Trash (right click in the Trash) | Deletes everything in the Trash for good |

The Trash follows the freedesktop standard. Each item has a small `.trashinfo` file
that remembers where it came from.

**Not verified:** whether a USB stick has its own Trash. Check before you rely on
trashing files on a stick. For a file you need to keep, copy it first.

**Protected folders.** Delete and Move to Trash first check the selection. If the
selection is a vital system folder (such as `/sysflfs` or `/boot`), a screen says
"You can't do that." Choose "Never mind" to keep everything. The check is a deliberate
guard against deleting the system by accident.

## Opening files and default apps

Enter on a file starts the default app for the file's type. The type is read from
the file name, using the shared MIME database.

| File type | Default app |
|---|---|
| Images (PNG, JPEG, WebP) | Images |
| PDF, EPUB, CBZ, XPS | Documents |
| Audio (MP3, FLAC, Ogg, Opus, AAC, WAV, M4A) | Music |
| Video (MP4, MKV, WebM, AVI, MOV, MPEG, Ogg video) | Videos |
| Text (plain, Markdown, JSON, shell scripts) | Text Editor |
| Archives (zip, tar, 7z, rar, gz, bz2, xz, zst) | Archives |

A file of a type with no default app is opened by the first app that handles the type.
A text file with no known name still opens in the Text Editor. A file with no app
shows a message.

**Open with...** (right click) lists every app that can open the type, with the
default first.

**Changing the default app.** The defaults are set in
`/usr/share/applications/mimeapps.list`, and your own choices go in
`~/.config/mimeapps.list`. The screen for changing the default is not described in the
project notes yet, so **use the file to change it**, or ask in a bug report.

## Folders on the desktop

The desktop shows the files in `~/Desktop` as icons. Click, Ctrl+click, drag to select.
Double click opens. Right click offers Open, Rename and Move to Trash. The desktop's
own right-click menu has "New folder", "New text file" and "Open terminal here".

Not yet: dragging a file from Files onto the desktop, and "Open with" and
"Properties" on desktop icons. See [04-the-desktop.md](04-the-desktop.md).

## Your home folder

Your home folder is `/home/NAME`. The usual folders are:

| Folder | Used for |
|---|---|
| `~/Desktop` | Files and folders on the desktop |
| `~/Documents` | Documents |
| `~/downloads` | Browser downloads (FreeBWS) |
| `~/Music` or `~/music` | The music library (Music reads either) |
| `~/Pictures` or `~/pictures` | Pictures. Screenshots (Snappel) go to `~/pictures/screenshots` |
| `~/Videos` | Videos |
| `~/.local/share/Trash` | The Trash |
| `~/.config/freelfs` | Settings (`desktop.conf`), the desktop's icon positions |

The folder names follow the usual desktop names. Some programs use lower case
names instead: FreeBWS saves to `~/downloads`, Snappel to `~/pictures`. Check the
names in Files.

## Files on the live system

The live system's home folder lives in RAM. Files you save there are gone when
you shut down, unless persistence is on (see [02-getting-started.md](02-getting-started.md)).

Before you shut down:

1. Plug in a USB stick.
2. Copy the files you want to keep to the stick (Copy in Files, then Paste on the stick).
3. Eject the stick.

Files on the stick are yours after the stick is mounted, and they stay there when
you shut down.

## Known bugs and limits

- **Opening media from Files failed on the laptop in round 17.** Round 18 is working on
  it. Videos decodes in software by default since round 18.
- **Shutdown** also failed on the laptop in round 17. See [09-troubleshooting.md](09-troubleshooting.md).
- No NTFS drives.
- No drag and drop from Files onto other programs, and none from the desktop.
- Trash on a stick is not verified.
- Desktop icons do not have "Open with" or "Properties" yet.

See [06-apps.md](06-apps.md) for each app, and [08-settings-reference.md](08-settings-reference.md)
for the settings that affect drives and the desktop.
