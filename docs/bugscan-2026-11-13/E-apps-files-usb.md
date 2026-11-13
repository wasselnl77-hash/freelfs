# Bug scan E: apps, Files and USB drives (2026-11-13)

Scope: src/flfiles, fledit, flgm, snappel, flmusic, flarchive, flpackages, flview, flterm, flvideo,
flpdf, flsetup, flreport, flupdate, drawtest. Read-only; nothing built, run or committed.

## Part 1: why Files shows no USB stick (priority)

Short answer: nothing in the live system mounts a removable disk after boot, and Files only lists
mounts under /drives/. A stick plugged in after boot is never mounted, so it cannot appear. The
Ventoy data partition is mounted, but at /run/live/src, which Files ignores. VTOYEFI is never mounted.

Causes, most likely first:

1. Nothing mounts removable block devices after boot. config/etc/fstab has only the root, /home,
   /.snapshots, /boot and virtual entries. config/etc/rc.d/rc.sysinit:62 runs `mount -a` once at
   boot. No udisks or automount recipe exists in recipes/. The udev recipe installs only stock rules
   (recipes/udev/recipe:59-60). The repo's own rules are gaming and backlight only
   (recipes/gaming-config/71-freelfs-gaming.rules; src/flsettings/Makefile:31). No src/ program mounts
   a plugged device. The only other mount() calls are install-time (flinstall helper, flpkg triggers)
   and flinitrd at boot.
   Trigger: boot the live ISO, plug in a stick. /proc/mounts gains nothing.

2. flfiles lists only /proc/mounts entries under /drives/. src/flfiles/flfiles.c:799 reads
   /proc/mounts, and :818 drops every mount not under /drives/NAME. No second source exists (no
   /sys/block walk, no mountinfo, no /run/media). The DRIVES section then prints "No other drives"
   (flfiles.c:2018).
   Trigger: any stick mounted elsewhere, such as /run/live/src, is invisible.

3. The Ventoy data partition is mounted, outside /drives. flinitrd mounts the exFAT partition at
   /run/live/src (src/flinitrd/flinitrd.c:371). On success, try_device_files() returns 0 at :386
   without unmounting it; the umount at :391 runs only on failure. So /proc/mounts shows
   "exfat on /run/live/src ro", which flfiles.c:818 drops.
   Trigger: boot the Ventoy stick, open Files: the stick is not listed. (Not confirmed with a live
   /proc/mounts capture; running the ISO was out of scope.)

4. VTOYEFI (vfat) is never mounted. find_live() stops at the first working device (flinitrd.c:419-422),
   and nothing scans again after switch_root.

5. A stick replugged after boot is never seen. flinitrd is PID 1 of the initramfs only. udevd runs
   (rc.sysinit:35-39) with no mount rule. The "known" dedupe at flinitrd.c:408-411 is boot-only.

Hidden on purpose? Partly. flinitrd keeps the boot stick read-only by design (header, flinitrd.c:12-15).
Nothing filters the stick out of Files on purpose: it is hidden by omission plus the /drives-only filter.

6. tester cannot mount, and no helper does it for them. mount(2) needs CAP_SYS_ADMIN. tester is uid
   1000, primary group users (build/53-iso.sh:105). util-linux is not setuid (its only owner line is
   /usr/bin/wall, recipes/util-linux/recipe:10). No udisks exists. fusermount3 is setuid
   (recipes/fuse3/recipe) but no FUSE exFAT or NTFS driver is packaged. The pattern to copy is in
   the tree: flpkg-seat (recipes/flpackages/recipe owner 0 98 4750) and flinstall-helper.

7. Files refreshes drives only on focus or refresh (flfiles.c:1515, :2242 via places_load). No mount
   change notification exists. This matters once something does mount.

## Part 2: ranked findings

Format: file:line, bug, trigger.

### Critical

- config/etc/rc.d/rc.sysinit:62 and config/etc/fstab: removable media are never mounted outside boot.
  Trigger: plug in a USB stick after boot. Nothing appears in /proc/mounts or in Files. (Part 1, 1 and 5.)

### High

- src/flinitrd/flinitrd.c:192-198, called at :307 (try_device_iso) and :368 (try_device_files): make_readonly()
  sets BLKROSET on every device probed before the live medium is found, and nothing clears it (no
  BLKROSET 0 anywhere). The probe order is by name, so disks and partitions sorted before the stick
  (for example an internal sda and its partitions) stay kernel-read-only for the session.
  Trigger: internal disk sda, boot stick sdb. sda and its partitions remain read-only after boot, so
  installs or copies to them fail with EROFS. (Code verified; BLKROSET persistence is from kernel
  knowledge, untested.)

