# Bug scan: flui, flpanel, flsettings, fltheme, flsound, fltime, config/etc

Read-only scan, 2026-11-13. Nothing was edited, built, run or committed. Line numbers are from the tree at HEAD c9604b6.

Coverage is partial. The full list is at the end (section 5). Everything listed below was checked in code. Nothing was run on hardware or under QEMU unless stated.

## 1. Priority: Shut down / restart on the live ISO (Ventoy, user `tester`)

### The path, step by step

1. **Panel button.** `src/flpanel/menu.c:386-391`: the start menu has one power button, "Shut down". It calls `launch("flpower off", 0)`. There is **no Restart entry anywhere in flpanel** (grep for "Restart"/"Reboot" finds none). `flpower restart` has no UI caller except `src/flcrash/flcrash.c:599`. If the user looked for a restart button in the panel, it does not exist.
2. **Launch.** `src/flpanel/panel.c:330-364`: `launch()` forks, calls `setsid()`, and execs `/bin/sh -c 'eval "flpower off"'`. Its stdout and stderr go to `~/.local/state/freelfs/log/flpower.log` (`panel.c:351-363`). **Nothing is shown on screen when flpower fails.**
3. **flpower.** `src/flpanel/flpower.c:162` picks `/usr/sbin/poweroff`. At `flpower.c:177-178` it calls `setuid(0)`, which only works if the binary is setuid root. Without the s-bit, `setuid(0)` fails with EPERM, `perror` prints "flpower: Operation not permitted", and the process exits 1. Its mode should be 04750 `root:seat` (`src/flpanel/Makefile:33-35`, `recipes/flpanel/recipe`, `owner=("/usr/bin/flpower 0 98 4750")`). Exec by `tester` also needs group `seat`. The live image adds `tester` to `seat` in `build/53-iso.sh:110-117`, so that part looks right.
4. **How the s-bit gets onto the live system.** The build cannot chown (rootless), so the owner is stored as a pending row in the flpkg database (`src/flpkg/txn.c:749-757`, `:772`). On the live system, `config/etc/rc.d/rc.sysinit:68-86` runs `flpkg fix-owners --keep` (line 73). Its output goes to `/dev/null`, and the only trace is a FAIL on the boot line (line 86). `src/flpkg/cmd_pkg.c:526` returns 0 silently when the database cannot be opened. Whether the flpower row is actually in the live database cannot be checked from the tree.
5. **Shutdown itself.** poweroff (sysvinit `halt`) asks init for runlevel 0 (`config/etc/inittab:11`). Init runs `rc 0` (`config/etc/rc.d/rc:11-21`), which stops the services in reverse order and then runs `rc.shutdown 0`. None of that runs if step 3 fails.

### Most likely cause

`src/flpanel/flpower.c:177-178` (`setuid(0)` returns EPERM), reached because `/usr/bin/flpower` on the live system is not `04750 root:seat`. The owner comes from the rc.sysinit live branch (`config/etc/rc.d/rc.sysinit:73`, output suppressed). The failure is written only to `~/.local/state/freelfs/log/flpower.log` (`src/flpanel/panel.c:351-363`), so the button looks dead.

This is a static trace. It is not confirmed on the laptop. It fits the symptom, and the QEMU tests would not have caught it if the image they test differs from the laptop path.

Checks to run on the laptop (read-only):
- `ls -l /usr/bin/flpower` should show `-rwsr-x--- root seat`.
- `id` should list `seat`.
- `cat ~/.local/state/freelfs/log/flpower.log` should show a line from `flpower` if it failed.
- If Shut down on the **login screen** works (`src/flgreet/flgreet-ui.c:470-481` calls `/usr/sbin/poweroff` directly, as root, with a two-click arm) but the panel's does not, that points at the setuid/seat step.

### Can rc.shutdown hang? (only once the power request reaches init)

- `rc.shutdown:14-18`: `killall5 -15`, `sleep 2`, `killall5 -9`. Bounded.
- `rc.shutdown:20`: `swapoff -a`. No swap in `config/etc/fstab`, so it is a no-op on both live and installed.
- `rc.shutdown:21-22`: `umount -a -r ...`. Busy mounts return EBUSY and the step goes on. Not a hang.
- `rc.shutdown:23` and `functions:46-49`: `remount_root ro` is a no-op on the overlay root.
- `rc.shutdown:24`: `sync` has no timeout. A stalled device with dirty pages would block here forever. (See M5.)
- `init.d/network:43`: `dhcpcd -q -x` has no timeout.
- `functions:58-69`: `pid_stop` waits at most 5 s, then SIGKILL.

### Live versus installed

