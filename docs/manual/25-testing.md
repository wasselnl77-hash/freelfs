# 25. Testing: headless tests, the QEMU suites and what they do not prove

Summary: FreeLFS has three test layers: headless `make check` targets in each program, the package test suites inside the
build, and QEMU runs of the real image driven through its serial console and screen. This chapter explains each, the
command language of the QEMU drivers, the round suites (m8 to m18), the audit of 2026-11-14 that changed how tests must be
written, and why a pass in QEMU is not a pass on real hardware.

## Contents

1. The three layers
2. Requirements for QEMU tests
3. Headless tests in each program (`make check`)
4. Package test suites inside the build
5. The QEMU drivers
6. The `.cmds` command language (`build/live-run.py`)
7. The round suites (m8 to m18)
8. Writing a test that can fail
9. The audit of 2026-11-14: where the tests did not test what the person does
10. QEMU passing is not real hardware
11. Rules for test runs on a development machine

---

## 1. The three layers

| Layer | What runs | Needs | Proves | Does not prove |
|---|---|---|---|---|
| Headless `make check` in a program | the program alone, with `FLUI_PPM` or `FLCOMP_HEADLESS`, and a test client | the program's build dependencies | the program's logic and first frame | the session, the image, real input |
| Package suites (`check()` of a recipe, `run_check` in the base stages) | the upstream test suites | the build | the package builds and its own tests pass | anything about FreeLFS as a whole |
| QEMU runs of the image or the live ISO | the whole system: boot, login, desktop, apps, USB, shutdown | QEMU, OVMF, a built image or ISO | the system boots and the listed paths work in a virtual machine | real GPUs, real Wi-Fi, real disks and power buttons |

A change should pass the first layer for the programs it touches, and the QEMU suite for the paths it touches. The
package suites run when the build is asked to run them (section 4).

## 2. Requirements for QEMU tests

- `qemu-system-x86_64` and OVMF firmware (`build/00-host-check.sh` reports both as optional; QEMU tests need them).
- `/dev/kvm` is used when it is usable. Without it the tests run much more slowly (chapter 20, section 6.2).
- A built image (`./build/50-image.sh`) for `boot-test.py`, `m8`, `m9`, `m10`, `m11`, `m12`, and `m10-snapshots.sh`.
- A **serial twin** of the live ISO for the `live-run.py` suites (`m16`, `m17`, `m18`): the same ISO with
  `console=ttyS0,115200` as the last kernel option. The normal ISO has no readable serial login.

  ```sh
  ./build/53-iso.sh --cmdline "console=ttyS0,115200"
  cp ~/freelfs-work/freelfs-live.iso ~/freelfs-work/scratch/twin.iso
  ```

  Copy it away before the next ISO build overwrites it.
- Sample files for the media tests: `build/m10-samples.py` makes a disk image with `sample.png`, `sample.pdf`, `sample.wav`,
  `sample.flac`, `sample.m4a`, `sample.mp3`, `sample.mp4` and a zip. The MP3 is written by hand by `build/mk-mp3.py`, because
  the FreeLFS ffmpeg has a decoder but no MP3 encoder. The image is attached as a read-only second virtio disk.
- A scratch directory: the suites write logs and pictures to `$FLFS_WORK/logs` or to an `OUT` directory of your choice
  (for example `~/freelfs-work/scratch/m18`). Each suite that boots the image uses a throwaway copy of it (`-snapshot`), so
  the account, the layout and the test wrappers do not stay in the image.

## 3. Headless tests in each program (`make check`)

These run with no display. They are the first place to look when you change a program. Targets in `src/*/Makefile` (29
programs and libraries have a `check` target):

`flboot`, `flcomp`, `flcrash`, `flmusic`, `flarchive`, `fledit`, `fldisplay`, `flgm`, `flinstall`, `fllock`, `flpackages`,
`flpanel`, `flpdf`, `flpkg`, `flreport`, `flsettings`, `flsh`, `flsnap`, `flsocketd`, `flsound`, `flstore`, `fltheme`,
`flupdate`, `flportal`, `flvideo`, `flview`, `freebws`, `libdssl`, `snappel`.