- src/flinitrd/flinitrd.c:371 and :382-386: on success the exFAT partition stays mounted at /run/live/src
  and is never offered to the user. Trigger: boot the Ventoy stick; /proc/mounts shows it, Files does not.

- src/flfiles/flfiles.c:799-820 (filter at :818): the Drives section shows only /drives/NAME mounts.
  Trigger: any removable mount outside /drives, including /run/live/src, is never listed.

- src/flfiles/flfiles.c:1585-1596 (menu) and :38-39 (actions): there is no mount, unmount, eject or
  "copy to stick" action. The docs promise one: src/flsettings/about.c:329-330 says "copy the file to a
  USB stick with Files before you shut down", and src/flreport/README says the same.
  Trigger: try to use a stick from Files, which is impossible today.

- tester cannot mount (Part 1, 6). Design gap: every fix needs a root helper.

### Medium

- src/flarchive/flarchive.c:604-615: `struct dirent **names; n = scandir(...)`, then `free(names)` runs
  even when scandir returns -1, when names was never set. Trigger: create an archive from a folder that
  contains an unreadable subfolder (chmod 000). The child crashes, the job reports "Stopped.", and
  ".name.zip.part" is left in the destination.

- src/flview/flview.c:139-147: exif_orientation() computes `off + 2` in 32-bit unsigned (`unsigned off`
  compared with size_t tn). For off >= 0xFFFFFFFE the sum wraps, the check at :140 passes, and
  U16(t + off) at :142 reads about 4 GB past the APP1 buffer. The loop bound at :144 wraps the same way.
  Trigger: a JPEG whose Exif IFD0 offset is 0xFFFFFFFE. Opening the folder (thumbnails decode every
  file) crashes flview.

- src/flview/flview.c:318-331: the animated WebP branch has no pixel cap. WebPAnimDecoder allocates the
  full canvas, then flview allocates and copies a second one. Trigger: an animated WebP with a 16383 x
  16383 canvas, about 1 GiB twice, on the 1 GB target.

- src/flview/flview.c:207-208 and :218: the JPEG scale loop stops at denom 8 without rejecting large
  images (PNG rejects at :260). Trigger: a JPEG with a 65535 x 65535 header decodes to about 8191 x 8191
  x 4, roughly 256 MiB per thumbnail.

- src/flpdf/flpdf.c:311-333: add_outline() recurses on o->down with no depth cap, and the realloc failure
  at :316-318 leaves p->toc NULL. Trigger: an outline thousands of levels deep (depends on MuPDF; see
  Uncertain).

- src/flmusic/mpris.c:238-245: handle_set() reads iface and name with dbus_message_iter_get_basic() before
  checking the argument type, then uses them in strcmp (:245). Trigger: a same-user D-Bus client sends a
  malformed Set call, which crashes flmusic (denial of service).

- src/flfiles/flfiles.c:1325-1343 (egg_allows): the vital-folder check is a prompt, not a guard. If flcrash
  is missing, or the person chooses "Continue", deleting or trashing /boot or /sysflfs goes ahead.
  Trigger: select /boot, press Del, choose Continue. Design; worth a note.

- src/flfiles/flfiles.c:268-300 (copy_tree) and :352 (move_path): the chmod result at :295 is ignored, and
  copy has no progress or cancel, so a large copy holds the window's loop (src/flfiles/README says so).
  Trigger: a multi-GB copy freezes Files. This also blocks copy-to-stick.

- src/snappel/cap.c:266 and src/snappel/img.c:58: with XDG_RUNTIME_DIR unset, the clipboard PNG goes to a
  predictable /tmp/snappel-clip-PID.png, and img_save_png() fopens "PATH.part" with "wb", which follows
  symlinks. Trigger: another local account pre-creates the symlink; the write lands on a file the user owns.

- src/flreport/flreport:224-226: the report makes a DNS lookup and a TLS request to example.com while it
  collects data. The code shows it; the README does not. Trigger: "Save a report" with a network connects
  out. Privacy note.

- src/flreport/flreport:15 (out_dir=${HOME:-/tmp}): on the live system HOME is RAM. With Part 1, the report
  cannot reach a stick and is lost at shutdown. Trigger: save a report on the ISO, then try to copy it to a
  stick with Files.

### Low

- src/flarchive/flarchive.c:920-922: a->apath is cleared before base_name(a->apath) builds the error title,
  so the message starts with ": ". Cosmetic.
- src/flarchive/flarchive.c:868: sscanf("%d\t%n") skips leading spaces of the name, so " notes" lists and
  selects as "notes", and extraction says "Nothing to extract." Trigger: a file name starting with a space.