| | Live (Ventoy ISO) | Installed |
|---|---|---|
| Owner step | `rc.sysinit:73` `fix-owners --keep`, output hidden | `rc.sysinit:73` else-branch, writes the DB, errors visible |
| Root | overlay (`functions:46-47` makes remount a no-op) | btrfs `@`, remounted ro at shutdown |
| `/home` | tmpfs upper in RAM (no `@home`) | btrfs `@home` |
| Shutdown umount list | same `-t` exclusions, so the same mounts are tried | same |
| Clock | `rc.shutdown:12` runs `hwclock --systohc` on the host RTC from inside the live session | writes the installed RTC |
| Flathub remote setup | skipped (`src/flcomp/session:117`) | runs |

## 2. Findings, ranked

### High

**H1. Shut down fails silently when flpower is not setuid (priority).** `src/flpanel/flpower.c:177-178`, owner step `config/etc/rc.d/rc.sysinit:73`. Trigger: live session, start menu, Shut down. flpower exits 1 and the message goes only to `~/.local/state/freelfs/log/flpower.log`. See section 1. Not confirmed on hardware.

**H2. Text field selection is never clamped to the buffer length, so a stale selection can make memmove run far out of bounds.** `src/flui/ui.c:197-218` keeps `_sel` as is. Only `_cur` is clamped (`ui.c:204-205`). `del_range` (`ui.c:177-181`) computes `(size_t)(*len - b) + 1` with `b > len`, which wraps to a huge size. Also `fl_clipboard_set(..., buf + lo, hi - lo)` (`ui.c:227`) reads past the end. Trigger: Ctrl+A in a field (`ui.c:221-224`), the app shortens the buffer while the field keeps focus, then Backspace, Delete, Ctrl+X or Ctrl+C. The trigger is narrow, but the result is memory corruption.

### Medium

**M1. SIGCHLD set to SIG_IGN breaks later waitpid calls, so helper errors are reported wrongly.** `src/flsettings/flsettings.c:140` (`play_test`) and `:153` (`spawn`) set `SIGCHLD` to `SIG_IGN` in the parent and nothing restores it. After that, `waitpid` returns ECHILD: `src/flsettings/datetime.c:143` (`helper`), `src/flsettings/snapshots.c:93` (`run_flsnap`), `src/flsettings/about.c:189` (`job_end`). Trigger: in Settings, play the sound test (`flsettings.c:196`) or use Themes / GTK hints (`flsettings.c:167-178`), then change the time zone, set the time, or switch NTP (`datetime.c:172`, `:333`, `:417`, `:427`). The UI then says "The helper did not finish." even though `fltime-set` may have applied the change. Based on POSIX waitpid semantics; not run.

**M2. Out-of-bounds read and cache thrash in the fallback-font miss table.** `src/flui/text.c:41` (`uint32_t miss[16]`), `:42` (`int nmiss`). `text.c:373-374` scans `i < f->nmiss` without capping at 16, so it reads past the array into `nmiss`, `ui`, `base` and `ui_next`. `text.c:405` (`f->miss[f->nmiss++ % 16]`) lets `nmiss` grow without bound. Consequence: undefined behaviour in the lookup, and once more than 16 distinct code points are missing, every glyph draw reruns `FcFontMatch` (`text.c:385`) because the ring has evicted the entry. Trigger: a notification or file name with 17 or more distinct code points that no installed font has (emoji or CJK on a system without those fonts).

**M3. Settings silently lost on rewrite: 64 keys, 95-byte values, 159-byte lines.** `src/flui/conf.c:73` (`MAXKEYS 64`) and `:99` (new keys dropped silently when full). `conf.c:75` (`val[96]`, truncated by snprintf at `:96`, `:101`). `conf.c:112` (`fgets` into 160 bytes, so a longer line splits and the tail is parsed as a new key). `fl_conf_set_many` (`conf.c:190-226`) rereads the file, then rewrites it from `kv[]` (`conf.c:217-218`). Trigger: a `desktop.conf` with more than 64 keys, a value of 96 or more characters, or a line of 160 or more characters. The next change made in Settings deletes or truncates them.

**M4. No restart control exists in the panel.** `src/flpanel/menu.c:386-391` has only "Shut down". `flpower restart` (`flpower.c:163-164`) has no panel caller. This is not a code crash, but it explains a "restart button does not work" report: there is no such button.

**M5. No timeouts on sync and on the DHCP release during shutdown.** `config/etc/rc.d/rc.shutdown:24` (`sync`), `config/etc/rc.d/init.d/network:43` and `:25` (`dhcpcd -q -x`). If either blocks, init never finishes runlevel 0 and the machine does not power off. Trigger: a removable or stalled device with queued writes, or a dead link during the DHCP release. Whether this actually hangs is uncertain (section 4), but nothing bounds it.

