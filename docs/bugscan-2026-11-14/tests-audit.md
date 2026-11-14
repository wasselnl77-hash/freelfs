# Test audit: where the QEMU tests do not do what the user does

Round 18, read-only audit (agent B). Nothing was built, booted or committed. Every finding
is from reading the scripts and the logs already in `~/freelfs-work/scratch/m16` and `logs/`.

Reported on the laptop (RTL8852BE, real GPU): shutdown does nothing from the panel and from
the login screen; Files does not open images or video; a USB stick is not shown.

## Summary

1. **Shutdown from the login screen is not tested at all.** The greeter's "Shut down" button
   (`src/flgreet/flgreet-ui.c:470-482`) is never clicked by any test. The only test that
   powers off from the UI does it from an empty desktop, and treats "QEMU exited" as success.
2. **`poweroff` is run as root on the serial console** in most tests (boot-test, vm-run,
   persist-2, iso-test). That bypasses the panel, `flpower off` and the greeter, which are
   the paths the user takes.
3. **A failed shutdown passes.** `live-run.py:176` prints `NOT exited` and the run still
   exits 0 (`live-run.py` never sets a failure exit code), so the wrapper scripts see success.
4. **Many checks cannot fail.** The console echoes the typed command, and `run()`/`CHECKS`
   match the marker against that echo. Markers such as `greeter-up`, `greeter-stopped`,
   `started`, `gone` and `""` are found in the command text itself, so they pass whatever
   happened.
5. **"Files opens video" is never tested through Files.** The only video open is
   `m10app flvideo <path>`; the Files-by-MIME section covers png, pdf, m4a and txt only.
6. **The USB stick is a whole-disk exFAT file, not a partitioned stick.** Real sticks have
   a partition table (`sdX1`) or are dd'd ISOs. The tests look at `/dev/sda` only.
7. **Screenshots are never checked.** Every "picture" is for a person to look at. A missing
   or black frame passes (`iso-test.py:97-103`, all `@shot` lines).
8. **Test environment is not the user's.** Apps run through `su tester` with hand-built
   `XDG_*`/`WAYLAND_DISPLAY` variables (`m10-samples.py:131-136`), `keyboard=us` is
   pre-written, the first-boot keyboard choice is always Enter, and the GPU is QEMU's
   `-vga std` (virgl or virtio-vga in a few runs), never amdgpu or i915.

---

## 1. Shutdown (most important)

### 1.1 Login-screen "Shut down" has no test
- **Where:** `build/m16-features.sh:17` ("Not here: ... `flpower off` as tester -- both are the
  first lines of usb-plug"); `build/m16/usb-plug.cmds:1-2` (only checks that Ctrl+Alt+T does
  nothing on the login screen).
- **Test does:** nothing with the button. Logs in and checks Ctrl+Alt+T.
- **User does:** clicks "Shut down" on the greeter, then "Click again" within 4 s
  (`flgreet-ui.c:470-482`). The code forks `/usr/sbin/poweroff` directly, and the child's
  exit status is never read. A failure there is silent.
- **Could hide:** the greeter's shutdown does nothing on the laptop. Every greeter path
  (arming, second click, `execl`, timer reset) is unexercised.
- **Fix:** add `build/m18/greeter-shutdown.cmds`, run from `live-run.py` on the twin ISO
  before login. Click once, `@wait 1`, take a shot, check the "Click again" label changed
  (pixel check), click again, `@poweroffwait 30`. Fail if not exited, and fail if the first
  click alone powers off. Run it on a second boot with a stick attached to see the
  greeter-with-stick case.

### 1.2 The panel's Shut down test uses an empty desktop and fixed coordinates
- **Where:** `build/m16/poweroff-menu.cmds:1-10`. Clicks at fixed pixels (448,731) and
  (700,374) of a 1280x800 screen, then `@poweroffwait 60`.
- **Test does:** logs in, clicks the panel menu by coordinates, clicks confirm by
  coordinates, waits for QEMU to exit. Nothing is open. No USB stick, no browser, no file
  window.
- **User does:** shuts down with Files, a browser (several WebKit processes), a video or
  music player and possibly a stick mounted (`/drives/...`) and an unsaved editor window.
  Scale 1.25 (the `m17` display test sets it) moves every button.
- **Could hide:** shutdown hangs when a busy app or a mounted stick is present (rc.shutdown
  waiting on processes or `umount`). The coordinates can also miss, and a miss passes (see 1.3).