`libflui` has no `check` target. Its behaviour is tested through the programs built on it.

Examples of what the headless targets do:

- `flcomp`: `make check` starts flcomp with `FLCOMP_HEADLESS=640x400`, connects `tests/shmclient` as a panel and as windows,
  sends fake pointer and key events through `FLCOMP_TEST_FIFO`, and checks the pixels of the screenshot
  (`tests/run.sh`). Other targets: `check-anim`, `check-keys`, `check-ws`, `check-lock`, `check-perf`, `check-x` (needs
  Xwayland, run in the build chroot).
- `flview`: `tests/run.sh` decodes a generated folder and drives the viewer to a PPM with `FLUI_PPM`.
- `fltheme`: `make check` checks the contrast of both colour schemes and of every accent (`tests/contrast`), the presets,
  and the GTK settings hints. `check-switch` runs `tests/switch.py`, which goes from dark to light and back in a headless
  flcomp.
- `flpkg`: `make check` runs `tests/test-core`, `test-build.sh`, `test-txn.sh`, `test-repo.sh`, `test-trust.sh` and
  `test-hook.sh`. The build test covers recipes; the transaction test kills flpkg at every stage.
- `flsettings`, `flpanel`, `fllock`, `flstore`, `freebws` and others have their own `tests/` directories and Python tests
  (for example `src/flsettings/tests/accent.py`, `src/flcrash/tests/egg.py` and `src/flfiles/tests/egg.py` for the egg).

Headless tests use the same environment switches as the programs (chapter 22, section 14; chapter 23, section 17).
Their screenshots are PPM files. A person should look at them as well: the rules in `docs/AGENT-RULES.md` ask for
screenshots in `docs/m7/` for each app.

## 4. Package test suites inside the build

Two mechanisms run upstream test suites while packages are made:

- A recipe's `check()` runs when `flpkg build` is run without `--nocheck`. The image build (`60-packages.sh`) always passes
  `--nocheck`, so these suites do not run there (chapter 21, section 3).
- The base stages (`30-base-toolchain.sh`, `31-base-system.sh`, `32-base-extra.sh` and others) call `run_check LEVEL CMD` from
  `build/lib.sh`. The level is set by `FLFS_CHECK` (chapter 20, section 3): `none` skips them, `core` runs the critical ones
  (glibc, binutils, gmp, mpfr, mpc), `all` runs every one. A failing suite is logged, not fatal: some tests cannot pass in a
  single-user namespace. Run `./build/test-report.sh` to list the failing suites, from the logs in `$FLFS_WORK/logs`.

## 5. The QEMU drivers

| Script | Purpose | Notes |
|---|---|---|
| `build/boot-test.py` | boots the image headless, logs in as root on the serial console, runs checks, and powers off | 2 GB and 1 GB runs in the round notes |
| `build/iso-test.py --iso FILE` or `--disk FILE` | boots the live ISO (as a CD) or a test disk, answers the first-boot keyboard question, checks that `rc.sysinit` printed no `[FAIL]`, logs in as `tester` on the screen, takes screendumps and checks them for blank frames | `iso-test-disk.py` makes a test disk with the ISO as a file, for the "ISO on a stick" path |
| `build/vm-run.py CMDFILE [QEMU-ARGS]` | boots the image, runs the commands of a file on the serial console, and can also run steps on the screen (`FLFS_VM_TTY`, a file of steps) | used by the `m7`, `m8`, `m9` and `m10` suites |
| `build/live-run.py [--iso FILE\|--disk FILE] [--mem 1G] CMDFILE [-- QEMU-ARGS]` | the newest driver: runs a `.cmds` file against the live ISO's serial twin, with screen and pointer | section 6 |
| `build/51-qemu.sh` | starts QEMU for the image or an ISO (chapter 20, section 6) | the drivers call it |

