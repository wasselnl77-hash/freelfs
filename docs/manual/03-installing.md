# 3. Installing FreeLFS

> The installer, **FreeLFS install**, copies the live system to a disk, sets up
> your account and makes the disk bootable. **It erases the disk you pick**
> in the default mode. Read the disk step carefully and try the installer in a
> virtual machine first.

## Contents

- [Before you start](#before-you-start)
- [Start the installer](#start-the-installer)
- [The steps](#the-steps)
  - [1. Language](#1-language)
  - [2. Keyboard](#2-keyboard)
  - [3. Wi-Fi](#3-wi-fi)
  - [4. Disk](#4-disk)
  - [5. Account](#5-account)
  - [6. Time zone](#6-time-zone)
  - [7. Summary](#7-summary)
  - [8. Install](#8-install)
- [What the installer does to the disk](#what-the-installer-does-to-the-disk)
- [The layout on disk](#the-layout-on-disk)
- [First boot](#first-boot)
- [Not supported and not tested](#not-supported-and-not-tested)
- [If the installer fails](#if-the-installer-fails)

## Before you start

- You need a **live session** from [02-getting-started.md](02-getting-started.md).
  The installer runs from the live system only.
- The computer must start in **UEFI** mode, with **Secure Boot off**.
- Back up everything on the disk you will install to. The default choice erases it.
- A disk of at least **6 GiB** for the system is needed. The system itself takes
  about 3 GB. Leave more room if you want to keep files.
- Keep the USB stick you started from. Nothing asks for it again, but you may need
  it if the installed system does not start.
- Use a spare computer or a virtual machine for the first try.

Every installed program and file is copied from the live image. The installer
does not download anything, so an internet connection is not needed. Wi-Fi is
optional. It only helps to copy known Wi-Fi networks to the new system.

## Start the installer

Open the start menu and choose **FreeLFS install**. It is also a desktop icon on
the live desktop.

The window shows the steps in order. Use **Continue** to go on and **Back** to
change an earlier step. On the last step the button says **Erase and install** when
the default mode erases the disk, otherwise **Install**. The window never names a disk path itself: the disk you pick
comes from the installer's own list.

Screenshots of each step are in `../m7/`, for example
`../m7/m10-install-live-3-language.png` for the language step.

## The steps

### 1. Language

Choose one of 12 languages: English (US), English (UK), German, French, Spanish,
Italian, Portuguese (Brazil), Dutch, Polish, Czech, Swedish and Turkish.

- The language sets the locale for the programs that read it (through
  `/etc/profile.d/20-lang.sh`). A text console keeps `C.UTF-8`.
- The choice also suggests a keyboard and a time zone for the next steps.
- FreeLFS itself stays English. The other languages are planned for the desktop
  and the apps (see `docs/PLAN-installer.md`); they are not there yet.

### 2. Keyboard

Four layouts are offered: `de`, `us`, `uk` and `fr`. There is a preview of the keys
and a field to try them in. The choice is written to `/etc/keyboard.conf` on
the new system. The first boot therefore does **not** ask about the keyboard again.

More layouts (and a test field for accented letters) are planned.

### 3. Wi-Fi

This step lists the Wi-Fi networks, using the same service as Settings > Wi-Fi.
Networks you already connected to on the live system are copied to the new
system, so it connects by itself after the first boot. You can skip this step.

### 4. Disk

The installer lists the disks it sees. It shows the disks it cannot use:

- the stick you started from (the "live medium");
- disks with something mounted, or that are busy (for example in use by another
  system);
- read-only disks.

Those can be seen but not chosen. Then pick one of three modes.

| Mode | What it does | Erases |
|---|---|---|
| **Use the whole disk** (the default) | Makes a new GPT partition table: a 512 MiB EFI system partition, and the rest as one btrfs partition | **Everything on the disk** |
| **Install next to existing partitions** | Uses only the free space. It picks the largest gap of at least 6.5 GiB, puts a 512 MiB EFI partition and a btrfs partition in it | Nothing. No existing partition is touched |
| **Manual** | A partition editor (see below) | Only what you tell it to |

**Install next to existing partitions** works only on a disk with a GPT partition
table. Dual boot with Windows is **not supported**: the installer does not shrink
partitions or add boot entries for other systems.

**Manual** shows the disk as a bar and a list:

- **Make a partition** in a free gap: choose the role (System, Boot, Swap or
  Data) and the size.
- **Delete** a partition.
- **Use as**: `/` (the system), `/boot`, swap, or `/drives/NAME` (a data disk).
- **Format**: keep the current file system or erase it.
- **New partition table** (GPT). One disk at a time.

Rules the editor enforces: `/` must be btrfs with the FreeLFS layout, `/boot` is
FAT32 and at least 200 MiB (an existing one may be kept), and data partitions are
ext4 or btrfs (or kept as they are).

When erasing, the summary asks you to tick a box before the button says
**Erase and install**.

### 5. Account

| Field | Rule |
|---|---|
| Full name | Free text |
| User name | Lower case letters, digits, `_` and `-`, starting with a letter or `_`, up to 32 characters. Names that already exist as a user, a group or a folder in `/home` are refused |
| Password | At least 4 characters, typed twice. The window shows it as squares and wipes it after sending it |

The new user is put in these groups: `users`, `seat`, `audio`, `video`, `render`,
`wheel` and `netdev`. `wheel` and `netdev` are groups the system uses for some
administration tasks (the system guide, chapters 10 to 19, covers them).

**Root's password is the same as the first user's password** in this version
(a one-line switch in the installer's code, `FI_ROOT_SAME_PASSWORD`). Change it
after the first boot with the usual `passwd` command if you want a different one.

### 6. Time zone

A world map and a search box, like the time zone page in Settings. The default
comes from the language you chose. Click a place on the map or type a city or a
country.

### 7. Summary

The helper writes its own list of what it will do: the partitions it makes, the
file systems, the copy and the boot setup. Erasing steps are shown in red. For
the default mode, tick the box to allow the erase, then press **Erase and install**.

Read the list once more before you press the button. There is no undo.

### 8. Install

The install runs in five steps with a progress bar:

1. Partition the disk.
2. Make the file systems (FAT32 for `/boot`, btrfs for the system, and the
   others you chose).
3. Copy the live system and write the settings (file systems, keyboard, language,
   time zone, the new account, and the boot files).
4. Put the boot loader and the kernel on the EFI partition.
5. Flush everything to the disk.

The last lines of each command are shown under the bar. When the install is
done, the window offers **Restart**. Remove the stick when the computer switches
off so it starts from the disk.

## What the installer does to the disk

For the whole-disk mode, the installer:

1. Writes a new GPT partition table with `sfdisk`. The partitions get random
   identifiers, and the installer finds them again by those identifiers.
2. Makes the file systems with `mkfs.fat`, `mkswap`, `mkfs.ext4` and `mkfs.btrfs`
   for the extra partitions.
3. Builds the btrfs system in a private, temporary area. The live system is read
   from its compressed image, the changes are applied, and one `mkfs.btrfs
   --rootdir` makes the file system with the three subvolumes. Nothing is
   mounted on the disk while this happens.
4. Writes the boot loader (`flboot`) and the kernel to the EFI partition, with a
   boot configuration that names the root partition.
5. Syncs the disk.

The changes made to the copied system include: the file system table
(`/etc/fstab`, by identifiers), the keyboard, the language, the time zone, the
new user and root's password, and the new user's home folder, made from
`/etc/skel`. The live user `tester` is removed from the new system.

## The layout on disk

```
disk (GPT)
  partition 1   512 MiB   EFI system partition   FAT32   mounted at /boot
  partition 2   the rest  btrfs                  subvolumes:
                                                   @           /
                                                   @home       /home
                                                   @snapshots  /.snapshots
```

Other partitions (swap, data disks) are added only in manual mode. The
`@snapshots` subvolume keeps the snapshots that the system takes before each
package change (see [09-troubleshooting.md](09-troubleshooting.md) for "Start an
earlier state").

The kernel, the boot configuration and a small microcode image sit on the EFI
partition. An installed system has no initial ramdisk for the root disk: the drivers
that disk needs are built into the kernel.

## First boot

1. The computer starts from the disk. The boot menu (flboot) shows the newest
   kernel first and the LTS kernel as a fallback. See [02-getting-started.md](02-getting-started.md).
2. The boot screen (flsplash) appears, then the login screen. Log in with the
   account you made.
3. The desktop starts. The keyboard is already set. There is no live-system
   question.
4. Known Wi-Fi networks, if you copied them, should connect by themselves. This
   was not yet verified on real hardware.

The installed system keeps its snapshots. Updates are taken with the Software
Store or `flpkg`, and each one first saves a snapshot.

Screenshots of an installed system: `../m7/m10-install-installed-1-login.png`,
`../m7/m10-install-installed-2-desktop.png` and `../m7/m10-install-installed-3-end.png`.

## Not supported and not tested

**Not supported (decided, not planned for now):**

- Dual boot with Windows, or with another Linux system's boot loader
- BIOS (legacy, non-UEFI) boot
- Disk encryption
- LVM and RAID
- A swap file of the installer's own

**Planned (see `docs/PLAN-installer.md`):**

- A map-based time zone picker with more keyboard layouts and a test field
- Hostname and auto-login fields
- A swap choice (zram, a swap file, or none)
- A hardware check before the install, an install log and a report button
- A welcome app on the first login
- App picks (Firefox, LibreOffice, VLC and others) installed after the install

**Not tested:**

- The real-disk path on a physical computer. The installer was tested in QEMU, onto
  a virtual disk file, and the installed system booted there. The project's test
  notes say the real-disk path, the setuid helper, the boot of the result and the
  restart are not covered by any test.
- "Install next to existing partitions" and "Manual" have not been run on a real disk.

## If the installer fails

- The install window shows a failure message and the last lines of the command
  that failed. Write them down, or take a screenshot.
- Nothing is written to a disk that you did not pick, and the installer checks
  each disk again before it writes.
- Make a report with `flreport` from the live system (Settings > About > Save a
  report) and copy it to a stick (see [07-files-drives-usb.md](07-files-drives-usb.md)).
- The disk may be partly written. Start the install again from the beginning;
  the default mode writes a new partition table, so it does not depend on what
  was there before.

See [09-troubleshooting.md](09-troubleshooting.md) for more.