- **Fix:** a second run, `poweroff-busy.cmds`, that first opens `flfiles` on the USB stick
  from `usb-plug`, opens `flvideo` on `sample.mp4` and `freebws` on `pop.html`, then powers
  off from the panel. Find the buttons by screenshot, not by fixed pixels: take a shot and
  compare with a stored reference region, or use `fl` IPC if one exists.

### 1.3 "NOT exited" passes
- **Where:** `build/live-run.py:172-177` prints `@poweroffwait: NOT exited` and `break`s.
  The `finally` at `live-run.py:185-190` then `terminate()`s QEMU. The exit code is 0, and
  `m16-features.sh:30` and `:36` only pipe through `tee` and never look at the result. The
  same applies to `persist-2-write.cmds:8` (`@poweroffwait 90`).
- **Test does:** a 60 s or 90 s wait, then kills the VM with SIGTERM and carries on.
- **User does:** waits and sees the machine stay on. That is the bug report.
- **Could hide:** every shutdown hang. A slow or stuck shutdown is turned into a power cut
  and the next step (persist-3) boots that disk.
- **Fix:** `live-run.py`: `@poweroffwait` must `sys.exit(1)` on NOT exited, and must not kill
  the VM first. Record the elapsed seconds and fail above a budget (20 s).
  `m16-features.sh` and `m17-features.sh`: `run` pipes python into `tee`. With the
  `pipefail` already set, a non-zero `live-run.py` exit then stops the script, so the fix
  is only in `live-run.py` plus a final `exit 1` in `m17-features.sh` when the night-light
  check fails (section 5).

### 1.4 "exited" is not proof of a clean poweroff
- **Where:** `live-run.py:176` (`qemu.poll() is not None`).
- **Test does:** accepts any QEMU exit as "the machine is off".
- **Could hide:** a QEMU crash, a guest reset that exits, or a failed `poweroff` that
  still ends the VM. The real check is the guest's ACPI shutdown.
- **Fix:** ask QMP for the `SHUTDOWN` event with `"guest": true` (QMP is already open in
  `live-run.py:64-82`). Also grep the serial transcript for `Powering off.` before exit, as
  `boot-test.py:290` already does.

### 1.5 Shutdown is run as root on the serial console
- **Where:** `build/m16/persist-2-write.cmds:7` (`poweroff`); `build/vm-run.py:168-170`
  (`c.send("poweroff")`, `c.drain(90)`); `build/boot-test.py:285` (`con.send("poweroff")`);
  `build/iso-test.py:205-206`.
- **Test does:** root's `poweroff` in the serial shell. No flpower, no greeter, no uid 1000.
- **User does:** Shut down in the panel as tester (`flpower off`, `flpower.c:238-240`, which
  clears the environment and then runs `/usr/sbin/poweroff`) or on the greeter.
- **Could hide:** a permission or environment problem in the user path. This is the least
  likely cause for "does nothing", but it means no test covers the path the user takes.
- **Fix:** the boot, vm-run and persist tests keep root `poweroff` for cleanup only. The
  shutdown test is the greeter and panel tests above, run as tester.

### 1.6 The live-ISO test never checks shutdown
- **Where:** `build/iso-test.py:205-206` (`con.send("poweroff")`, `con.drain(40)`), then
  `finally` at 214-216 calls `qemu.terminate()`.
- **Test does:** sends poweroff, reads for 40 s, no check, then kills the VM. The script
  prints `PASS` at line 217 whatever happened.
- **Could hide:** "the live ISO does not power off". This is the medium the user runs.
- **Fix:** after `poweroff`, `qemu.wait(60)` with a `Powering off.` check on the transcript.
  Fail the run if QEMU is still running, and drop the `terminate()` in the success path.

### 1.7 flpower's `off` path has no test
- **Where:** `src/flpanel/tests/power.sh:1-47` covers only `flpower saver`. The `off` and
  `restart` branches (`src/flpanel/flpower.c:238-244`, `run_power` 201-230) are untested.
  `build/m16/usb-plug.cmds:15` only lists the mode of `/usr/bin/flpower`
  (`-rwsr-x--- root seat`; the output is not compared).
- **Could hide:** a setuid, group or environment problem that only shows as tester.
- **Fix:** in the greeter test, after login, `su -s /bin/sh tester -c 'flpower off; echo rc=$?'`
  and check that a shutdown follows (QMP event). Also `ls -l` must show `root seat` with
  the expected mode, and that line should be an assertion.