- src/flarchive/flarchive.c:155-163: child_term() calls nftw()/remove() from a SIGTERM handler, neither of
  which is async-signal-safe. Trigger: cancel an extraction at the wrong moment; rare deadlock or corruption.
- src/flmusic/flmusic.c:885-903 (mp_open, MPRIS OpenUri): any same-user bus client can queue any readable
  file, and add_track() has no limit. Trigger: repeated OpenUri calls.
- src/flmusic/flmusic.c:429-446 (cache_save): paths are written without removing TAB or newline (titles are
  cleaned at :124). Trigger: a file name with a tab breaks its cache line and is rescanned on every start.
- src/flterm/term.c:683-689 and src/flterm/main.c:128-132: OSC 0/2 titles pass through unfiltered, including
  bytes 0x80 and up (bidi overrides such as U+202E). Trigger: `cat` a file that sets a title; the window
  title is spoofed. Good: no CSI 21t title report and no OSC 52 are implemented, so output cannot inject
  into the shell's input.
- src/flterm/term.c:746-749: the UTF-8 decoder accepts overlong, surrogate and above U+10FFFF sequences.
  Trigger: crafted output bytes; display glitch only.
- src/flupdate/flupdate-notify:93-96: `bg="$bg $!"` grows on each check and the background subshells are
  never reaped in the loop. Trigger: running for weeks; small zombie buildup.
- src/flupdate/flupdate-notify:53 and :59: `grep -c .` counts every output line, so a header line from
  flpkg or flatpak counts as an update. Trigger: depends on their output format (Uncertain 6).
- src/flsetup/flsetup.c:72: strerror(errno) runs after fl_kbd_conf_write(), which may not set errno.
  Trigger: a failed save shows the wrong reason.
- src/fledit/fledit.c:446-611 (undo_push and do_replace): if the undo push fails, del and dup leak. OOM only.
- src/flgm/startup.c:192: write_atomic() uses a fixed "PATH.tmp" with fopen("we"), which follows a symlink in
  the same directory. Trigger: needs write access to the config directory.

## Part 3: what Files needs to work with sticks