**M6. Root clock is stepped by any server answer, with no sanity limit.** `src/fltime/fl-sntp.c:156-163` (`apply` steps the clock by any offset of 0.2 s or more, with no maximum). `fl-sntp.c:96-97` accepts the first answer whose origin timestamp matches. Trigger: an on-path attacker on the same network (for example open Wi-Fi) answers the query. Certificate checks and logs then see a wrong time.

### Low

**L1. Byte truncation splits UTF-8 in `fl_text_fit`.** `src/flui/text.c:514-516`: when the text fits in width but its bytes reach `cap`, it is cut at `cap-1` bytes, with no ellipsis and possibly mid-character. Reachable: `src/flsettings/wallpaper.c:702-703` uses `char fit[48]`, so a non-ASCII wallpaper name of 48 bytes or more that fits the width shows a broken glyph. Latent: `text.c:532-535` writes 4 bytes (ellipsis plus NUL) when `cap < 4`, and `text.c:513` with `cap == 0` gives `n = SIZE_MAX`. All current callers pass `cap >= 48`, so these are not reachable today.

**L2. Paste into a text field splits UTF-8.** `src/flui/ui.c:244-249` inserts clipboard bytes one at a time and stops at `cap`, so a multibyte character can be left half-written. Trigger: paste multibyte text into a nearly full field.

**L3. Passphrase paste is not NUL-terminated and cuts mid-character.** `src/flsettings/wifi.c:894-896` appends clipboard bytes to `pw` without setting `pw[pwlen] = 0`. It works only because the buffer is kept zeroed (`wifi.c:34`, `:911`, backspace at `:884-885`). The cut at `PWMAX` (63 bytes) can split a UTF-8 character. Trigger: paste a passphrase that is long or non-ASCII.

**L4. Clipboard read: unchecked malloc, realloc leak, 1 MB cut.** `src/flui/wl.c:72` (malloc not checked), then `read` into `buf` at `:85` without a check. `wl.c:81` (`buf = realloc(buf, cap)`) loses the old block when it fails. `wl.c:76-80` stops at 1 MB, which can cut mid-character. Trigger: out of memory, or paste of more than 1 MB.

**L5. fl_fd_add result ignored in fl_timer_new.** `src/flui/wl.c:1368`. If the array grows out of memory, the timer is never polled. Trigger: out of memory only.

**L6. NULL font dereference when no font is found.** `src/flui/kbd.c:97` calls `fl_font_height(name_font)`, and `src/flui/text.c:274` dereferences the pointer without a check. `fl_font_mono` and `fl_font_ui` return NULL when nothing matches (`text.c:107-130`, `:242-245`). Trigger: a system where fontconfig finds no usable font.

**L7. notify_free does not free the three timers.** `src/flpanel/notify.c:1404-1411` frees `nt` but not `nt->tick`, `nt->retry` or `nt->save` (created at `notify.c:1376-1378`). Their fd entries keep pointing at the freed struct. It runs only at exit (`src/flpanel/panel.c:1071`), so it is harmless today.

**L8. Battery: mixed units summed, and int overflow on a tiny rate.** `src/flpanel/battery.c:203-214`: energy readings (uW) and charge readings (uA) are both added to `rate`. `battery.c:265` and `:267` cast `e/ema*3600` to int with no range check, which is undefined behaviour when the rate is very small. Trigger: two batteries of different kinds, or a near-zero reported current.

**L9. hints.c drops the tail of a large settings.ini.** `src/fltheme/hints.c:40-42` reads at most 65536 bytes, and `ini_set` writes back only what it read. Trigger: a `~/.config/gtk-3.0/settings.ini` larger than 64 KiB loses the rest on rewrite.

**L10. run_flsnap stops draining the pipe at cap.** `src/flsettings/snapshots.c:88-89`. When output reaches the buffer size the parent stops reading and closes the pipe. The child then gets SIGPIPE or EPIPE on its next write. Trigger: flsnap output larger than the caller's buffer.

**L11. desktop.conf values keep leading whitespace.** `src/flui/conf.c:125-130` trims only the right side of the value and the right side of the key, so `theme = light` is read as `" light"`, which is not `light`. Trigger: spaces around `=` in a hand-edited desktop.conf. The same parser is used by flsound (`src/flsound/flsound.c:190-194`), which does not trim at all.