---

## 2. Opening files (images and video)

### 2.1 Video is never opened from Files
- **Where:** `build/m10-features.tty:105` (`run m10app flvideo /home/tester/m10/sample.mp4`);
  the Files section at `m10-features.tty:111-133` covers `sample.png`, `sample.pdf`,
  `sample.m4a` and `notes.txt` only.
- **Test does:** starts `flvideo` with a path argument from a helper shell (no Files, no
  MIME lookup). The Files section never names an `.mp4`.
- **User does:** double-clicks a video in Files. Files picks the handler by MIME type
  (`mimeapps.list`, per the comment at `m10-features.tty:111`).
- **Could hide:** exactly the reported bug. A wrong default, a missing MIME association
  for `video/mp4`, or Files not launching `flvideo` would all pass.
- **Fix:** add to the Files section `type sample.mp4\n`, then `pgrep -x flvideo` and a
  shot. Also check `~/.config/mimeapps.list` and `xdg-mime query default video/mp4` inside
  the session, expecting `flvideo.desktop`.

### 2.2 Image and document checks are process checks
- **Where:** `m10-features.tty:117` (`run pgrep -a flview`), `:123` (`pgrep -a flpdf`),
  `:127` (`pgrep -a flmusic`), `:131` (`pgrep -a fledit`).
- **Test does:** counts a process named like the app. A window that is open but blank, one
  that crashes after 1 s, or one that opened a different file all pass.
- **Could hide:** a viewer that starts and shows nothing, which is the usual form of
  "does not open".
- **Fix:** after the open, take a shot, convert it, and check the mean brightness and the
  share of non-background pixels against the Files window (`d-files-open` style). Fail on
  a blank frame. Check `/run/user/1000/<app>.log` for "cannot open" or a stack trace.

### 2.3 The Files test starts the app from a path argument, not from the menu
- **Where:** `build/m16/files-dclick.cmds:3` (`setsid flfiles /home/tester >/tmp/files.log`),
  `:5` (fixed click 511,270), `:8` (`pgrep ... fledit`), `:9` (screenshot, not checked).
- **Test does:** starts Files with a directory argument, so Files does not start in its
  default location. The double-click lands on a hard-coded position. It then checks that a
  process called `fledit` exists.
- **User does:** opens Files from the menu (default location, usually `~`) and double-clicks
  a file wherever it appears.
- **Could hide:** a wrong default folder, a file in the wrong place (the click misses and
  nothing opens), and a file opened by the wrong program.
- **Fix:** start Files through the launcher (the panel's menu, or `flcomp`'s launch IPC
  if there is one). Find the file's position from the shot, not from a constant. Check
  that the process's `/proc/<pid>/fd` or the window title names `readme.txt`.

### 2.4 The helpers set an environment the session does not have
- **Where:** `build/m10-samples.py:131-136` writes `m10sh`/`m10app`, exporting
  `XDG_RUNTIME_DIR`, `WAYLAND_DISPLAY`, `DBUS_SESSION_BUS_ADDRESS` and
  `XDG_CURRENT_DESKTOP=FreeLFS`, then `su` and `setsid sh -c`. The same pattern is in
  `build/m16/usb-plug.cmds:27,30`, `files-dclick.cmds:3`, `clipboard.cmds:3,26`,
  `window-qol.cmds:2`, `iso-test.py:188-190`.
- **Test does:** sets the variables that the session would have, by hand. No session
  leader, no logind seat, no `XDG_SESSION_ID`, no `XDG_DATA_DIRS` from the panel.
- **User does:** starts an app from the panel or Files, which inherits flcomp's environment.
- **Could hide:** MIME and `xdg-open`-style lookups, portal calls and seat checks that use
  the session. These are the things that break "open with" and file dialogs.
- **Fix:** one helper, `m18sh`, that runs the command with `env -i` plus exactly the
  variables read from `/proc/$(pgrep -x flcomp)/environ` for user 1000. Then compare the
  two environments in the log.

### 2.5 The M10 Files test runs on the installed disk, not on the live ISO
- **Where:** `build/m10-features.sh:19-27` (the image `50-image.sh`, `-snapshot`). The
  live-ISO path is `build/m16-features.sh` (twin ISO) and `iso-test.py`, which do not open
  any image or video file.
- **Test does:** the MIME and Files checks on the installed image, where `/home/tester` is
  created by the test and `desktop.conf` is pre-written (`m10-features.cmds:5-6`).
