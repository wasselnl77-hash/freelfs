# Bug scan A: compositor, session, login, lock (2026-11-13)

Read-only pass over src/flcomp, src/flgreet, src/fllock, src/flcrash, src/flsplash, plus the
power path into src/flpanel and config/etc/rc.d. Nothing built, edited or run. Generated
protocol .c/.h files were skipped.

## Coverage (read this first)

- Read in full: src/flgreet/flgreet.c, src/flgreet/flgreet-ui.c, src/fllock/fllock.c,
  src/fllock/fllock-check.c, src/flcomp/main.c, src/flcomp/lock.c, src/flcomp/desktop.sh.
- Read in part: seat.c (keymap head, key dispatch 831-1010), flshell.c (120-200, 240-335),
  media.c (40-260), dmabuf.c (160-330), xwm.c (85-140, 365-490, 1475-1560), xwsel.c (120-200,
  360-400), drm.c (open/close, seat callbacks), gl.c (292-315, 436-450), render.c (36-70,
  408-420), shot.c (64-80), surface.c (8-25, 405-425), anim.c (778-800), flcrash.c (150-230,
  340-352, 560-620, 840-905, 1000-1040, 1260-1270, 1396-1402, 1562-1578). Power-path only:
  flpanel menu.c and panel.c launch(), flpower.c, flui ui.c and wl.c run_pass, config/etc rc
  scripts, rc.conf, inittab, init.d/seatd, recipes/flpanel.
