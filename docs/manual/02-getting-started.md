# 2. Getting started

> This chapter takes you from nothing to a running live session: what you need,
> how to get the image, how to put it on a USB stick, how the boot menu works,
> and what the live user can do.

## Contents

- [What you need](#what-you-need)
- [Get the live image](#get-the-live-image)
- [Put it on a USB stick](#put-it-on-a-usb-stick)
  - [Option A: write it with dd](#option-a-write-it-with-dd)
  - [Option B: Ventoy](#option-b-ventoy)
- [Start the computer from the stick](#start-the-computer-from-the-stick)
- [The boot menu (flboot)](#the-boot-menu-flboot)
- [The first login and the keyboard question](#the-first-login-and-the-keyboard-question)
- [The live session](#the-live-session)
- [Keeping your files: persistence (flpersist)](#keeping-your-files-persistence-flpersist)
- [Shutting down](#shutting-down)
- [If nothing shows up](#if-nothing-shows-up)
- [Next steps](#next-steps)

## What you need

| Item | Minimum | Notes |
|---|---|---|
| Computer | x86_64 (64-bit Intel or AMD) | No ARM support yet |
| Firmware | **UEFI** | Legacy BIOS boot is not supported |
| Secure Boot | **Off** | flboot and the kernel are not signed yet |
| Memory | 1 GB | 2 GB or more is comfortable, because the live session runs in RAM |
| Graphics | Any card the kernel drives | Only tested in QEMU and on one AMD laptop; Intel is untested |
| USB stick | 1 GB for the ISO written with dd | 4 GB or more if you want to keep files (see persistence below) |
| Network | Optional | Wi-Fi is set up from Settings > Wi-Fi; a cable works too |

The stick's whole contents are erased when you write the image to it. Use a
stick with nothing on it you need.

## Get the live image

The release file is a single ISO image, `freelfs-live*.iso`, about 843 MB. It
boots as a CD, can be written to a USB stick with `dd`, and can be copied to a
Ventoy stick as a file.

There is no public download page yet. The project plans a tester website
(see `docs/PLAN-installer.md`). Until then, get the file from someone who built
it, or build it yourself. The build is described in the developer chapters
(20 to 29). In short, the image is made by `build/53-iso.sh` and ends up in
`~/freelfs-work/freelfs-live.iso` on the build machine.

Before you write any image to a stick, check its checksum against the SHA-256
value published with it:

```sh
sha256sum freelfs-live.iso
```

If the two values differ, do not use the file.

## Put it on a USB stick

There are two ways. They behave differently, so choose with care.

| | Option A: `dd` | Option B: Ventoy |
|---|---|---|
| Stick afterwards | Only FreeLFS | Ventoy plus other ISO files you copy |
| Keep your files between sessions | **No**: there is no file system to hold them | **Yes**, with a persistence file (see below) |
| Secure Boot | Off | Off |
| Tested | QEMU, and written to a stick by the project | QEMU with Ventoy 1.1.18 (not on real hardware) |

### Option A: write it with dd

Find the device name of the stick. Use the whole disk (for example `sdb`), not a
partition (`sdb1`). Check the size so you do not pick the wrong disk:

```sh
lsblk
```

Then write the image, as root:

```sh
sudo dd if=freelfs-live.iso of=/dev/sdX bs=4M conv=fsync status=progress
```

Replace `/dev/sdX` with your stick. **dd does not ask twice.** If you name your
hard disk, that disk is erased. When dd finishes, unplug the stick.

### Option B: Ventoy

[Ventoy](https://www.ventoy.net) is a free tool that makes a USB stick start any
ISO file you copy onto it. Install Ventoy on the stick with its own tool (outside
FreeLFS), then:

1. Copy `freelfs-live*.iso` to the big data partition of the stick. Keep the name
   starting with `freelfs-live`: the live system looks for that pattern.
2. Start from the stick. Ventoy shows a list of ISO files. Pick the FreeLFS file
   and choose **Boot in normal mode**. This is the mode that was tested.
3. Secure Boot must be off.

Only Ventoy's normal mode has been tested. Its GPT mode and the other boot
modes have not.

## Start the computer from the stick

1. Plug in the stick and turn the computer on.
2. Open the firmware's boot menu. The key depends on the computer (often F12,
   F8, F9 or Esc; it is shown on the first screen). Choose the USB stick.
3. If the computer starts Windows or its own system instead, the stick is not
   chosen. Open the boot menu again, or change the boot order in the firmware
   setup, and make sure Secure Boot is off.

## The boot menu (flboot)

FreeLFS's boot loader is called **flboot**. It shows a short menu:

| Entry | When you see it |
|---|---|
| **FreeLFS live** | Always on the live stick. It starts after a 3 second countdown |
| **FreeLFS live (verbose: flive.debug)** | The same, with the boot messages printed on screen. Use it when something goes wrong |
| Kernel entries | On an installed system: the newest kernel first, then the LTS kernel as "FreeLFS (LTS kernel ...)" |
| **Start an earlier state** | Only when snapshots exist (installed systems) |

Keys:

| Key | What it does |
|---|---|
| Up, Down (or j, k) | Choose an entry |
| Enter | Start the chosen entry |
| Esc | Leave the earlier-states list |
| Any key | Stops the countdown, so the menu waits for you |

Starting an earlier state is explained in [09-troubleshooting.md](09-troubleshooting.md).

What happens after you start the live entry:

1. The boot screen (flsplash) shows the logo for a few seconds.
2. The system looks for the live image. It waits up to 20 seconds for disks.
   This wait can be longer for a slow stick. If the stick is not found, a short
   prompt appears with these keys: `r` tries again, `l` lists the disks, `p`
   powers the computer off, `b` reboots.
3. The login screen appears on the first screen.

## The first login and the keyboard question

The live user is called **tester**. There is **no password**. Pick the user on
the login screen and press Log in.

Before the desktop appears, the system asks which keyboard you use. The choices
are:

| Choice | Layout |
|---|---|
| `de` | German (QWERTZ) |
| `us` | US English (QWERTY) |
| `uk` | British English |
| `fr` | French (AZERTY) |

A preview of the keys and a field to try them are on the same screen. Continue
saves the choice to `/etc/keyboard.conf`. The live system asks at every new
boot, because the stick has no place to keep the answer.

The login screen has a "Shut down" button too.

Ctrl-Alt-F2 opens a plain text console with its own login. The live system gives
logins only on tty1 and tty2 (tty3 to tty6 are left out there to save memory).

## The live session

The live system is read-only. It is made from a compressed image on the stick.
Everything you change is kept in RAM, in a layer on top:

- Files in `/home/tester`, changed settings, installed programs, logs: **kept in
  RAM**, so they are gone when you shut down (unless you use persistence).
- The stick and the computer's disks: **not written**. The live system never
  touches the disks of the computer.
- Software Store: the Flathub catalogue is **not loaded** on the live system
  (it would take memory). The Store offers "Install FreeLFS" for installing
  to a disk and, as a second choice, "Try one small app anyway". The FreeLFS
  packages tab works only when a repository is set up (see [06-apps.md](06-apps.md)).
- A desktop icon, **FreeLFS install**, starts the installer. See
  [03-installing.md](03-installing.md).

A good first look:

| Try | Where | What to expect |
|---|---|---|
| The start menu | Super key, or the start button at the bottom left | Programs in categories, a search box at the top |
| Settings > Wi-Fi | Start menu, Settings | The networks around you (needs Wi-Fi hardware) |
| Settings > Keyboard | Start menu, Settings | The layout you chose, and the list of shortcuts |
| The Terminal | Super+T | The shell `flsh` with colours and suggestions |
| The web browser | Start menu, Internet, FreeBWS | Web pages in FreeBWS (see [06-apps.md](06-apps.md) for its limits) |
| Save a report | Settings > About | A file in your home folder for bug reports |

## Keeping your files: persistence (flpersist)

On a **Ventoy** stick you can keep your home folder between sessions. The live
system then stores `/home` in a file on the stick instead of RAM.

The file is `freelfs-persist.img`. It sits at the top of the stick's data
partition, or next to the ISO file. The live system finds it at the next boot and
uses it as `/home`.

There are two ways to make it:

1. **Settings > About**, in the "Live system" section: the button
   **Keep my files on this USB stick** creates a 4 GB file. A line says it needs 4 GB
   free on the stick, and it takes a few minutes. Cancel leaves everything as it is.
2. In a terminal: `flpersist create 4G`. Use `--btrfs` for a btrfs file system
   instead of ext4. `flpersist status` says `none`, `ready` (the file exists and is
   used from the next boot) or `active`.

After it is made, restart the live system to use it. Only `/home` is kept; the
system itself is still the read-only image.

Notes:

- A stick written with `dd` has no file system to hold the file, so persistence
  does not work with it.
- To start without the file, add `flive.nopersist` to the kernel command line
  in the boot menu. Everything then stays in RAM again.
- Flatpak apps you install on the live system live in your home folder, so they
  are kept too.

## Shutting down

Use the **start menu > Shut down** (or Restart, or Log out). A 30 second countdown
gives you the chance to cancel.

Everything in RAM is lost when you shut down, unless you have persistence. Copy
the files you want to keep to a USB stick first, with the Files app (see
[07-files-drives-usb.md](07-files-drives-usb.md)). The home folder is in RAM, so
`flreport` also says so.

**Known bug:** shutdown failed on the laptop in round 17. The fix is part of
round 18 and was not yet verified on that laptop when this chapter was written.
If the computer does not power off, wait a minute, then hold the power button.
Report it with `flreport` (see [09-troubleshooting.md](09-troubleshooting.md)).

## If nothing shows up

| Symptom | Try |
|---|---|
| The stick does not appear in the boot menu | Check the boot-menu key and that Secure Boot is off. Try another USB port |
| The menu appears but the live entry does not boot | Choose "FreeLFS live (verbose: flive.debug)" and read the messages |
| "Could not find the live system" or the prompt with `r`, `l`, `p`, `b` | Press `r` to retry. Press `l` to see the disks the system found |
| The screen goes black after the boot screen | The graphics card is not supported yet. Try the verbose entry, and see [09-troubleshooting.md](09-troubleshooting.md) |
| Nothing works after login | Ctrl-Alt-F2 opens a console. Run `flreport` there, or see chapter 9 |

## Next steps

- [03-installing.md](03-installing.md): install FreeLFS on a disk.
- [04-the-desktop.md](04-the-desktop.md): the desktop, panel and tray.
- [09-troubleshooting.md](09-troubleshooting.md): common problems.
- Back to [01-introduction.md](01-introduction.md) or the table of contents in
  [README.md](README.md).