- **User does:** boots the live ISO (squashfs root with an overlay in RAM, nothing
  pre-written).
- **Could hide:** MIME files and icons missing from the squashfs or from the live overlay.
- **Fix:** run the Files-open steps (2.1 to 2.3) on the twin ISO too, with `-snapshot` off
  and no pre-written config.

### 2.6 A video with a real GPU is not tested
- **Where:** the default VGA is `-vga std` (`build/51-qemu.sh:75`). `build/m11-x11-gpu.sh:21`
  and `build/m12-sandbox.sh:28` use virgl, not amdgpu or i915. The video test runs software
  decode only.
- **Could hide:** decoder selection, dmabuf import, and GL paths that only show on real GPUs.
- **Fix:** QEMU cannot test amdgpu or i915 (see the last section).

---

## 3. USB stick

### 3.1 The stick is a bare exFAT file with no partition table
- **Where:** `build/m16-features.sh:32-33` (`truncate`, `mkfs.exfat -L TESTUSB "$usb"`), and
  `usb-plug.log` (`/dev/sda TESTUSB exfat 268435456 /drives/TESTUSB removable`). The test
  mounts `/dev/sda` and never `/dev/sda1`.
- **Test does:** the whole device is the file system.
- **User does:** plugs a stick with an MBR or GPT partition (`sdb1`), or a stick written with
  `dd` from the ISO (`build/52-usb.sh:96`), which has an ESP and live partitions.
- **Could hide:** the reported "stick not shown". A partitioned stick may never reach the
  mount logic (`flmount list`, the Files sidebar, the notification), and the dd'd ISO stick
  may show its own live partitions as data drives.
- **Fix:** build the test stick with `sfdisk` as `iso-test-disk.py` does, one exFAT
  partition, and mount `/dev/sda1`. Add a second stick: the twin ISO written with `dd`, and
  assert that the live medium is not offered as a drive.

### 3.2 Hot-plug is a QEMU device add, and the wait is fixed
- **Where:** `build/m16/usb-plug.cmds:18-20` and `usb-notify.cmds:2-3`
  (`@mon drive_add` / `device_add usb-storage`), then `@wait 10` (`usb-plug.cmds:20`) and
  `@wait 4` (`usb-notify.cmds:4`).
- **Test does:** adds a `usb-storage` on an emulated xHCI and waits 10 s, then lists.
- **User does:** plugs a real stick into a real xHCI (USB 3, UAS, hub, USB-C dock). The
  kernel can take 5 to 20 s to enumerate and auto-mount.
- **Could hide:** slow enumeration, and a stick that is found only after a second plug.
- **Fix:** poll `flmount list` every second up to 60 s and print the elapsed time. Fail if
  the stick is not listed in the Files sidebar. A pixel check is needed for the sidebar
  (the test has only `a-files.ppm`, unchecked, at `usb-plug.cmds:27-29`).

### 3.3 Unmount, eject and mount results are printed, not asserted
- **Where:** `usb-plug.cmds:30-33`, `usb-plug.cmds:36-37` (`flmount eject /dev/sr0`, `flmount
  mount /dev/vda1`, expected rc=1: "no such partition", not checked), `usb-notify.cmds:8-11`,
  `:12-13`.
- **Test does:** prints `unmount-rc=0`, `eject-rc=0`, and reads the next line.
- **Could hide:** eject that returns 0 while the device is still there. The user then pulls
  a mounted stick and loses data.
- **Fix:** after `eject`, `test ! -e /sys/block/sda` and `! grep -q /drives /proc/mounts`,
  or fail. Compare the exit codes with the expected values, not just print them.

### 3.4 Persistence passes whatever the marker says
- **Where:** `build/m16/persist-3-check.cmds:2` (`cat /home/tester/marker.txt`, no comparison
  to `persisted-ok`); `persist-1-create.cmds:5` (`flpersist create 512M` run, its rc printed
  and not asserted); `m16-features.sh:46-50` (the three runs, none checked).
- **Could hide:** a `/home` that is not kept over a reboot. This is the feature the user
  would notice first on a stick.
- **Fix:** `grep -qx persisted-ok /home/tester/marker.txt || echo FAIL` with a non-zero exit
  that `m16-features.sh` honours. Assert `rc=0` in persist-1.

---

## 4. Checks that cannot fail (cross-cutting)

The console echoes the command it was given. The tests match a marker against the whole
output, and the output includes that echo. So any marker that appears in the command text
is always found.