1. A root helper for removable media, in the flpkg-seat / flinstall-helper pattern: a setuid (or
   udev-triggered) `flmount` with group seat and a recipe owner line. Commands: `list` (removable or USB
   block devices from /sys/block/*/removable and the bus, with label, fs type, mount state), `mount DEV`,
   `umount DEV`, `eject DEV` (sync, umount, then `echo 1 > /sys/block/X/device/delete`). Checks: accept only
   names from its own listing, never paths; refuse non-removable or non-USB devices; refuse /run/live/* and
   the live medium's device; refuse the EFI partition (type C12A7328-F81F-11D2-BA4B-00A0C93EC93B, or label
   VTOYEFI). Mount at /drives/LABEL (else UUID, else devN) with nosuid,nodev,noexec, uid=/gid=/umask= for
   exFAT and vfat, and a chown of the root for btrfs and ext4. A udev rule (ACTION=="add", SUBSYSTEM=="block",
   ENV{ID_BUS}=="usb", ENV{ID_FS_USAGE}=="filesystem") can call the same helper for automount, with the same
   checks; the ID_FS_* values come from the blkid that udev already provides.
2. Files: the Drives section reads the helper's list plus /proc/self/mountinfo (not only /drives). Poll
   mountinfo for POLLPRI so a new stick appears without a focus change. Context menu: Mount, Unmount, Eject.
   Read-only devices (the live medium) get a badge and no write actions.
3. Files: "Copy to USB stick" on a selection. Pick a mounted removable place, check free space with statvfs,
   copy with progress in the window loop (the flarchive progress pattern, not the blocking copy_tree), sync,
   then offer Eject. Report EROFS and similar errors by name.
4. src/flportal/filechooser.c:252 has the same /drives-only filter. Route its places through the same helper
   so Save dialogs reach a stick.
5. src/flinitrd/flinitrd.c: (a) set BLKROSET only on the device that is loop-mounted, and clear it (BLKROSET 0)
   on every other probed device before switch_root; (b) keep the Ventoy partition mounted (the loop needs it)
   and list it in Places as "Live medium (read-only)"; (c) never probe VTOYEFI with make_readonly.
6. Kernel and userspace: CONFIG_NTFS3_FS=y for Windows sticks (flinstall/helper.c:1464 already writes ntfs3 into
   fstab, but config/kernel/freelfs.config has no NTFS option). exfatprogs is missing, so sticks cannot be formatted
   or checked as exFAT. exFAT itself is built in (freelfs.config:151), and USB storage, UAS, SD and VFAT are present.
7. flreport: when a removable place is mounted, offer it as the save location; on the live ISO say that the file is
   in RAM until it is copied.

## Part 4: checked, no finding

- Shell injection: no system() or popen() in the area. Every exec uses an argv array (flfiles.c:877, 1143, 1178,
  1340; flpackages.c:369, 1177; snappel.c:119, 206; flterm/main.c:111-117; flpkg-seat.c:107). flreport and
  flupdate-notify pass only fixed strings to sh.
- flpkg-seat.c: name validation (no leading "-", "/", "..", or .flpkg), environment clearing, fd and signal reset,
  and privilege drop are correct.
- flfiles do_paste (:1262-1300): the "folder inside itself" test runs in the right direction.
- flpdf page_at() never returns -1, so the click path is safe. The --page argument is guarded.
- flterm: no clipboard write and no title report, so output cannot inject into input.
- flarchive extraction: leading "/" and "./" stripped, ".." blocked by ARCHIVE_EXTRACT_SECURE_NODOTDOT, absolute
  paths and symlinks guarded, setuid bits removed. Remaining gap: Uncertain 1.

## Uncertain (verify)

1. flarchive hardlinks (flarchive.c:368-375): norm_path() does not strip "..", and an absolute hardlink target is
   not rejected in this code. Whether libarchive's SECURE_* flags stop a hardlink to ../../ or to a user file
   outside the folder was not checked (libarchive source is not in the repo). Highest priority here: a crafted
   archive could hardlink a user file into the extraction folder. Suggest an explicit reject of ".." and leading
   "/" in hardlink targets.
2. BLKROSET persistence (flinitrd.c:192-198): kernel behavior from knowledge, not tested; whether a whole-disk
   BLKROSET also covers its partitions was not confirmed.
3. The Ventoy partition left mounted at /run/live/src (flinitrd.c:371, :382-386): code path confirmed; no live
   /proc/mounts capture.
4. The exif wrap (flview.c:139-147) assumes a 32-bit unsigned int, as with gcc on x86_64. The crash is from
   reading; exploitability beyond a crash was not assessed.
5. flpdf outlines (flpdf.c:311-333): depends on MuPDF's outline builder (not in the repo). Cycles and depth were
   not checked.
6. flupdate-notify counting (`grep -c .`): depends on flpkg and flatpak output format, not checked.
7. glibc scandir() leaves *namelist untouched on failure (from knowledge); the free() at flarchive.c:615 is
   undefined either way.
8. Whether exFAT mounts with uid=/gid=/umask= are writable for the seat user on the target: not tested.

## Coverage

Read in full: src/flreport/flreport; src/flsetup/flsetup.c; src/flupdate/flupdate-notify; src/flpackages/flpkg-seat.c;
config/etc/fstab; the mount-relevant parts of src/flinitrd/flinitrd.c (1-30, 131-200, 200-325, 355-500);
config/etc/rc.d/rc.sysinit (1-63); the USB, exFAT, VFAT, FUSE and NTFS lines of config/kernel/freelfs.config;
the setuid owner lines in recipes/.

Read in part: src/flarchive/flarchive.c (38-240, 237-500, 498-700, 863-1120); src/flfiles/flfiles.c (222-470,
700-840, 863-870, 980-1150, 1240-1390, 1505-1570, 1980-2030, 2232-2300); src/flview/flview.c (126-432, 539-570,
and grep of 1037-1100); src/flpdf/flpdf.c (133-170, 246-275, 311-410, 593-645, 880-900, 985-1005);
src/flmusic/flmusic.c (124-262, 429-600, 885-904) and mpris.c (1-20, 232-300); src/flterm/term.c (300-315, 455-470,
571-600, 680-860) and main.c (95-135); src/flpackages/flpackages.c (180-195, 340-420, 755-780, 1550-1575);
src/fledit/fledit.c (446-560) and syntax.c (300-322); src/snappel/snappel.c (84-125, 740-790), cap.c (255-289),
img.c (55-95); src/flgm/proc.c (290-315), sys.c (385-400), startup.c (185-205).

Not read: src/flvideo/flvideo.c; src/drawtest/drawtest.c; src/flgm/gm.c and perf.c; most of flgm/sys.c and startup.c;
most of fledit.c and syntax.c; most of snappel.c and cap.c; snappel/fl-screencopy-*; mpris.c beyond the ranges above;
flpackages UI and most parsing; flview UI and render; flpdf render and draw; the flfiles open, load and keys paths.
These are the next pass. Estimated coverage of the about 26.7k lines in the area: roughly a third, mostly the risky parts.