The QEMU monitor and QMP sockets are unix sockets in a temporary directory. The console transcript goes to
`$FLFS_WORK/logs/live-run.log` (or the equivalent log of the other driver).

## 6. The `.cmds` command language (`build/live-run.py`)

A `.cmds` file has one command per line. A `#` line is a comment. A line that starts with `@` is a driver command. Any other
line is a shell command typed on the serial console as root (`live-run.py` logs in as root on the serial console first,
and waits for the prompt after each command).

| Command | Meaning |
|---|---|
| `@wait SECONDS` | sleep |
| `@keys A B C` | QEMU key names, one after the other (for example `ctrl-alt-t`, `meta_l`, `ret`) |
| `@key NAME [HOLD_MS]` | one QEMU key, optionally held for a time |
| `@type TEXT` | type letters, digits and some punctuation. The driver maps the keys for the German (QWERTZ) default of the live session. |
| `@mon TEXT` | any QEMU monitor command (`drive_add`, `device_add`, `screendump`) |
| `@shot FILE` | screendump to FILE (PPM). It **fails** the run when the file is missing, not a PPM, empty, or one colour (over 98.5 % of the sample). |
| `@shotok FILE` | the same check as `@shot`, kept for old files |
| `@shotcolors FILE MIN` | fails unless the picture has at least MIN different colours (a gradient or a video frame has hundreds; a blank window a handful) |
| `@mouse X Y` | absolute pointer position, in pixels of a 1280 x 800 screen (needs `-device usb-tablet` in the QEMU arguments) |
| `@click [left\|right\|middle]` | a click at the pointer |
| `@dclick` | two quick clicks, a double click (the driver's comment says flui counts it within 400 ms) |
| `@drag X1 Y1 X2 Y2 [SHOT]` | press, move in 8 steps, release; the optional screendump is taken while the button is held |
| `@greeter` | waits up to 90 s for the login screen (`flgreet-ui`). Fails if it does not come up. Presses nothing. |
| `@desktop` | waits for the login screen, presses Enter (the `tester` account has no password), waits up to 60 s for the panel (`flpanel`) |
| `@ok TEXT :: CMD` | runs CMD in the guest as root. The step **fails** unless CMD exits 0. TEXT is the name of the check in the log. |
| `@waitok SECS TEXT :: CMD` | the same, tried once a second for up to SECS seconds |
| `@put LOCAL REMOTE` | copies a file into the guest through the serial console (base64 and gzip), mode 755 |
| `@poweroffwait SECS` | waits up to SECS for QEMU to exit. Fails when QEMU is still running, when the power-off took more than 20 s, or when the console did not show `Powering off`. A successful power-off ends the file. |

How a verdict is checked (from `guest_ok`): the command is sent as `( CMD ) >/tmp/ok.out 2>&1; echo OK$?T$((6*7))`, and
the driver accepts only `OK0T42`. The `T42` is computed by the shell, so the echo of the command line cannot contain the
token. This is the fix for the audit's finding that markers could be matched by the echo of their own command.

A short example, from `build/m18/panel-shutdown.cmds`:

```
@desktop
@ok the panel runs as tester :: pgrep -u tester -x flpanel
@key meta_l
@wait 2
@mouse 448 731
@click
@wait 2
@shot @OUT@/ps-1-dialog.ppm
@key ret
@poweroffwait 40
```

It logs in, checks that the panel runs, opens the start menu with the Super key, clicks the Shut down entry by its
coordinates, takes a picture of the confirmation window, confirms with Enter, and waits up to 40 seconds for the machine to
power off. The `@OUT@` and `@HERE@` placeholders are replaced by `m18-honest.sh` before the run.

Other examples: `build/m16/usb-plug.cmds` (a partitioned USB disk plugged in while the desktop runs, and mounted under
`/drives`), `build/m18/greeter-shutdown.cmds` (one click arms the "Click again" button on the login screen, the second powers
off), and `build/m16/persist-2-write.cmds` (checks that `/home` is the persistent image, then writes a marker).

Fixed coordinates in `@mouse` depend on the screen and the windows. The audit (section 9) noted this; the newer suites take
screendumps and check them where they can.

## 7. The round suites (m8 to m18)

Every round of the project got a suite. The scripts are in `build/`. The `.cmds` and `.tty` files they use are in the directories
`build/m16/`, `build/m17/` and `build/m18/`, or next to the scripts (`build/m10-features.cmds`, `build/m7-desktop.cmds`, ...). The list below is what each script's header says.
The build directory has no `m13`, `m14` or `m15` suite scripts (not verified whether those rounds had one).

| Suite | Runs | Checks |
|---|---|---|
| `build/m7-desktop.cmds`, `m7-desktop.tty` | `vm-run.py` | makes the account `tester` without a password and restarts the login screen (the M7 desktop round) |
| `build/m8-freebws.sh [2G 1G]` | the image in QEMU at 2 GB and 1 GB, throwaway copy | FreeBWS from the start menu: one tab through five origins, three and four tabs, links, discarding, the native DSSL page. Logs the resident size of every WebKit process. Pictures in `docs/m7/freebws-*`. |
| `build/m8-finish.sh` | runs after the WebKit build | builds flui, flcomp and freebws, makes the image and the ISO, runs m8 and boot-test at 2 GB and 1 GB |
| `build/m9-features.sh [2G 1G]` | the image in QEMU | notifications, the lock screen (wrong and right password), Settings > Date and time with the world map, time zones, `fl-sntp`, the ad blocker's first start, an error page, the setuid helpers |
| `build/m10-features.sh [2G 1G]` | the image in QEMU | workspaces and the overview, Snappel (Print), General Manager, accent colour, system sounds, fledit, Archives, Images, Documents, Music, Videos (with the media keys), Files opening files by MIME type, the Software Store, autostart, flsnap on the btrfs root |
| `build/m10-snapshots.sh` | three boots of a writable copy | a snapshot with flsnap, a change, a second snapshot; flboot's "Start an earlier state"; `flsnap rollback` |
| `build/m10-install.py` | the live ISO, plus a blank 16 GiB sparse virtual disk | the installer, clicked through like a person (language, keyboard, Wi-Fi, disk, account, time zone, summary, install), then the installed disk is booted alone and checked (btrfs subvolumes, ESP contents, login) |
| `build/m11-x11-gpu.sh x11\|gpu [1G]` | the image | `x11`: an X11 client through Xwayland and flreport's tarball. `gpu`: FreeBWS on a real https site with virtio-vga-gl (virgl), so flcomp's dmabuf import runs |
| `build/m12-sandbox.sh [1G [off]]` | the image | the FreeBWS web process sandbox: `freebws --sandbox-check`, bwrap in front of the web process, a page that draws, a download from a host server, the fallback |
| `build/m16-features.sh TWIN.iso [IMAGE.img]` | the live twin (and the disk image for `flauth`) | `usb-plug` (a USB disk hot-plugged: flmount list, mount, eject, a file written to it), `usb-notify` (the connected card), `poweroff-menu` (the panel's Shut down really powers off), `clipboard` (history list and paste), `window-qol` (window menu, drag to an edge, middle-click close), `files-dclick` (double click opens a file in the editor), `freebws-popup` (window.open posts back to its opener), `persist-1-create`, `persist-2-write`, `persist-3-check` (a stick with `freelfs-persist.img` keeps `/home` over a reboot), `flauth` (wrong and right password for a package removal) |
| `build/m17-features.sh TWIN.iso` | the live twin | fldisplay with two outputs, night light, Settings pages (Display, Bluetooth, Printers, Wallpaper), Bluetooth with no adapter, CUPS started on demand with a fake IPP printer |
| `build/m18-honest.sh TWIN.iso` | the live twin, with `STEPS="..."` to choose | the real UI paths of the round-18 report: `greeter-fresh` and `greeter-shutdown` (the login screen's Shut down, one click then "Click again"), `forced-shutdown` (a service that hangs; rc's 15 s watchdog must still power off), `panel-shutdown` (with Files, an editor and a video open), `files-open` (a PNG and an MP4 by double click in Files, with the pictures checked for colours) |

Each suite prints `ok`, `NOT ok` or `FAIL` lines, and ends with `RESULT: PASS` or `RESULT: FAIL (n): ...` (`live-run.py`). Its
exit status is 1 when any check failed.

## 8. Writing a test that can fail

The audit of 2026-11-14 found tests that passed whatever happened. Write new tests so that they cannot:

1. **Use the real UI.** Click the button the person clicks, at the place it is. Do not start the program from a shell argument
   and call the test done; do not set `WAYLAND_DISPLAY` by hand. Copy the session environment from the panel's process
   (`m18/panel-shutdown.cmds` does this), so that the program gets what it gets in a real session.
2. **Do not power off from a shell by default.** `poweroff` typed as root bypasses the panel, `flpower` and the login screen.
   Use the UI, and `@poweroffwait` to check it.
3. **Make the verdict a computed token.** The console echoes every command it sends, so a marker that is in the command
   (for example `echo greeter-up`) is always found. Use `@ok` and `@waitok`, which accept only a computed value.
4. **Check the picture.** A screendump that exists is not a window. Use `@shot` (fails on blank frames) and `@shotcolors`
   (fails on flat frames), or check a region's colours.
5. **Make a missing thing fail.** A wait for a process must fail when the process never appears (`@greeter`, `@desktop`, and
   `@waitok` do this). A `pkill` that finds nothing must not be reported as a success.
6. **Let a hang be a failure.** Give every wait a limit, and make the limit a failure, not a power cut. `@poweroffwait` does not
   kill QEMU when it times out; it fails the run.
7. **Keep a deliberately wrong check.** Before trusting a new check, change its expected value and see it fail. A check that has
   never failed has not been tested.
8. **Do not write to the host.** Test runs use a scratch directory and `XDG_*` variables that point at it (section 11).

## 9. The audit of 2026-11-14: where the tests did not test what the person does

`docs/bugscan-2026-11-14/tests-audit.md` (round 18, read-only) read the test scripts and logs against what a person does on the
laptop. The laptop had shown: shutdown did nothing from the panel and from the login screen, Files did not open images or video,
and a USB stick was not shown. The audit's main findings:

1. **Login-screen Shut down had no test.** The greeter's button (`src/flgreet/flgreet-ui.c`) forks `poweroff` and does not read
   the child's status. No test clicked it. Fix: `build/m18/greeter-shutdown.cmds` and `greeter-fresh.cmds`.
2. **`poweroff` was run as root on the serial console** in most tests, which bypasses the panel and the greeter. Fix: test the
   UI paths (`panel-shutdown`, `greeter-shutdown`). *Status: `boot-test.py` and `iso-test.py` still end with `con.send("poweroff")`
   on the serial console (read on 2026-11-14).*
3. **A failed shutdown passed.** `@poweroffwait` printed `NOT exited` and the run exited 0. *Status: fixed in `live-run.py`,
   which fails the run and checks the 20 s budget and the `Powering off` line.*
4. **Many checks could not fail.** The console echoes the typed command, so markers in the command matched. *Status: fixed in
   `live-run.py` (the computed `OK0T42` token). `iso-test.py` uses computed markers (`gone-$((6*7))`), and `boot-test.py`
   compares computed output. Other older checks may still match an echo; check each suite when you change it.*
5. **"Files opens video" was never tested through Files.** `m18/files-open.cmds` opens a PNG and an MP4 by double click, as the
   session's `tester`.
6. **The USB stick was a whole-disk exFAT file, not a partitioned stick.** `m16/usb-plug` uses a partition table (`sda1`),
   and flmount must not offer the whole disk as a drive.
7. **Screenshots were never checked.** `@shot` now fails on a missing, blank or one-colour picture. *Status: fixed in
   `live-run.py`.*
8. **The test environment was not the user's.** Apps ran with hand-built variables, the keyboard was pre-set, and the GPU was
   QEMU's `-vga std`, never amdgpu or i915.

The audit ranked the fixes: the greeter shutdown test first; then `NOT exited` and the power-off checks; then Files for MP4 and the
real Files start, with pixel checks; then the echo-matched markers; then the partitioned stick and the eject check; then the
session environment helper and the first-boot keyboard by name; then the pixel checks and the GPU and touch items as hardware checks.

The audit also lists what only a real machine can check (section 10).

## 10. QEMU passing is not real hardware

The repository is explicit about this, and so should every contributor:

- `docs/HARDWARE.md`: "Nothing of it has run on any Intel or AMD machine yet" (for the hardware round of 2026-11-12, built on
  2026-11-13, run in QEMU only). The Intel status is "prepared, untested on real hardware: needs testers".
- The M10 integration round had a real-laptop run that failed; the fixes were made and tested in QEMU, and the laptop was not
  retested after the kernel change (`docs/HARDWARE.md`, status table). The laptop run of round 15 showed that shutdown did
  nothing, Files did not open images or video, and a USB stick was not shown. The round-18 audit explains why the QEMU suites
  had passed while the laptop showed these problems.
- QEMU has virtio-gpu or `-vga std`, no real GPU drivers (amdgpu, i915, xe), no real Wi-Fi chip, no real battery, no lid switch,
  no real backlight, and no real power button.

The real-hardware checklist in the audit (section "Real-hardware checks") covers what QEMU cannot:

1. Login-screen Shut down: click once, then "Click again", with nothing else running.
2. Panel Shut down, Restart and Log out with Files, a browser (three tabs), a video and a stick open.
3. The physical power button (short press) and the lid close, on battery and on mains.
4. Images and video from Files on the real GPU: PNG, JPEG and MP4.
5. USB sticks: an MBR stick and a GPT stick (`sdX1`), a stick written with `dd`, a Ventoy-style file, and eject.
6. The touchpad: right click, two-finger click, double tap to open a file, drag to a screen edge.
7. Secure Boot on, if the laptop has it on: the stick and the installed system must boot.
8. First boot with a keyboard that is not German (US or UK), and the Fn keys.
9. Wi-Fi (RTL8852BE on the development laptop): a real access point, a suspend and resume, and the hardware test of `iwd`.
10. Night light and a fractional scale (1.25 or 1.5) on the real panel.
11. Suspend and resume: the screen comes back with the same windows, and shutdown after that.
12. Shutdown of an installed system on an SSD under btrfs: the time to power off.

Record the hardware (model, GPU, Wi-Fi), the kernel version (`uname -r`), the image's hash and the logs. `src/flreport` collects a
report of the machine for bug reports. Report results in the issue tracker (chapter 26).

## 11. Rules for test runs on a development machine

Taken from `docs/AGENT-RULES.md`, which applies to any tool that runs tests on the project's machines and is a good rule for
contributors too:

- Never use sudo, and never enter a password. Everything is rootless.
- Point `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and `HOME` at a scratch directory for a test. Never write to the real `~/.config`,
  `~/.local` or dotfiles.
- Never write to a block device, and never touch `/run/media`. The USB images are files; `52-usb.sh` prints the `dd` command and
  leaves the writing to you.
- Do not kill processes you did not start.
- Long test runs go through `build/gentle.sh` (chapter 20, section 11), and never run more than four compile jobs on the 7 GB host.
- Do not wait with `while pgrep -f PATTERN`: the loop matches its own command line and never ends. Wait on a PID
  (`while kill -0 $PID 2>/dev/null; do sleep 5; done`), use `pgrep -x NAME`, or run the job in the background.
- Do not read or search the `~/Dokumente/tokens` folder. Tests need no credentials.