- `build/boot-test.py:55` (`grep -c '^1:.*flgreet' /etc/inittab` expects `1`: the command
  contains `1`).
- `build/boot-test.py:56-57` (`... && echo greeter-up` expects `greeter-up`; the command
  contains `echo greeter-up`). The greeter may never have started.
- `build/boot-test.py:61-62` (`... echo greeter-stopped` expects `greeter-stopped`; the
  `pkill`s and `telinit q` can fail and the check still passes).
- `build/iso-test.py:190` (`echo started` expects `started`, the command contains it).
- `build/iso-test.py:195, 201, 204` (`echo gone` expects `gone`).
- `build/iso-test.py:196` (marker `""`: `"" in out` is always true).
- `build/live-run.py:166-167`: `...; echo greeter-up` is unconditional (`;`, not `&&`), so
  the wait for the greeter can never fail. `live-run.py:171` then sleeps 15 s regardless.
- `build/m16/usb-plug.cmds:3`: `pgrep ... && echo greeter-up` prints the same word whatever
  happens, and the run goes on.

**Fix:** match only the output that comes after the echoed command, or print a marker
computed at run time (`echo MK-$((6*7))` expects `MK-42`, which cannot appear in the
command). Make `run()` in `iso-test.py` and `CHECKS` in `boot-test.py` fail on a missing
marker and count it. Add a self-test: a deliberately wrong marker must fail.

---

## 5. Pictures and screens are never checked

- `build/iso-test.py:97-103`: `shot()` writes a PNG only if the screendump exists, and never
  fails. The `PASS` at line 217 is printed with no screenshot checked.
- `build/live-run.py:141-142`, `m16-features.sh` and `m17-features.sh`: every `@shot` is a
  `.ppm` that a person looks at. No pixel check of the window, the notification or the
  menu. A black screen or a crash dialog passes.
- `build/m17-features.sh:56-57`: the night-light check prints `FAIL ...` and the script
  exits 0. The comment says the hardware gamma does not show in a screendump, so a real
  failure and a test limit look the same.

**Fix:** a `check_shot` helper: fail if the file is missing, if the image is nearly all one
colour (the greeter, the desktop and the window each have a known background), or if a
reference region differs. Note the screen the test expects: the greeter, the desktop, the
window. Keep the pictures as evidence, but make the verdict a number.

---

## 6. Waits and timeouts that hide hangs

- `build/live-run.py:171`: `time.sleep(15)` after Enter on the greeter. The desktop may
  still be coming up, or may have failed, and the test goes on.
- `build/iso-test.py:173-174` (`sendkey ret`, `sleep 15`), `:119-121` (`sleep 8` before the
  first-boot key).
- `build/m16/freebws-popup.cmds:9`: `@wait 70` for FreeBWS to start. A slow start passes.
- `build/m17/printing.cmds:34`: `@wait 75` for the cupsd idle exit. The idle exit is not
  asserted at 60 s, and `printing.cmds:36` prints `avahi-exited-too` as a `pgrep` line with no
  assertion.
- `build/m16/persist-2-write.cmds:8` (`@poweroffwait 90`), `build/vm-run.py:169` (`drain(90)`),
  `build/vm-run.py:163` (`expect(..., 1800)`), `build/iso-test.py:206` (`drain(40)`).
- **User does:** notices a 10 s or 30 s hang.

**Fix:** record the time to each milestone (greeter up, desktop up, poweroff) and fail
above a budget set well under what the user waits. For shutdown: 20 s in QEMU.

---

## 7. Other environment and path differences

- **The keyboard is pre-set.** `build/m10-features.cmds:5-6`, `m8-freebws.cmds:7-8`,
  `m11-x11-gpu.cmds:4-5`, `m12-sandbox.cmds:12-13` write `keyboard=us` and `LAYOUT=us`. The
  first-boot keyboard question is answered with Enter (`live-run.py:104`, `iso-test.py:121`,
  `boot-test.py:236`, `vm-run.py:155`). The default layout is German. The user's layout
  choice is never exercised, and a key that differs between layouts (Strg, AltGr, the
  `meta` combinations) can go unseen. Fix: one pass with the first-boot choice made by name
  (for example `us`), and one with the default.
- **Config is pre-written.** `m10-features.cmds:5` writes `desktop.conf`. A fresh session
  has none, and the defaults (accent, panel, night light) differ. Fix: a run with no
  `desktop.conf`.