**L12. Stale pid files: the wrong process can be signalled or skipped.** `config/etc/rc.d/functions:53-54` (`pid_alive`) and `:58-68` (`pid_stop`) act on the PID in the file without checking the process. A stale file with a reused PID makes `daemon_service start` skip the start (`functions:79`), and `stop` sends SIGTERM to whatever now owns that PID (`functions:62`, `:67`). Trigger: a daemon crashes and leaves its pid file, and the PID is reused.

## 3. Checked, no bug found

- `src/flpanel/flpower.c`: argument handling. Only fixed words reach exec and sysfs paths (`flpower.c:161-172`). The environment is cleared before exec (`:181-182`). The saver state file is root-only with umask 077 (`:170`).
- `src/fltime/fltime-set.c`: zone names are checked against a strict grammar (`:55-80`), realpath containment and a TZif magic check (`:83-100`), the environment is cleared, and the ntp path is fixed at build time.
- `src/fltime/fl-sntp.c`: the origin-timestamp match (`:96-97`) and the bounds on the server list (`:171-190`).
- `src/flui/text.c:339-369` (`utf8_next`): bounds and continuation checks are correct.
- `src/flsettings/wifi.c`: the passphrase buffer is wiped on send, cancel and prompt end (`wifi.c:482`, `:753`, and the wipe in `send_passphrase`).
- `config/etc/rc.d/*`: every variable expansion in the rc scripts is quoted, and no config value is passed to eval.
- `src/flui/wl.c:1473-1555` (main loop): the fd arrays are index-stable across callbacks, and the -2 "more room" path is handled.

## 4. Uncertain

- Whether the flpower row is in the live database, and whether `fix-owners --keep` applies it. This decides H1. It needs the live `/var/lib/flpkg/db.sqlite`.
- Whether `sync` or `dhcpcd -q -x` actually stalls shutdown on the laptop (M5).
- sysvinit's `halt` without `-f` and how it signals init, and whether init's session setup makes `killall5` (`rc.shutdown:14-17`) spare or hit init's own session. Not verified against sysvinit source.
- Whether the Shut down button needs two clicks on the login screen only (`flgreet-ui.c:470-481` arms, then runs on the second click within 4 s). The panel button has no such step.
- `hwclock --systohc` (`rc.shutdown:12`) writing the host RTC from a live session: intended or not.
- M2's out-of-bounds read: the struct layout after `miss[]` is read from the header, not checked with a compiler.

## 5. Coverage

- **Read in full:** `src/flpanel/flpower.c`, `src/fltime/fltime-set.c`, `src/fltime/fl-sntp.c`, `src/flui/conf.c`, `src/flui/kbd.c`, `src/flui/internal.h`, `src/flui/text.c`, `src/fltheme/hints.c`, `src/flpanel/perfmode.c`, `config/etc/inittab`, `config/etc/rc.conf`, `config/etc/fstab`, `config/etc/rc.d/rc`, `config/etc/rc.d/rc.shutdown`, `config/etc/rc.d/rc.sysinit`, `config/etc/rc.d/functions`, every `config/etc/rc.d/init.d/*`, `config/etc/group`, `passwd`, `profile`, `hosts`, `hostname`, `shells`, `issue`, `inputrc`, `os-release`.
- **Read in part:** `src/flui/ui.c` (lines 1-60 and 135-372, the text field and widget primitive), `src/flui/wl.c` (920-1200, 1245-1555), `src/flui/gfx.c` (1-170, 470-530), `src/flpanel/battery.c` (1-300), `src/flpanel/notify.c` (history, setup and teardown), `src/flpanel/menu.c` (power and launch section, refilter), `src/flpanel/panel.c` (vol_spawn, SIGCHLD, window list), `src/flsettings/wifi.c` (passphrase path), `snapshots.c` (55-100), `datetime.c` (115-160), `flsettings.c` (125-160), `src/flsound/flsound.c` (config read and tick).
- **Not read:** `src/flui/worldmap.c` and `worldmap_data.c` (data table and path code), `ui.c` widgets outside the text field, `gfx.c` rasteriser body, `wl.c` window and registry code (about 520-900 and 1000-1100 outside the clipboard path), `panel.c` main body, `notify.c` card drawing and D-Bus body, `deskicons.c`, `wsmenu.c`, `flyout.c`, `flnotify.c`, `menu.c` list drawing, `battery.c` 300-430, `src/flsettings/` wallpaper, pages, workspaces, about, datetime body and the rest of wifi, `src/fltheme/fltheme.c` and `presets.c`, the PipeWire body of `flsound.c`, the generated protocol headers, and `tests/`.

Coverage is not complete. The uncovered files are most of flsettings, fltheme, and the flpanel and flui drawing code. Another pass is needed before calling the scan complete.