- NOT read in this pass: anim.c (most), xdg.c, surface.c (most), render.c (most), gl.c (most),
  screencopy.c, output.c, overview.c, switcher.c, osd.c, theme.c, config.c, perf.c, sound.c,
  shot.c (most), xwdeco.c, xwsel.c (most), xwm.c (most), seat.c (keymap, pointer, clipboard),
  drm.c (most), flcrash.c (most), src/flsplash/* (flsplash.c, genart.c), flcomp tests/*.sh and
  tests/*.c. "No bug found" for these means not checked.
- desktop.sh: quoting and control flow read in full; no quoting bug found.

## The power path, as it is in the tree

- Panel "Shut down" (src/flpanel/menu.c:388-391) calls launch("flpower off"). launch()
  (src/flpanel/panel.c:330-362) forks, calls setsid, and runs the command through sh -c with
  stderr sent to ~/.local/state/freelfs/log/flpower.log. Nothing reaches the screen.
  flpower (src/flpanel/flpower.c:161-164) does setuid(0) (line 177) and then execl()s
  /usr/sbin/poweroff. The binary has to be root:seat 4750 (recipes/flpanel/recipe:10).
- Greeter "Shut down" (src/flgreet/flgreet-ui.c:470-483) runs as root and execs
  /usr/sbin/poweroff directly. It does not use flpower.
- Restart: there is no restart button in flgreet-ui (470-471 has two buttons: Shut down and
  Log in), in fllock (none), or in the panel menu (menu.c:386-401: Shut down and Log out
  only). The only restart in this area is the R key on the crash screen
  (src/flcrash/flcrash.c:590, do_reboot at 597-601).
- Log out: menu.c:396-400 calls fl_shell_logout, which reaches flshell.c:173-176 (quit(0)).
  desktop.sh:35 then exits 0, and init respawns the greeter.
- Ctrl-Alt-Backspace: seat.c:916-918, quit(0), same result.
- Lock screen: no power action (fllock.c has none, lock.c draws none). Locking hides the
  panel, so a locked laptop has no Shut down control on screen. This is a design gap, not a
  crash.
- Shutdown order: init runlevel 0 runs config/etc/rc.d/rc, which stops services in reverse
  SERVICES order (rc:11-12; rc.conf:17 lists seatd) with SIGTERM (functions:81-82). Only after
  that does rc.shutdown send killall5 -15 to end flcomp (rc.shutdown:15-17).

Most likely causes of a shutdown or restart that "does not work", from this area, in order:
H3 (seatd dies before flcomp, so flcomp quits with status 1 and the crash screen shows during
power-off), H4 (the crash screen's R key fails silently when flpower is not setuid), then M1
(flpower errors go to a log file, not the screen). Runtime verification is needed; see
"Uncertain".

## Findings, ranked

### Critical

C1. src/flcomp/seat.c:874-875, 878-879, 906-907 (handle_binding), with src/flcomp/main.c:85-106
(spawn, run_program) and src/flgreet/flgreet.c:149 and 200-203.
The greeter's compositor runs as root with --firstboot, and handle_binding never checks
C.nolock or firstboot, so the app shortcuts start flterm, general-manager and flfiles as root
before anyone logs in.
Trigger: on the login screen press Ctrl+Alt+T (or Super+T, Super+E, Ctrl+Shift+Esc). A root
terminal or file manager opens with no password. Fix: skip the run_program calls when
C.nolock is set, or do not bind them in firstboot mode.

### High

H1. src/flcomp/flshell.c:173-176 (sh_logout), 247-259 (sh_lock), 308-317 (shell_bind, no
client check), with src/flcomp/lock.c:121-133 (lock_begin).
fl_shell is a global that any Wayland client can bind, and nothing checks which client it is.
So any client can end the session (logout calls quit(0)), take the lock with its own surface
(which then gets keyboard focus), or change the output mode (set_output).
Trigger: any program in the user session that binds fl_shell calls logout or lock. A fake
lock surface then receives the typed password. Not checked: whether the flatpak/flportal
sandbox hides this global from sandboxed apps.

H2. src/flgreet/flgreet.c:126 (pipe without O_CLOEXEC), src/flcomp/main.c:85-100 (spawn does not
close inherited fds), src/flgreet/flgreet.c:157-166 (read loop stops only at EOF or a full
buffer, not at the newline).
The write end of the greeter's pipe is inherited by every program the greeter's flcomp starts,
and flgreet waits for every writer to close.
Trigger: on the login screen press Super+T, then type a name and Enter. flgreet-ui exits, but
flgreet keeps waiting while the terminal is open, so login never starts until that terminal
is closed.

H3. src/flcomp/drm.c:85-89 (seat_cb calls quit(1) when libseat_dispatch fails),
config/etc/rc.d/rc:11-12 and 17-19, config/etc/rc.conf:17, config/etc/rc.d/functions:81-82
(daemon_service stop sends SIGTERM via pid_stop), config/etc/rc.d/rc.shutdown:15-17 (killall5
comes after the services), src/flcomp/desktop.sh:30-35 and 65-69 (non-zero status starts
flcrash).
On runlevel 0, rc stops seatd, and also dbus, iwd and network, before killall5 ends the session.
So flcomp loses its libseat connection while it is still running, quits with status 1, and
desktop.sh starts the crash screen during the power-off.
Trigger: Shut down from the panel or the greeter. Expected evidence: flcomp exits with status 1
in its log, and a "desktop crashed" screen appears during shutdown. The power-off itself still
goes ahead, because rc continues without waiting for flcrash.
Confidence: medium. The claim that libseat_dispatch fails on EOF is assumed, not checked.

H4. src/flcrash/flcrash.c:597-601 (do_reboot), with callers at 886-890 and 1022-1028.
execl() replaces the process, so the /usr/sbin/reboot fallback at line 600 runs only when
/usr/bin/flpower is missing. If flpower exists but fails (no setuid bit, or the user is not in
group seat), flcrash ends with flpower's status 1. The comment at 601 says "stay on the screen",
but both callers return EXIT_SHELL.
Trigger: press R on the crash screen while /usr/bin/flpower is not setuid root (recipes/flpanel
/recipe:10 sets owner= 0 98 4750). Nothing restarts; desktop.sh:65-69 falls through to the shell.

### Medium

M1. src/flpanel/panel.c:330-362 (launch), src/flpanel/menu.c:388-391 (Shut down).
Errors from "flpower off" go to ~/.local/state/freelfs/log/flpower.log, and the menu closes
before anything can be shown, so a failed power-off looks like a button that does nothing.
Trigger: flpower is not setuid, the user is not in group seat, or /usr/sbin/poweroff is missing.
(Owned by the flpanel agent; listed because it is on this path.)

M2. src/flgreet/flgreet-ui.c:470-483 (Shut down).
A second click within 4 seconds is needed (timer armed at 475). The first click only changes the
label to "Click again", and nothing else happens. The child forked at 477 is never waited for
(zombie until flgreet-ui exits), and _exit(127) at 480 hides a missing /usr/sbin/poweroff.
Trigger: one click, then waiting; or a missing poweroff binary.

M3. src/flcomp/main.c:141-148 (panel exit handling).
panel_restarts is never reset, so after five panel exits with code other than 127 in one session,
the panel is not started again. Its Shut down and Log out menu is then gone until the next login.
Trigger: five panel crashes after a login.

M4. src/flcomp/xwsel.c:134-139 (xfer_append), with callers at 275-277 and 295-297.
X-to-Wayland clipboard data has no size cap. MAX_BYTES (xwsel.c:22) is checked only in
wreq_read_cb (379, 390), which is the other direction.
Trigger: an X client offers a multi-gigabyte selection through INCR. The buffer grows until
realloc fails or RAM runs out (the target is 1 GB).

M5. src/flcomp/dmabuf.c:194-199 (plane-0 offset and stride check) and 220-224 (planes 1-3 copied
with no check).
The extra planes' offsets and strides are never checked against the buffer size.
Trigger: a multi-plane buffer (the AMD DCC path) whose plane-1 offset points past its fd. The
import then depends on the driver to reject it. Uncertain (see list).

### Low

L1. src/flcomp/surface.c:416. A client can make a 16384x16384 copy (1 GiB) per surface with no total
cap. It is a denial-of-service risk on a 1 GB machine, not a crash.

L2. src/flcomp/gl.c:442-443. If the readback buffer malloc fails, the frame is not copied and the
old pixels stay on screen. No error is reported.

L3. src/flcrash/flcrash.c:200 and 217. strdup() results are not checked, and log_has() (strstr)
dereferences them. Out of memory only.

L4. src/flgreet/flgreet.c:131-133. On fork failure both pipe ends leak before return -1. The
fallback then execs agetty, which inherits them.

L5. src/flcomp/media.c:63-85 (job_cb). Any read result that is not data (EAGAIN, EINTR, or an
error) finishes the job at once with partial output and closes the pipe. The OSD then shows a
wrong volume.

L6. src/fllock/fllock.c:88-103 with src/fllock/fllock-check.c:105 (flock LOCK_EX, no timeout).
If another fllock-check holds the lock, the screen shows "Checking..." with no timeout. The only
way out is Ctrl+Alt+F2. The fllock-check.c header (lines 18-19) describes a timeout that
fllock.c does not have.

L7. Design gap, not a bug: no power control on the lock screen (lock.c:302-306 locks on lid
close; fllock has no power action). See the power-path notes above.

## Uncertain (needs a runtime check)

- H3: that a seatd stop makes libseat_dispatch fail and flcomp quit(1). Check the flcomp log
  under ~/.local/state/freelfs/log after a test shutdown, and look for "flcomp exited with
  status 1".
- /usr/sbin/poweroff exists in the image. The sysvinit recipe sets base_sbindir=/usr/sbin
  (recipes/sysvinit/recipe:25), but the symlink names were not read, and build output was not
  accessible.
- The seat group (gid 98) has the users. config/etc/group has no seat line, so the group is
  made elsewhere (not found in build/*.sh).
- flpower's setuid bit is applied on an installed laptop. The owner= entries are applied by
  flpkg; this was not checked. Run `ls -l /usr/bin/flpower`.
- H1: whether a flatpak or flportal sandbox hides the fl_shell global.
- C1 severity assumes flterm gives a root shell by default; not checked.
- flui click handling: ui.c fl_widget (17-92) and wl.c run_pass (355-440) read; a click is not
  double-counted. No greeter click bug found there.
