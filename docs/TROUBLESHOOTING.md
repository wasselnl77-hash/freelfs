# FreeLFS troubleshooting

> Problem, cause, fix. Written for people who try FreeLFS on a real computer
> or in a virtual machine. It is based on problems that testers actually hit
> (the laptop reports and the bug scans of 13 and 14 November 2026), and on what
> the code and the round notes say about them.
>
> FreeLFS is **pre-alpha**. Read the warning in [README.md](../README.md) first.
> This guide does not replace the user manual. Its short problem table and the
> basic steps are in [manual/09-troubleshooting.md](manual/09-troubleshooting.md).
> Read that first for the everyday cases. This guide goes deeper.

Written 2026-11-17 from the round notes up to round 22, the bug scans, the
hardware notes and the five laptop reports (the last one is from 17 November).

## Contents

- [How to read this guide](#how-to-read-this-guide)
- [1. Boot](#1-boot)
  - [1.1 "Verification failed" or "Security violation" (Secure Boot, Ventoy)](#11-verification-failed-or-security-violation-secure-boot-ventoy)
  - [1.2 F2 and F12 do nothing (fast boot)](#12-f2-and-f12-do-nothing-fast-boot)
  - [1.3 A black screen](#13-a-black-screen)
  - [1.4 NVIDIA graphics](#14-nvidia-graphics)
  - [1.5 The boot splash is short](#15-the-boot-splash-is-short)
- [2. The USB stick](#2-the-usb-stick)
  - [2.1 "Permission denied" when saving to the stick](#21-permission-denied-when-saving-to-the-stick)
  - [2.2 Eject refused: "FreeLFS is running from this stick"](#22-eject-refused-freelfs-is-running-from-this-stick)
  - [2.3 Load into RAM](#23-load-into-ram)
  - [2.4 Files are 0 bytes after copying](#24-files-are-0-bytes-after-copying)
  - [2.5 The stick does not show, or gives input/output errors](#25-the-stick-does-not-show-or-gives-inputoutput-errors)
- [3. Wi-Fi](#3-wi-fi)
- [4. Bluetooth, sound and printing](#4-bluetooth-sound-and-printing)
- [5. Display, scaling, night light and trails](#5-display-scaling-night-light-and-trails)
- [6. FreeBWS: white page, freeze and Google sign-in](#6-freebws-white-page-freeze-and-google-sign-in)
- [7. Firefox](#7-firefox)
- [8. RAM looks high on a live system](#8-ram-looks-high-on-a-live-system)
- [9. Shutdown and restart do not finish](#9-shutdown-and-restart-do-not-finish)
- [10. Installing](#10-installing)
- [11. Software: flpkg, Flathub, Nix, Distrobox](#11-software-flpkg-flathub-nix-distrobox)
- [12. Making a good bug report](#12-making-a-good-bug-report)
- [13. FAQ](#13-faq)
- [14. Open questions](#14-open-questions)
- [15. Login, root, passwords and the firewall (round 23)](#15-login-root-passwords-and-the-firewall-round-23)

## How to read this guide

Each section has four parts:

- **Symptoms**: what you see.
- **Likely causes**: what the notes and the code say.
- **Fix**: steps to try, in order. Commands are typed in a terminal (open one from
  the start menu, or press Ctrl+Alt+F2 for a text console).
- **Send in a bug report**: what to attach. See [section 12](#12-making-a-good-bug-report).

Status words used in this guide:

| Word | Meaning |
|---|---|
| **Fixed in code** | A change is in the tree. It was tested in QEMU or with a headless test, but the notes do not say it was checked on a real laptop |
| **Open** | Known, and no fix is in the notes |
| **Not verified** | A step or claim that the notes do not confirm on real hardware. Try it, but do not rely on it |
| **Planned** | Written in the roadmap, not built |

Paths in commands: `~` is your home folder. On the live system your home is in RAM
(see [section 8](#8-ram-looks-high-on-a-live-system)).

---

## 1. Boot

### 1.1 "Verification failed" or "Security violation" (Secure Boot, Ventoy)

**Symptoms**

- The firmware shows a message such as "Verification failed", "Security Violation"
  or "Access Denied". The exact words depend on the maker. The stick does not start.
- With a Ventoy stick, a blue screen called MokManager may appear first. It asks you
  to enroll a key. This is Ventoy's own key, not FreeLFS's.
- Windows asks for a BitLocker recovery key after you change the firmware settings.

**Likely causes**

- **Secure Boot is on.** FreeLFS does not support Secure Boot yet. `flboot` and the
  kernels are not signed, and there is no signed shim. This is in
  [manual 15, section 15.10](manual/15-users-security.md) and in
  [HARDWARE.md](HARDWARE.md) ("Secure Boot").
- **Ventoy needs its own key enrolled** before it starts under Secure Boot. On one
  laptop, Ventoy did not start until that key was enrolled. Enrolling it only starts
  Ventoy. FreeLFS would still be refused, so this does not solve the problem.
- **BitLocker** (Windows): changing Secure Boot or the boot order can make Windows
  ask for its recovery key. This is general Windows behaviour. The project has not
  tested it with FreeLFS.

**Fix**

1. Save the BitLocker recovery key before you change anything. Find it in your
   Microsoft account (account.microsoft.com, Devices) or on paper where you stored it.
   Do this first if the computer has Windows with BitLocker on.
2. Restart and open the firmware setup. The key is often F2, F10, Delete or Esc.
   The name of the option differs by maker. Look for **Secure Boot**.
3. Set **Secure Boot** to **Disabled**. Some firmware also needs the Secure Boot keys
   cleared or the mode set to "Other OS" first. Not verified for every maker.
4. Save and exit. Then start the stick, and choose the FreeLFS entry in the boot menu.
5. Do not enroll Ventoy's key to make FreeLFS start. Secure Boot must be off for FreeLFS.

**Send in a bug report**

- The computer's maker and model, and the firmware version (shown in the firmware setup).
- A photo of the exact message on screen.
- Whether Secure Boot was off when the stick started (yes or no).

### 1.2 F2 and F12 do nothing (fast boot)

**Symptoms**

- You press F2, F12 or Esc at power-on, but the firmware's setup or boot menu does
  not open. The computer goes straight to the system.

**Likely causes**

- **Fast boot** in the firmware skips the short window in which the keys are read.
  This is common on recent laptops. The manual says the same
  ([manual 02, "The boot menu"](manual/02-getting-started.md)).

**Fix**

The FreeLFS boot menu (flboot) has two entries for this, added in round 21:

1. Restart the computer. On an **installed** system the boot menu stays for 5 seconds.
   Press any key (Space is fine) during those 5 seconds to keep it open.
2. Choose **Firmware setup (BIOS)**. The computer restarts into the firmware's setup
   screen. This works only when the firmware supports it, which the menu checks.
3. To start a USB stick instead, choose **Other drives and systems**, then the stick.
   The computer restarts into the firmware's own boot choice.

On the **live stick** the countdown is 3 seconds. Any key stops it, so the menu waits.

Permanently: in the firmware setup, turn off **Fast Boot** (the name differs by
maker). Not verified on FreeLFS.

Status: **Fixed in code.** It was tested in QEMU with OVMF (`build/m21-flboot.sh`).
It is **not verified** on a real laptop with fast boot.

**Send in a bug report**: the model, and whether the menu showed the two entries.

### 1.3 A black screen

**Symptoms**

- The boot menu shows, then the screen goes black. Or the desktop never appears,
  and a text console does not respond either.

**Likely causes**

- The graphics driver hangs. Intel graphics are **untested** on real hardware. AMD
  is the tested case. NVIDIA is covered in [1.4](#14-nvidia-graphics).
- A kernel message stops the display. The manual's "verbose" entry shows the messages.

**Fix**

1. On the **installed** system, restart. In the boot menu choose
   **FreeLFS (Safe graphics, kernel ...)**. It starts with `nomodeset` (no GPU driver).
   The screen then uses the firmware's plain framebuffer. [HARDWARE.md](HARDWARE.md)
   says this entry is always in the menu. Not verified on every machine.
2. On the **live stick**, choose **FreeLFS live (verbose: flive.debug)**. The boot
   messages stay on the screen, so you can see where it stops. Take a photo of the
   last lines.
3. Once you are in a text console (Ctrl+Alt+F2), look at the graphics messages:

   ```sh
   dmesg | grep -i -E 'drm|amdgpu|i915|xe|nouveau|simpledrm' | tail -40
   ```

Status: the Safe graphics entry is **fixed in code** (round 20). The live menu entry
for it is **not verified**.

**Send in a bug report**: the photo of the verbose screen, the `dmesg` lines above,
and `flreport` if you got to the desktop.

### 1.4 NVIDIA graphics

**Symptoms**

- A black screen after boot on an NVIDIA machine. Or a desktop that is very slow.

**Status**

- **Phase 1 is in the round-20 build** (nouveau driver with KMS, a plain framebuffer
  fallback, and the "Safe graphics" entry). It was **never run on a real NVIDIA card**.
  QEMU cannot emulate one.
- **Planned**: NVK, the open Vulkan driver (phase 2, needs the 16 GB build machine).
  A proprietary NVIDIA driver is only a possible later package (phase 3). It is not
  in FreeLFS, and you should not install a driver from elsewhere into FreeLFS.

**Fix**

1. Choose **Safe graphics** in the boot menu (see [1.3](#13-a-black-screen)).
2. On the desktop, find out which card the desktop uses:

   ```sh
   grep "display on" "$XDG_RUNTIME_DIR/flcomp.log"
   ```

   The line says `GPU driver` or `firmware framebuffer`. The firmware framebuffer means
   no GPU driver was used.
3. Check the driver in use:

   ```sh
   lspci -nnk | grep -A3 -i -E 'vga|3d'
   ```

   Look for `Kernel driver in use: nouveau`.
4. Look for firmware or GSP messages:

   ```sh
   dmesg | grep -i -E 'nouveau|gsp|firmware' | tail -50
   ```

**Send in a bug report**: the card's name (`lspci -nnk` line), the screen photo, the
`dmesg` lines above, and `flreport`. An NVIDIA tester is wanted: the notes say phase 1
has not yet been checked on any NVIDIA card.

### 1.5 The boot splash is short

**Symptoms**

- The FreeLFS logo appears for only a second or two, then the login screen or the
  desktop.

**Likely causes**

- An earlier build stopped the splash after 1 to 2 seconds. That was report 3.
- The current code holds the splash for **at least 3 seconds** from the first picture
  (`T_MIN 3000` in `src/flsplash/flsplash.c`). A fade of 0.3 s follows. A failing boot
  step is only logged to `/run/flsplash.log`. It does not switch the screen to text.
- The 17 November report (report 5) shows the splash ran for about 4.4 seconds before
  it released the screen. Its log is `/run/flsplash.log`, which `flreport` collects.

**Fix**

- Update to round 20 or newer. The version is in `/etc/os-release` (the report
  shows `VERSION_ID=0.1-dev`, which does not tell the round). Compare the build date of
  your image with the round notes.
- To see boot messages instead of the splash, press **Esc** during boot. The boot
  scripts say Esc shows the text (`config/etc/rc.d/functions`). Not verified by a
  tester.
- Check the log:

  ```sh
  cat /run/flsplash.log
  ```

Status: **Fixed in code** (rounds 18 and 20). One laptop report shows the expected
length. A second report of a short splash on the newest build is not known.

**Send in a bug report**: `flreport`, and the time from the start of the logo to the
login screen (a phone video is fine).

---

## 2. The USB stick

These problems come from the live system running from a stick. Read the manual's
[USB chapter](manual/07-files-drives-usb.md) for the basic steps. The notes here
explain the failures that testers reported.

### 2.1 "Permission denied" when saving to the stick

**Symptoms**

- Saving, copying or creating a folder on the stick gives "Permission denied".
- The stick's files belong to `root`.

**Likely causes**

- On a Ventoy stick the system runs from an exFAT or FAT partition on the stick. Those
  file systems fix the owner at the first mount. The boot code used to mount them as
  root. Round 21 mounts them with `uid=1000,gid=98` (the live user `tester`, group
  `seat`). This was reproduced in QEMU with a virtual Ventoy stick.
- Report 2 said that unplugging and plugging the stick back fixed it. Round 20 found a
  lock in the mount code that made a second plug-in wait. The notes link the replug
  workaround to that lock, but do not prove it for every report.

**Fix**

1. Check the mount options:

   ```sh
   grep /drives /proc/mounts
   ```

   For an exFAT or FAT stick, `uid=1000` should appear. If it does not, go to step 2.
2. Eject the stick in Files, unplug it, wait 5 seconds, and plug it in again.
3. If it still fails, list the drives and remount the one you need. `DEV` is the
   device name from the list, for example `/dev/sda1`:

   ```sh
   flmount list
   flmount remount DEV
   ```

4. If the stick has ext4 or btrfs, the owners stored on the stick are kept. Files
   written by root stay root's. Not verified for FreeLFS. Use another computer to fix
   the owners, or reformat the stick (copy your files first).

Status: **Fixed in code** (round 21). Report 5 (17 November) ran round 21 with Load into
RAM active, and its stick mount had `uid=1000`. Without Load into RAM, the stick is
the boot stick, and Files refuses to eject it (see [2.2](#22-eject-refused-freelfs-is-running-from-this-stick)).

**Send in a bug report**: the output of `grep /drives /proc/mounts` and `flmount list`.

### 2.2 Eject refused: "FreeLFS is running from this stick"

**Symptoms**

- Files refuses to eject, unmount or remount the stick. It shows:
  `FreeLFS is running from this stick. Shut down first, or boot with 'Load into RAM'.`

**Likely causes**

- This is correct behaviour. The live system runs from files on the stick. Removing
  the stick would break every program that is not yet in memory. This was the cause
  of the "Eject, then apps do not open" reports, according to the notes.

**Fix**

- Shut down (the power button in the start menu), then remove the stick.
- To remove the stick **while you keep working**, start the system with
  [Load into RAM](#23-load-into-ram). Then the stick is released.

**Do not** pull the stick out to test it. Use the Shut down button.

Why the same stick behaves differently:

- **Without Load into RAM**, the system runs from files on the stick. The stick is the
  "FreeLFS boot stick", and Files refuses to eject, unmount or remount it.
- **With Load into RAM**, the system is copied into RAM and the stick is released. It then
  becomes a normal removable drive, under `/drives/LABEL`, and it can be ejected like any
  other stick. Report 5 (17 November) ran with Load into RAM active, so its stick
  was listed as "removable" and mounted under `/drives/Ventoy`. That is correct.

### 2.3 Load into RAM

**What it does**

- Copies the system image (about 0.9 GB) into RAM and releases the stick. After that
  you can remove the stick. The notes say the copy costs about 0.9 GB of RAM.

**How to start it**

- Choose **FreeLFS live (Load into RAM)** in the boot menu, or add `flive.toram=1` to
  the boot command line.
- It runs by itself when the machine has at least the image size plus 4 GiB of RAM.
  It never runs by itself on a 1 GB machine. It does not run by itself with a
  persistence file (`freelfs-persist.img`), because that file is your home folder.
- `flive.toram=0` forbids it.

**Check it worked**

```sh
ls /run/live/toram
```

The file exists when the system was loaded into RAM.

**Limits**

- Anything you saved on the live system is lost at shutdown unless you copied it to
  a stick. The home folder is in RAM. See [section 8](#8-ram-looks-high-on-a-live-system).

Status: **Fixed in code** (round 21). Tested in QEMU only. **Not verified** on a
laptop.

### 2.4 Files are 0 bytes after copying

**Symptoms**

- A file copied to the stick shows size 0 after you unplug it.
- A screenshot saved on the stick is empty.

**Likely causes**

- The stick was pulled before the data was written. The code comments in the
  screenshot tool and in Files say this happens when a stick is pulled soon after
  saving.
- Round 22 added a flush (fsync) before a screenshot or a copy to an exFAT stick. This
  reduces the risk. It does not remove it.

**Fix**

1. **Always eject first.** In Files, click the stick's eject button, and wait for the
   notice that the stick can be removed. Then unplug it.
2. Check the sizes:

   ```sh
   find /drives -type f -size 0 2>/dev/null
   ```

   Any file listed is empty. Copy it again, then eject.
3. Do not start a second copy while the first one is still running.

**Send in a bug report**: the output of the `find` above, the steps (did you eject?),
and the stick's file system (Files shows it in the drive's details).

### 2.5 The stick does not show, or gives input/output errors

**Symptoms**

- A stick plugged in after boot does not appear in Files.
- "Input/output error" after you plug the stick in, or in a folder on the stick.
- A stick works only after you unplug and plug it in again.

**Likely causes**

- Files shows drives under `/drives`. The stick is mounted by the desktop after you
  plug it in, not at boot. A notice "USB stick connected" should appear. Click it to mount.
- An "input/output error" right after plugging in is often a stick that is not ready.
  The mount code tries again up to three times (round 20).
- A lock in the mount code made a second plug-in wait up to 70 seconds. Round 20 fixed
  it. The notes link the older "replug and it works" reports to this lock, not proven
  for every case.
- **NTFS sticks are not supported.** Use FAT32, exFAT, ext4 or btrfs.

**Fix**

1. Wait 5 seconds after plugging in.
2. List the drives:

   ```sh
   flmount list
   ```

   The line shows the device, the label, the file system and the mount point.
3. If the stick is listed but not mounted, click it in Files, or run
   `flmount remount DEV` (for example `flmount remount /dev/sda1`).
4. If a folder gives "input/output error" in Files, Files offers to run the remount.
   Accept it.
5. If nothing helps: unplug the stick, shut down, and use another USB port.

**Send in a bug report**: the output of `flmount list`, and the file system of the stick.

---

## 3. Wi-Fi

**Symptoms**

- The Wi-Fi page says "No Wi-Fi hardware found", or no Wi-Fi network is listed.
  The full message table is in [manual 09](manual/09-troubleshooting.md#wi-fi).
- `ip link` shows only `lo`.

**Likely causes**

- The kernel has no driver for the chip, or its firmware file is missing.
- A driver is loaded but not bound to the card (flreport prints
  "NO DRIVER BOUND" for such a card).

**Diagnose**

Run these in a terminal and keep the output:

```sh
lspci -nnk | grep -A3 -i -E 'network|wireless'
ip link
ls /sys/class/net
dmesg | grep -i firmware | tail -40
dmesg | grep -i -E 'iwlwifi|rtw|mt79|mt76|ath1|brcm' | tail -40
tail -40 /var/log/iwd.log
```

How to read it:

| What you see | What it means |
|---|---|
| `lspci` shows a network card, and `Kernel driver in use:` names a driver | The driver loaded. The problem is later (network service, or the Wi-Fi page) |
| `lspci` shows the card with no `Kernel driver in use:` line | No driver bound. Note the `[vendor:device]` id, and report it |
| `dmesg` says `failed to load firmware` or `Direct firmware load ... failed` | The firmware file for the chip is missing. Report the file name it asks for |
| `ip link` shows `wlan0` (or another name) | The interface exists. Connect from the Wi-Fi page |
| `/var/log/iwd.log` ends with an error about the network | The problem is the network or the password, not the driver |

**Chips FreeLFS has drivers for** (from [HARDWARE.md](HARDWARE.md), round 20):

| Family | Chips named in the notes | Status |
|---|---|---|
| Realtek | RTL8852BE (PCI id 10ec:b85b, driver `rtw89_8852be`), other rtw89 and rtw88 chips, rtl8xxxu (USB), rtlwifi (old PCIe) | **The RTL8852BE came up** on an AMD Ryzen 5 7520U laptop (report 5, 17 November: `wlan0` up, with an address from the router). Others not verified on hardware |
| Intel | iwlwifi family (iwldvm, iwlmvm, iwlmld for Wi-Fi 7) | Modules present, **not verified on Intel hardware** |
| MediaTek | MT7921, MT7922, MT7925, mt76 family | Modules present. MT7922 and MT7925 **wanted from testers** |
| Qualcomm Atheros | ath9k, ath10k (QCA9377, found in an old Acer laptop), ath11k, ath12k | Modules and firmware present. **Not verified on hardware** |
| Broadcom | brcmfmac | Modules present. **Not verified on hardware** |

A card that is not in this list is not known to work. Report its id.

**Fix**

- If the driver is bound and the firmware loaded, connect from **Settings > Wi-Fi**.
  If a known network does not connect at once, connect once by hand from that page.
- If no driver is bound, there is no fix in the notes. Send the report.
- Do not copy firmware or drivers from another distribution into FreeLFS. They are not
  part of the tested setup.

**Send in a bug report**: `flreport` (it lists the network controllers, their drivers
and the firmware lines), and the output of `lspci -nnk` for the network card.

---

## 4. Bluetooth, sound and printing

### 4.1 Bluetooth

**Symptoms**

- The Bluetooth flyout shows no adapter, or the adapter is off.
- A device does not pair.

**Likely causes**

- The Bluetooth service (`bluetoothd`) starts only when the kernel sees an adapter
  (`/sys/class/bluetooth`). A udev rule starts it (round 17).
- On the AMD laptop of report 5, the kernel printed
  `Direct firmware load for rtl_bt/rtl8852bu_config.bin failed`. The bug scan of
  14 November found the kernel treats this file as optional. The notes do not say
  whether Bluetooth worked on that laptop.
- A default pairing agent is set up for one-shot pairing with "no input, no output".
  Devices that need a PIN may not pair. Not verified.

**Fix**

```sh
ls /sys/class/bluetooth
dmesg | grep -i bluetooth | tail -30
cat /var/log/bluetooth.log
bluetoothctl show
bluetoothctl power on
bluetoothctl scan on
```

- `bluetoothctl show` should print `Powered: yes`. If it does not, run `bluetoothctl power on`.
- Stop the scan with `bluetoothctl scan off`.
- Pair from the Bluetooth flyout. Pairing that needs a PIN may fail (see above).

The bluetooth log may contain `cups: command not found`. The bug scan found no script
that calls `cups`, and the case was not reproduced. It is harmless as far as the notes show.

Status: **Round 17 work.** Not verified on real Bluetooth hardware in the notes.

**Send in a bug report**: `/var/log/bluetooth.log`, the `dmesg` lines, and the device's
name and kind (headset, keyboard, phone).

### 4.2 Sound

**Symptoms**

- No sound, or sound only on one output.

**Fix**

1. Click the volume icon in the panel. Check the mute button.
2. Check the output list:

   ```sh
   wpctl status
   cat /proc/asound/cards
   ```

   The output marked `*` is the one in use. An empty **Sinks** list means the audio
   service did not start. Look at `$XDG_RUNTIME_DIR/session.log`.
3. Set the volume and unmute from a terminal to test:

   ```sh
   wpctl set-mute @DEFAULT_AUDIO_SINK@ 0
   wpctl set-volume @DEFAULT_AUDIO_SINK@ 0.5
   ```

4. Check whether a Bluetooth headset took the sound (the Bluetooth flyout shows it).
5. The "System sounds" switch in Settings > General only affects the short event sounds.

The logs of report 5 showed messages from RealtimeKit (`RTKit error: ServiceUnknown`)
and from an optional audio plugin (`api.alsa.acp.device could not be loaded`). Report
5's sound list still showed a speaker output. These messages have no symptom in the
notes. Ignore them unless audio crackles under load. Not verified.

**Send in a bug report**: `wpctl status`, `cat /proc/asound/cards`, and `session.log`.

### 4.3 Printing

**Status**

- CUPS 2.4.20 is installed. It starts on demand when a program asks for a printer, and
  stops after 60 seconds without use (manual 14).
- Printer discovery uses the network (avahi). Driverless (IPP Everywhere) printers are
  the first choice.
- **Not verified** on a real printer. The notes have no real-printer test.

**Fix**

1. Open **Settings > Printers**. Add the printer it finds. Set it as default if you want.
2. Print a test page from the same page.
3. If the printer is not found, check that it is on the same network, then add it by
   its address. From a terminal:

   ```sh
   lpstat -p -d
   lpstat -t
   ```

   These commands come with CUPS. If they are missing, printing is not installed.

**Send in a bug report**: the printer's make and model, the connection (network or USB),
and `lpstat -t`.

---

## 5. Display, scaling, night light and trails

### 5.1 Wrong resolution, or the screen looks wrong

**Fix**

1. Open **Settings > Display**. Change the mode or the scale (100 to 200 percent).
2. Press **Apply**. Within 15 seconds press **Keep these settings?**. If you do not,
   the old mode comes back. This is the safety net for a mode the screen cannot show.
3. To see the connectors and their state in a terminal:

   ```sh
   grep -H . /sys/class/drm/card*-*/status
   ```

   `connected` means a screen is plugged in on that connector.

Status: working as described in the manual. Tested on the QEMU display and on the
AMD laptops. Intel is **not verified**.

### 5.2 Night light

- **Settings > Display > Night light**: Off, On or Auto. Warmth from 2700 K (very warm)
  to 6500 K (normal). Auto uses the From and To times.
- On a plain framebuffer (for example after Safe graphics) the night light is drawn by
  software. The log says `night light is done in software`.

Check the log:

```sh
grep -i "night light" "$XDG_RUNTIME_DIR/flcomp.log"
```

If the night light does nothing on a GPU driver, report it with this line.
Status: **Fixed in code** (round 17); the software path ran in QEMU. **Not verified**
on laptop GPUs.

### 5.3 Trails on the screen

**Symptoms**

- A rectangle or outline stays on the desktop after you drag a selection box (rubber-band
  selection). Or other pieces of old windows stay visible.

**Likely causes**

- The desktop icon layer did not replace every pixel of its buffer. Fixed in round 20.
  Reproduced in QEMU, and reported on an Intel and an AMD laptop.

**Fix**

- Update to round 20 or newer. If the trail still appears, note the steps (what you
  dragged, and whether the trail stays after a click elsewhere) and send it.

Status: **Fixed in code.** Not verified on the laptops that reported it.

### 5.4 A window has its own title bar, or the title bar is not the FreeLFS one

**Symptoms**

- Firefox, some GTK apps, and some Wayland apps show their own title bar and buttons,
  not the FreeLFS one.

**Likely causes**

- A Wayland window that does not ask for server-side decorations draws its own bar.
  The compositor only adds a frame to X11 windows (Xwayland) that need one. The log
  shows this for each window, for example
  `window "fledit" (Wayland): no xdg-decoration object, the client draws its own title bar`.
- Firefox from Flathub uses GTK 3. The log of report 5 (17 November) shows its window
  with no decoration object, so Firefox drew its own bar.

**Fix**

- None in the notes. This is a known limit, not a bug that is planned to be fixed.
  Use the window's own buttons.

### 5.5 Sliders do not respond

**Symptoms**

- A slider in a settings window does not move, or does not follow the mouse.

**Status**

- Report 3 said that sliders still did not work. Round 20 fixed scrollbar dragging in the
  toolkit. The notes do not record a slider fix. **Open**, not verified.
- Use the arrow keys or click on the bar next to a slider as a workaround, if the
  window supports it.

**Send in a bug report**: the window (Settings page name) and whether the value changes
at all.

---

## 6. FreeBWS: white page, freeze and Google sign-in

FreeBWS is the built-in browser. It is based on WPE WebKit. This section is long because
it has had the most reports.

### 6.1 History

| Report or round | What testers saw | What was done | Status |
|---|---|---|---|
| Report 1 | "FreeBWS buggy, still not working" and Firefox "worked flawlessly" | Not specific | Open |
| Report 2 | The page froze at the end of a Google login. Rebooting fixed it | Round 19: the cause was not found. One hardening was made | The cause was not proven on the laptop |
| Round 20 | A frame watchdog, and a log at `~/.cache/freebws/log` | A page that loads for 8 seconds without a new frame for 5 seconds is restarted on the same page. Buffers switch to shared memory (`shm`) | Fixed in code |
| Report 3 (white screen), and the claude.ai white page | A white screen after the login | Round 21 reproduced a freeze and white page in a VM with a test sign-in site. The cause: `wpebackend-fdo` 1.16.1 did nothing for a surface call, so closing a view ended the whole Wayland connection of that web process, and the other views stopped painting. Fixed by a patch (the surface-feedback patch) | **Fixed and verified on the real AMD laptop in round 21.** The user confirmed it works ("IT WORKS") |
| Report 5 (17 Nov) | The Google sign-in pages loaded in the log. The notes record no freeze in that report | None | Not verified |

### 6.2 A white page or a freeze

**Symptoms**

- A page stays white, or stops redrawing, while the address bar and tabs still work.
- A freeze after a Google or other sign-in window.

**Fix**

1. Wait about 10 seconds. The watchdog restarts a page that stops painting.
2. Reload the page with Ctrl+R.
3. Run FreeBWS from a terminal and keep the errors:

   ```sh
   freebws 2>~/fb.log
   ```

   Then read `~/fb.log` (`less ~/fb.log`).
4. Read the FreeBWS log:

   ```sh
   less ~/.cache/freebws/log
   ```

   Look for `falling back to shm buffers` (the browser switched to the safer drawing
   path) and for lines about watchdogs or hung web processes.
5. For more detail, start with the debug variable. It writes every load event to the
   same log:

   ```sh
   FREEBWS_DEBUG=1 freebws 2>~/fb.log
   ```

6. Force the safer drawing path for one run:

   ```sh
   FREEBWS_BUFFERS=shm freebws
   ```

   The browser also writes `buffers=shm` to its settings file when the watchdog
   restarts it. The settings file is `~/.config/freebws/config`.

**Test only**: `FREEBWS_SANDBOX=off freebws` turns off the web process sandbox. Use it
to find out whether the sandbox causes the problem, and turn it back on afterwards.
It is not a fix. To check whether the sandbox works on your computer, run
`freebws --sandbox-check`.

**Send in a bug report**: `~/.cache/freebws/log` and `~/fb.log` (if you made it), the
address of the page, and the steps. Note whether the freeze came after a sign-in.

### 6.3 Google sign-in (and other sign-ins)

**Symptoms**

- "Continue with Google" does nothing, or the sign-in window opens and then stays blank.
- The sign-in finishes in another window, but the first window never returns.

**Likely causes (not all confirmed)**

The bug scan of 13 November, which read the code and did not run it, found these
possible causes. Round 21 notes say the popup problem is partly addressed.

- A popup that the page opens after a delay (for example after a `fetch`) may be blocked
  by WebKit. The page then waits for a window that never appears.
- A sign-in window that stays in the background for more than 10 minutes, or any
  background tab under memory pressure, can be discarded. Its opener then loses the
  link to the popup. The settings `FREEBWS_DISCARD_IDLE` and `FREEBWS_DISCARD_MB` control
  this (see the test variables below).
- The browser does not set its own user agent. Some sign-in pages treat an unknown
  engine as untrusted. Not verified whether Google refuses this engine.
- The ad blocker's helper script replaces `window.fetch`. Bot checks can notice that.

**Test variables** (for a laptop test, not as a fix; they change the behaviour):

```sh
FREEBWS_DISCARD_IDLE=0 FREEBWS_DISCARD_MB=0 freebws 2>~/fb.log
FREEBWS_ADB_SKIP=yt freebws 2>~/fb.log
FREEBWS_EVAL=navigator.userAgent freebws 2>~/fb.log
```

- `FREEBWS_DISCARD_IDLE=0 FREEBWS_DISCARD_MB=0` turns off the discarding of background tabs.
- `FREEBWS_ADB_SKIP=yt` turns off the script that replaces `window.fetch`.
- `FREEBWS_EVAL=navigator.userAgent` prints the engine's user agent to the log.

**Fix**

- If the sign-in does not work or freezes, use **Firefox from Flathub** for that site
  (section 6.4 and [section 7](#7-firefox)).

### 6.4 When to use Firefox from the Store instead

Use **Firefox** from the Software Store (Flathub) when:

- a sign-in (Google, Microsoft, or any site with a second window) freezes or does not
  return, even after the steps above,
- a site shows a challenge page (for example Cloudflare Turnstile) that does not finish,
- you need a browser extension, or a feature FreeBWS does not have.

The manual lists these cases in [manual 09](manual/09-troubleshooting.md#the-web-browser-cannot-log-in-to-a-site).
FreeBWS is not removed. Keep both.

On an installed system:

```sh
flpkg search firefox
flpkg install --from=flatpak org.mozilla.firefox
```

On the live system the Flatpak files go into RAM, and `flpkg` warns you. See
[section 11](#11-software-flpkg-flathub-nix-distrobox).

---

## 7. Firefox

Firefox is a Flatpak app from Flathub. It runs with the portals: the file chooser and the
notifications go through FreeLFS's own services.

### 7.1 Maximize

**Symptoms**

- Firefox does not fill the screen when you click its maximize button or double-click
  its title bar.

**Status**

- Round 21 tested the same kind of title bar (a GTK-style test client) and it maximized,
  went full screen, moved and resized. **Not reproduced** with Firefox itself.
- Firefox draws its own title bar (see [5.4](#54-a-window-has-its-own-title-bar-or-the-title-bar-is-not-the-freelfs-one)).

**Fix**

- Drag the window to the **top edge** of the screen. The desktop maximizes it, with an
  outline shown first. Drag to the **left or right edge** to snap the window to a half.
- Or use Firefox's own maximize button in its title bar.

**Send in a bug report**: the steps, and the `flcomp.log` line for the Firefox window.

### 7.2 The file picker: clicking "+" does not open a file chooser

**Symptoms**

- Report 5 said that clicking "+" did not open the Files app. The notes do not say which
  "+" button was meant.
- A file upload from Firefox does not open a window, or the window does not respond to
  clicks.

**Likely causes**

- Firefox asks the portal for a file chooser. Round 21 found two bugs in FreeLFS's
  portal chooser: rows did not take clicks, and the window was not redrawn after an
  action. Both were fixed and tested in QEMU (`build/m21-features.sh portal-folder`).
- The chooser is the portal's own dialog, not the Files app. Files is a different program.

**Fix**

1. Try the upload again in Firefox. The dialog should be a FreeLFS window. Pick a file,
   and check that the list responds to a click and to a double-click on a folder.
2. If nothing opens, check the portal log:

   ```sh
   tail -40 ~/.local/state/freelfs/log/flportal.log
   ```

**Send in a bug report**: a screenshot (Print, see [section 12](#12-making-a-good-bug-report)),
the exact button you pressed, and `flportal.log`.

Status: the chooser bugs are **fixed in code** and tested in QEMU. Not verified on a
laptop with Firefox.

### 7.3 The sandbox user-namespace error

**Symptoms**

- The log shows `Sandbox: CanCreateUserNamespace() clone() failure: EPERM`.

**Likely causes and status**

- Round 22 checked this. User namespaces work for your user and for the sandbox tool
  (bwrap). The `EPERM` matches Flatpak's own filter, which blocks nested namespaces.
  Firefox falls back to running without that nested sandbox.
- **This is a message, not a failure.** If Firefox starts and works, you can ignore it.

**Send in a bug report only if** Firefox does not start at all. Then send `flreport`
and the full line.

---

## 8. RAM looks high on a live system

**Symptoms**

- The General Manager shows several gigabytes used, more than the apps you think you
  have open.

**Likely causes**

- The live system keeps its **writable layer in RAM**. Anything you write, including
  the home folder and `/tmp`, is in memory. Report 5 measured about 2.8 GB in that layer,
  mostly Flatpak data under `~/.local/share`.
- A Flatpak install on the live system goes to RAM. `flpkg` warns about this.
- Shared memory (`Shmem`) counts as used. Browsers and the desktop use it.
- "Used" here means total minus available (`MemTotal - MemAvailable`). Cached files
  count as available, so they are not the problem.

**Look at it**

```sh
grep -E 'MemTotal|MemAvailable|Shmem:|Cached:' /proc/meminfo
df -h / /tmp
du -sh ~/.local/share/flatpak ~/.local/share 2>/dev/null
```

On the live system `df -h /` shows the writable layer (`overlay`) and `/tmp` is in RAM.
The General Manager (Ctrl+Shift+Esc) shows the memory per app and per Flatpak group.

**Fix**

- Close the apps you do not need. Close Firefox tabs you do not use.
- Do not install large Flatpak apps on the live system. Install them after you install
  FreeLFS on a disk. On an installed system the home folder is on the disk.
- For a stick, do not use Load into RAM on a small machine. It adds about 0.9 GB.
  See [2.3](#23-load-into-ram).

**Send in a bug report**: the `/proc/meminfo` lines above and a screenshot of the
General Manager's memory view. `flreport` also writes a memory summary.

---

## 9. Shutdown and restart do not finish

**Symptoms**

- After **Shut down** the screen goes off, but the computer stays on. Or nothing happens.
- On the login screen, the **Shut down** button needs two clicks.
- The screen shows text progress lines during shutdown, or the desktop shows a crash
  screen as the computer stops.

**Likely causes**

- Shutdown runs in stages. Services stop in reverse order. A service or a file system
  that does not respond can stop the shutdown. The rc scripts force the power-off after
  **15 seconds** (`shutdown_watchdog`).
- Round 16 to 18 changed the power path (`flpower` tries `telinit`, then `poweroff`, then
  `halt -f`, each with a time limit). The cause of the failure on one Ryzen laptop was
  **not found**. QEMU does not reproduce it.
- The panel's **Shut down** runs `flpower off` in the background. An error goes only to
  `~/.local/state/freelfs/log/flpower.log`, so you see no message. The bug scan of
  13 November suspected a missing setuid bit on `flpower`. Round 16 says the live system
  already had it. Check it below.
- The panel menu has **Shut down** and **Log out**. The bug scan of 13 November found no
  restart entry in the panel. Check the menu on your build.

**Fix**

1. Save your work. Choose **Shut down** from the start menu.
2. Wait up to 15 seconds. The watchdog forces the power-off after that.
3. If the computer is still on, hold the power button until it turns off.
4. On the login screen, click **Shut down** once, then again within 4 seconds (as of the
   13 November scan).
5. Check the log and the file mode:

   ```sh
   cat ~/.local/state/freelfs/log/flpower.log
   ls -l /usr/bin/flpower
   id
   ```

   `flpower` should show `-rwsr-x---` owned by `root` and group `seat`. Your `id` output
   should include `seat`. A plain `flpower` run from the terminal can show the error.
6. For a restart, the notes show no restart button. `flpower restart` exists in the code,
   but its use from a terminal is **not verified**. The crash screen's **R** key restarts
   the computer. If it does not, use the hold-the-button method above.
7. Ctrl+Alt+F2 opens a text console. From there `pkill flcomp` ends the desktop. The
   shutdown progress lines are also written to the text console (tty1), so you can
   switch there to read them (not verified on your build).

Status: the power path is **fixed in code** in parts (rounds 16 to 18). The laptop
failure is **open**: the cause was not found.

**Send in a bug report**: `flreport` (it collects the logs and `/run/flsplash.log`),
`flpower.log`, and a photo of the screen when the shutdown stopped.

---

## 10. Installing

### 10.1 Secure Boot after install

**Symptoms**

- The installed system does not start. The firmware says Security Violation.

**Cause and fix**

- An installed FreeLFS system needs Secure Boot off, for the same reason as the live
  stick. See [1.1](#11-verification-failed-or-security-violation-secure-boot-ventoy).
- New kernels are not signed. Keep Secure Boot off.

### 10.2 Erasing a disk

**Symptoms**

- Your other operating system or files are gone after you installed FreeLFS.

**Facts**

- The default mode **erases the disk you pick**. The installer says so on its last
  step, with the button **Erase and install**. In the default mode you tick a box to allow
  the erase before the button works (manual 03).
- The installer's check for a disk in use does not see every kind of hold. It does not
  see device-mapper, LVM or md holders (bug scan of 13 November, item D7). Unplug any USB
  disk or stick you do not want to touch before you start.
- The installer was tested on a virtual disk in QEMU. A real disk was **not tested**
  (README and manual 09).

**Before you start**

1. Back up everything on the disk. There is no undo.
2. Unplug other disks and sticks you want to keep.
3. Read the disk name in the installer window, and check its size against the disk
   you meant.

### 10.3 Dual boot

**Facts**

- **Dual boot with Windows is not supported.** The installer does not shrink Windows
  partitions.
- **Install next to existing partitions** uses only free space: the largest gap of at
  least 6.5 GiB on a GPT disk. It puts an EFI partition (512 MiB) and a btrfs partition
  in it. It erases nothing. It is **not tested on a real disk** (the notes say so).
- **Other drives and systems** in the boot menu can start another system's boot program
  (for example Windows' boot manager). Tested only in QEMU with OVMF. **Not verified**
  with a real Windows install.

**Send in a bug report** (if you try the free-space mode): the disk's partition table
(`lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINT`), and the installer's error text.

---

## 11. Software: flpkg, Flathub, Nix, Distrobox

`flpkg` is the one command for software. It looks in several sources, in this order:
**FreeLFS repository, Flathub, Nix, Ubuntu, Arch, Fedora**. These are the commands that
exist now (`src/flpkg/sources.sh`):

```sh
flpkg search TEXT
flpkg install NAME
flpkg install --from=flatpak APPID
flpkg install --yes NAME
flpkg remove NAME
flpkg upgrade
flpkg list
```

`--from=` picks the source: `freelfs`, `flatpak` (Flathub), `nix`, `apt`, `pacman` or
`dnf`. `remove` uses the source that installed the name. `--yes` skips the questions.

### The FreeLFS repository

- **Not published yet.** The repository address is set up for a release on GitHub, but
  the packages are not there (round 22 notes). Until it is published, software comes
  from Flathub or Distrobox.

### Flathub

- The Store and `flpkg --from=flatpak` add the Flathub remote for your account
  (`flatpak --user`). No root is needed.
- Tested by the project against a stand-in, not against live Flathub in every case. If
  an install fails, send the output.
- On the live system, Flatpak apps go into RAM. Install them after you install FreeLFS
  (see [section 8](#8-ram-looks-high-on-a-live-system)).

### Nix

- `flpkg` can use Nix when the `nix` command is installed. The front end was written and
  tested against stand-in programs. **It was never run against real Nix** (round 22).
- **Not verified.** Do not rely on it.

### Distrobox and Podman (Ubuntu, Arch, Fedora containers)

`flpkg install --from=apt|pacman|dnf NAME` uses rootless Podman through Distrobox (round 24a, run against the real
registries; the round report has the test table). Problems and what they mean:

- **"creating the container failed" / pull errors**: no network or no DNS in the guest, or Docker Hub's anonymous rate limit
  (wait, try later). Test with `podman pull docker.io/library/debian:stable-slim`.
- **`newuidmap` / "there might not be enough IDs available"**: your account has no range in `/etc/subuid` and `/etc/subgid`
  (`grep $USER /etc/subuid`) or `/usr/bin/newuidmap` is not setuid (`ls -l /usr/bin/newuidmap` must show `-rwsr-xr-x`).
  The installer adds a range for the user it creates and the live boot one for `tester`; for an account made by hand, as root:
  `echo NAME:200000:65536 >> /etc/subuid` and the same for `/etc/subgid`.
- **"overlay is not supported over overlayfs"**: the home is on overlayfs (live system). `flpkg` writes `~/.config/containers/storage.conf`
  for you; for a hand-made `podman` call, create it with `mount_program = "/usr/bin/fuse-overlayfs"` under `[storage.options.overlay]`.
- **"containers not creating Cgroups must create a private PID namespace"**: you ran `distrobox create` by hand. FreeLFS has no
  systemd, so containers have no cgroup (`cgroups = "disabled"`); add `--unshare-process`.
- **Out of space or RAM**: on a live system without persistence `~/.local/share/containers` is RAM. `distrobox rm -f freelfs-apt` and
  `podman rmi -af` give it back. Size on disk: Ubuntu about 150 MB, Arch 450 MB, Fedora 250 MB, plus what you install.
- **First start is slow**: the first `distrobox enter` installs a few helper packages in the new box.
- **Nothing in the menu**: only packages with a `.desktop` file get a menu entry; every other command is in `~/.local/bin`
  (is it in your PATH?).
- **Not shipped**: `aardvark-dns` and user-defined bridge networks (rootless containers use pasta; Distrobox uses the host network),
  Podman's systemd units, `podman-remote`, Docker compatibility sockets. `netavark` is upstream's prebuilt binary, not built from
  source (the build system has no Rust).

### What is not in FreeLFS

- **AUR**: not supported. There is no AUR helper.
- **configuration.nix** and NixOS-style configuration: not supported.

---

## 12. Making a good bug report

A useful report has four parts: **what you did**, **what you expected**, **what happened**,
and **the logs**.

### Step by step

1. **Write down the steps.** Number them. Note the time. Say which build you run
   (`cat /etc/os-release` shows the name and version).
2. **Make the report** from a terminal:

   ```sh
   flreport
   ```

   It writes `~/freelfs-report-YYYYMMDD-HHMMSS.tar.gz` and prints the path as its last
   line. The **Save a report** button in Settings > About does the same.
3. **Put the report on a stick**, if you test on the live system. Your home folder is in
   RAM and is lost at shutdown. Use `-o` to write straight to the stick. The folder must
   exist and be writable, and the stick must be mounted:

   ```sh
   flmount list
   flreport -o /drives/YOURLABEL
   ```

   **Check the last line** that `flreport` prints. If the folder was not writable,
   `flreport` silently writes to `/tmp` instead. The path it prints is the only way to
   tell.
4. **Remove private data before you post the report.** It contains network names,
   addresses (IP and MAC) and device identifiers. It does not contain passwords, Wi-Fi keys,
   browser history or file contents, except the settings files it lists.
5. **Open an issue** on GitHub: `github.com/wasselnl77-hash/freelfs`. Describe the
   problem with the steps and attach the report (or a link to it).
   For a security problem, read [SECURITY.md](SECURITY.md) first.

### Screenshots

- **Print**: the whole screen. The picture is saved under `~/pictures`.
- **Shift+Print**: a region you select.
- **Super+Shift+S**: the screenshot tool (Snappel) for the active window.

Attach the picture file, not a photo of the screen, when you can.

### Which log holds what

| Log | Where | What it shows |
|---|---|---|
| The desktop | `$XDG_RUNTIME_DIR/flcomp.log` | The compositor, windows, displays, the night light, the title bar of each window |
| Session services | `$XDG_RUNTIME_DIR/session.log` | Sound and session services |
| One program | `~/.local/state/freelfs/log/NAME.log` | What the program printed when it was started from the panel or the menu |
| The browser | `~/.cache/freebws/log` | Loads, watchdogs, restarts (FreeBWS) |
| Shutdown | `~/.local/state/freelfs/log/flpower.log` | Why Shut down failed |
| Boot splash | `/run/flsplash.log` | Boot steps, and the splash timing |
| Wi-Fi | `/var/log/iwd.log` | Connection messages |
| Bluetooth | `/var/log/bluetooth.log` | The Bluetooth service |
| Kernel | `dmesg` | Drivers, firmware, hardware |
| Video | `~/.cache/flvideo/mpv.log` | The video player |

`flreport` collects most of these. Add the ones above that match your problem.

### Hardware details

```sh
lspci -nnk
lsusb
ip link
cat /etc/os-release
```

The notes also ask for `lsmod`, `vainfo` (if installed) and `/sys/class/backlight` for
graphics and laptop problems. See [HARDWARE.md](HARDWARE.md) ("Wanted testers").

---

## 13. FAQ

**Is FreeLFS beginner friendly?**
Not yet. It looks like a traditional desktop, and the start menu, Files and Settings are
built for everyday use. But it is pre-alpha. When something breaks you may need a
terminal and the logs in this guide. Try the live stick on a spare computer or in a
virtual machine first.

**Is it stable?**
No. The README says so: "Do not use this for anything real." Expect crashes and changes
without notice. Keep backups.

**Does it use systemd?**
No. It uses **sysvinit 3.14** (`init` as process 1) with its own boot scripts in shell.
The udev daemon comes from the systemd source with its login parts switched off
([manual 14](manual/14-services-init.md)).

**Does it support the AUR?**
No.

**Does it use Nix or configuration.nix?**
No NixOS configuration. `flpkg` can use Nix packages when Nix is installed, but that
part was never run against real Nix (see [section 11](#11-software-flpkg-flathub-nix-distrobox)).

**Is it spyware? Does it send telemetry?**
I searched the source for telemetry and analytics code and found none. The crash and
report tools write files on your computer. `flreport` does not upload anything. The
source is MIT licensed ([LICENSE](../LICENSE)), so you can read it.
Some things do use the internet: the Store and Flathub, the update check, the ad-block
filter lists in FreeBWS, and the time service. This is a code search, not a security audit.

**Did AI write it?**
Yes, in large part. The project notes describe rounds of work done by several AI coding
agents in parallel, under written rules ([docs/AGENT-RULES.md](AGENT-RULES.md)). This guide
was also drafted with an AI assistant from the project notes and the reports. Check the
claims against the code and the notes before you rely on them. The round notes say which
parts were tested only in QEMU.

**Who makes it?**
The project is on GitHub at `wasselnl77-hash/freelfs`. The license names "The FreeLFS
contributors". The website is `wasselnl77-hash.github.io/freelfs`.

**How can I support it?**
Optional donations: `buymeacoffee.com/wassel`. The project makes no promises in return.

**Which hardware works?**
The tested machine is an AMD Ryzen 5 7520U laptop. Its Wi-Fi (Realtek RTL8852BE) worked
in report 5. Intel, NVIDIA, and most other chips are **not tested on real hardware**.
See [HARDWARE.md](HARDWARE.md).

**Can I dual boot with Windows?**
No. See [10.3](#103-dual-boot).

**Is there a graphical installer?**
Yes: "FreeLFS install" on the live desktop. It was tested in QEMU. A real disk was not
tested. See [section 10](#10-installing).

**Does it update itself?**
No. It tells you when updates are available. Install them with `flpkg upgrade`, or in
the Store's Updates tab.

**Can I run Windows programs?**
Not in the notes. Use Flathub apps, or a Distrobox container (`flpkg install --from=apt NAME`).

**Can I play games? Does Steam work?**
Steam was **not tested**. The notes say Xwayland (for X11 programs) is there. Test on a
spare computer.

**Where do I report bugs?**
GitHub issues at `github.com/wasselnl77-hash/freelfs`. Include a `flreport` file (see
[section 12](#12-making-a-good-bug-report)).

---

## 14. Open questions

The notes do not settle these. If you see one, send a report:

1. **Shutdown on the Ryzen laptop.** The cause of the failure in rounds 16 and 17 was not
   found. Report 5 does not say whether its shutdown worked.
2. **The "+" button in report 5.** It is not clear which button was meant.
3. **Sliders.** Report 3 said they did not work. No fix is recorded.
4. **Printers, Bluetooth audio and Intel graphics.** No real-hardware test is recorded.
5. **NVIDIA phase 1.** Not run on a real NVIDIA card.
8. **Intel Wi-Fi, MediaTek and Atheros Wi-Fi.** The drivers are in the image. No real card
   is recorded as working.
9. **Trash on a stick and drag and drop from Files.** Not done (round 21 notes). Not verified.

## 15. Login, root, passwords and the firewall (round 23)

**"I cannot log in as root."** Correct for the live ISO and the released images: root is locked (`!` in
`/etc/shadow`) and no root login is allowed over the serial port (`/etc/securetty` lists tty1 to tty6 only). On the live
system, work as `tester`: the actions that need administrator rights open the password dialog (flauth), and on the live
system the empty-password `tester` is not asked. On an installed system, the account's own password does the same. In a
terminal, `su` works for members of group `wheel` only (the installer's user is one). A development image (built with
`FLFS_DEV_ROOT=1`, its `/etc/issue` says "Development image") has an empty root password and root may log in on ttyS0.

**The installer asks for two passwords, or the Next button stays grey.** The account step wants the password twice, at
least **8 characters** (it was 4). The check box "Use the same password for the administrator (root)" is on by default.
Untick it to give root its own password (a second pair of fields). When the install starts, a password dialog may open
(on the live system the `tester` has no password and is not asked; on any other account it is, every time, and a wrong or
cancelled password stops the install before the disk is touched).

**"flauth: this account has no password; set one first" (exit 4).** An empty password is accepted only for the live
session's `tester` while `/run/live` exists. Anywhere else, give the account a password with `passwd`.

**`dmesg` says "Operation not permitted".** `kernel.dmesg_restrict = 1` (`/etc/sysctl.d/10-freelfs-hardening.conf`).
Run `dmesg` as root (`su`, for a member of `wheel`), or lower the setting for the session with
`sysctl kernel.dmesg_restrict=0` as root. The report tool (flreport) leaves the dmesg part empty for a normal user.

**A server I run is not reachable from another computer.** The firewall drops unsolicited inbound traffic
(`/etc/nftables.conf`, service `firewall`). Add a line such as `tcp dport 8080 accept` to the chain `input` and run
`nft -f /etc/nftables.conf` as root. `nft list ruleset` shows what is active. Outbound traffic (browser, Flatpak,
downloads, Wi-Fi) is not filtered. To switch the firewall off: `FIREWALL=0` in `/etc/rc.conf`, or `!firewall` in `SERVICES`.
Printers found by name need mDNS: the firewall opens UDP 5353 only while avahi runs (it starts with the first print request).
An installed system made before round 23 keeps its old `/etc/rc.conf`: add `firewall` first in `SERVICES` there.

**`gdb -p PID` or `strace -p PID` fails.** `kernel.yama.ptrace_scope = 1`: a process can be traced by its parent only.
Start the program under gdb/strace, or run the tool as root.

**`ping` or `traceroute` fails for a user.** `ping` uses an ICMP datagram socket (`net.ipv4.ping_group_range`). `traceroute` is
setuid for group `seat` only. Both work for the installed user and for `tester`.

## 12. Nix (round 24b)

### 12.1 `flpkg install --from=nix` says "the Nix service is not running"
The rc service `nix` is not running. As root: `rc nix start`. It is started at boot when the `nix` package is installed (config/etc/rc.conf, SERVICES). Check that `/var/nix/daemon-socket/socket` exists.

### 12.2 "cannot connect to daemon" or "single-user mode" errors
nix found no daemon socket and tried to use /nix as a single user. Start the service (12.1). Only root can write the store through the daemon.

### 12.3 The first search is slow
nix downloads the nixpkgs flake (from the registry) the first time, and evaluates it. After that the search is quick. A search needs network.

### 12.4 `hello` (or another Nix program) is not found after an install
`~/.nix-profile/bin` must be on PATH. Log in again, or run `. /etc/profile.d/nix.sh`.

### 12.5 Nix takes RAM on a live system
Nix's store is in RAM (no persistence): the install warning gives the size. nix-daemon stops 60 s after the last Nix command; `rc nix stop` stops it at once.