- **The browser test wraps the binary.** `build/m8-freebws.cmds:13-14` replaces
  `/usr/bin/freebws` with a wrapper that sources `/tmp/freebws-env`. The discard timings the
  test sees are the test's, not the shipped ones.
- **The popup test loads `file://`.** `build/m16/freebws-popup.cmds:10-12` opens
  `file:///home/tester/pop.html`. An https origin has different popup and sandbox rules.
  Fix: serve the fixture from `http://localhost` (or https through a local CA) and test that.
- **Root reads the USB state.** `usb-plug.cmds:22-23` and `usb-notify.cmds:9` read
  `/proc/mounts` and `ls /drives` as root. The user's view is the Files sidebar.
- **Mouse input is an absolute tablet.** `live-run.py:144-145`. There is no touchpad, no
  palm rejection and no two-finger right click. Right click in the tests is a real button
  3 event that a touchpad does not send.
- **The twin ISO has a different kernel command line.** `build/53-iso.sh --cmdline
  console=ttyS0,115200` (`53-iso.sh:35`). With `console=` on the serial port only, kernel
  messages and any console handover can differ from the real ISO. Fix: keep one screenshot
  run on the real ISO with no serial console, and compare the greeter with the twin's.
- **Snapshot mode discards writes.** Most runs use `-snapshot` (`m8`, `m9`, `m10`, `m12`).
  Writes that stay in a dirty page cache, and a btrfs `fsync` at poweroff on a real SSD,
  never get tested.
- **`m8-finish.sh:24`** uses `while pgrep -f 'build/60-packages.sh'`. This is the pattern
  AGENT-RULES forbids. It is not self-matching in that script, but it can match any editor
  or grep that mentions the path. Replace it with a PID wait (`kill -0`).

---

## 8. Ranked fix order

1. Greeter shutdown test (1.1), with a stick attached and with a browser open.
2. `NOT exited` and `poweroff` fixes (1.3, 1.4, 1.6): exit 1, QMP `SHUTDOWN guest=true`, a budget.
3. Files-by-MIME for mp4 and the real Files start (2.1, 2.3), with pixel checks (2.2).
4. Echo-matched markers (section 4): fix the matcher, add a self-test.
5. Partitioned stick and dd'd ISO stick (3.1), eject verification (3.3), persistence
   comparison (3.4).
6. Session environment helper (2.4), and the first-boot keyboard by name (7).
7. Pixel checks for screenshots (5), GPU and touch items as hardware checks (below).

---

## Real-hardware checks (QEMU cannot do these)

Do these by hand on the laptop, with a stopwatch and a phone camera where useful.

1. **Login screen Shut down.** Click once, then "Click again", with nothing else running.
   Time to power off. Then repeat with a USB stick plugged in, and with a browser open.
2. **Panel Shut down, Restart, Log out** with Files, a browser (three tabs), a video and a
   music file open, and a stick mounted. Time each. Note whether the screen goes dark
   before the power LED goes out.
3. **Physical power button**, short press, and the lid close, on battery and on mains.
4. **Images and video from Files** on the real GPU (amdgpu or i915): PNG, JPG and MP4 by
   double-click, and by the Enter key. Check the window is not black, and that the decoder
   is hardware where expected (check the logs, not the picture).
5. **USB sticks:** an MBR stick and a GPT stick (`sdX1`), a stick written with `dd` from the
   ISO, and a Ventoy-style stick. Plug in after boot and before boot. Each must show in the
   Files sidebar within about 5 s. Eject from Files, then pull it: `/sys/block` must have
   lost the device.
6. **Touchpad:** right click, two-finger click, double-tap to open a file, a drag to a screen
   edge for snap.
7. **Secure Boot** on, if the laptop has it enabled: boot the stick and the installed system.
   QEMU's OVMF has Secure Boot off.
8. **First boot** with a keyboard other than German (US or UK), and the Fn keys.
9. **Wi-Fi** (RTL8852BE): a real access point, a suspend and resume, and the hwsim test's
   checks on the real radio.
10. **Night light and fractional scale** (1.25 or 1.5) on the real panel: the colour change
    is only visible on the screen, not in a QEMU screendump.
11. **Suspend and resume**: the screen comes back with the same windows, and shutdown after
    a resume works.
12. **Shutdown with an installed system on an SSD** under btrfs: time to power off after a
    large write (a copy of a few GB), so the flush at poweroff is included.
