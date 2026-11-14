# 14. Services and the init system: rc.conf, rc.d scripts, udev rules

This chapter explains FreeLFS's own init setup: sysvinit with shell scripts, `/etc/rc.conf`, every system service, the udev rules, and how to enable, disable, start and stop a service.

Status: written from `config/etc`, `recipes/initscripts` and the packages named below, at round 17 (2026-11-14).

## Contents

1. [Init and runlevels](#141-init-and-runlevels)
2. [/etc/rc.conf](#142-etcrcconf)
3. [The rc scripts](#143-the-rc-scripts)
4. [The services](#144-the-services)
5. [Session services (not in rc.conf)](#145-session-services-not-in-rcconf)
6. [udev and its FreeLFS rules](#146-udev-and-its-freelfs-rules)
7. [Enable, disable, start, stop](#147-enable-disable-start-stop)
8. [Adding a service](#148-adding-a-service)
9. [Known issues](#149-known-issues)

## 14.1 Init and runlevels

FreeLFS uses **sysvinit 3.14** (`init` as PID 1) with **our own** boot scripts. There is no systemd. The files come from `config/etc` and are installed by `recipes/initscripts` (version 0.1 release 11).

`/etc/inittab`:

| Line | What it does |
|---|---|
| `id:3:initdefault:` | the default runlevel is 3: multi-user, consoles; the desktop starts from tty1 |
| `si::sysinit:/etc/rc.d/rc.sysinit` | the boot setup, once, first |
| `l0` to `l6` | `/etc/rc.d/rc 0` to `rc 6` |
| `0` | halt (rc.shutdown) |
| `1`, `S` | single-user: services stop |
| `2` to `5` | start the enabled services |
| `6` | reboot (rc.shutdown) |
| `su:S06:once:/sbin/sulogin` | a root shell in single-user mode |
| `1:2345:respawn:` | tty1: `flgreet` (the login screen); if it is missing, the boot splash in text mode and `agetty` |
| `2` to `6` at `2345` | `agetty` on tty2 to tty6 |
| `S0:2345:respawn:` | serial console ttyS0 (QEMU, servers), speeds 115200, 38400, 9600 |
| `ca:12345:ctrlaltdel:` | Ctrl+Alt+Delete reboots |

The boot sequence is in [11](11-boot-process.md). Runlevel 5 is named in the file but not used by a display manager yet ("later" in the comment).

## 14.2 /etc/rc.conf

`/etc/rc.conf` is sourced by `rc`, `rc.sysinit`, `rc.shutdown` and the network script. Shipped content (`config/etc/rc.conf`):

| Variable | Default | Meaning |
|---|---|---|
| `HARDWARECLOCK` | `UTC` | `UTC` or `localtime`; the RTC is read at boot and written at shutdown in this mode |
| `KEYMAP` | empty | console keymap for `loadkeys`; empty means the layout in `/etc/keyboard.conf`, else the kernel default |
| `CONSOLEFONT` | empty | console font for `setfont` on tty1 to tty6 |
| `MODULES` | `()` | kernel modules to load at boot, in order (`modprobe -a`) |
| `SERVICES` | `(syslog dbus network seatd iwd bluetooth cups ntp)` | services to start, in order; they stop in reverse order. Prefix a name with `!` to disable it |
| `NETWORK` | `()` | interfaces for the network service: `"eth0 dhcp"` or `"eth0 static 192.168.1.10/24 192.168.1.1"` (empty means DHCP on every interface, see [16](16-networking.md)) |

Example, with Bluetooth and printing off and a fixed address on the wired port:

```sh
SERVICES=(syslog dbus network seatd iwd !bluetooth !cups ntp)
NETWORK=("eth0 static 192.168.1.10/24 192.168.1.1")
```

The `!name` entries stay in the list, so they are easy to turn back on.

`/etc/rc.local`, when it exists and is executable, runs at the end of `rc 2` to `rc 5`.

## 14.3 The rc scripts

| File | Run by | What it does |
|---|---|---|
| `/etc/rc.d/functions` | sourced by all scripts | status lines (`stat_busy`, `stat_done`, `stat_fail`, `step`), `daemon_service` (start, stop, restart, status for a daemon with a pid file), `enabled_services`, `say`, `text_console`, `force_power`, `shutdown_watchdog`, the splash hooks |
| `/etc/rc.d/rc.sysinit` | init, `si` | one-time boot setup: mounts, udev, fsck, drives, clock, keyboard, sysctl, login records (step list in [11](11-boot-process.md)) |
| `/etc/rc.d/rc` | init, runlevels 0 to 6 | starts or stops the services; on 0 and 6 it stops the desktop, the services and hands over to `rc.shutdown` |
| `/etc/rc.d/rc.shutdown` | `rc 0` and `rc 6` | saves the clock, kills the rest, unmounts, halts or reboots |
| `/etc/rc.d/init.d/NAME` | `rc` and by hand | one script per service: `start`, `stop`, `restart`, `status` (the network script also has `status`) |

`rc 0` and `rc 6` run this order: write `/run/flshutdown`, arm the 15 s watchdog, stop flgreet and flcomp, stop the services (8 s each), then `rc.shutdown` (see [11](11-boot-process.md), 11.7).

## 14.4 The services

Services are the names in `SERVICES`. The scripts are in `config/etc/rc.d/init.d/`. Pid files are in `/run`.

| Service | Script does | Program | Pid file, log | Notes |
|---|---|---|---|---|
| `syslog` | starts `syslogd -m 0` | sysklogd 2.7.2 | `/run/syslogd.pid` | the system logger |
| `dbus` | `dbus-uuidgen --ensure`, `dbus-daemon --system` | D-Bus 1.16.2 | `/run/dbus/pid` | the system bus. Needed by Wi-Fi and Bluetooth settings |
| `network` | runs `dhcpcd` (DHCP on all interfaces, or the `NETWORK` entries) | dhcpcd 10.5.2 | none; `status` shows `ip -brief addr` | wired only. Wi-Fi interfaces are denied to dhcpcd (see [16](16-networking.md)) |
| `seatd` | starts `seatd -g seat` | seatd 0.9.3 | `/run/seatd.pid` | gives the desktop access to the screen, GPU and input devices for group `seat` |
| `iwd` | starts `iwd` with `/usr/lib/iwd/bin` first in PATH | iwd 3.12 | `/run/iwd.pid`, log `/var/log/iwd.log` | Wi-Fi; needs dbus |
| `bluetooth` | starts `bluetoothd` **only if a Bluetooth adapter exists** (`/sys/class/bluetooth/hci*`); otherwise it reports "not started" and exits 0 | BlueZ 5.88 | `/run/bluetoothd.pid`, log `/var/log/bluetooth.log` | a default pairing agent (NoInputNoOutput) starts with it. `maybe-stop` is called by udev when the adapter goes away |
| `cups` | starts `flsocketd` on `/run/cups/cups.sock`, which runs `/usr/libexec/flcupsd` | flsocketd, CUPS 2.4.20 | `/run/flsocketd-cups.pid` | printing **on demand**: cupsd and avahi start at the first connection and stop after 60 s without a job (`config/cups/flcupsd`) |
| `ntp` | starts `fl-sntp` (SNTP client) unless `NTP=0` in `/etc/fltime.conf` | fltime (`fl-sntp`) | `/run/ntp.pid` | sets the clock from pool.ntp.org, or the servers in `SERVERS=` of `/etc/fltime.conf`. Settings > Date & time switches it through `fltime-set` |

`udev` is **not** in `SERVICES`. `rc.sysinit` starts `udevd` once, before anything else that needs devices, and `udevadm settle` waits for it.

Services that are not in `SERVICES` and start by other means:

| Thing | Started by | Notes |
|---|---|---|
| flsplash (boot screen) | `rc.sysinit`, then handed to flgreet | |
| flmount `--auto` | `rc.sysinit` | mounts removable drives under `/drives` |
| `flpkg fix-owners` | `rc.sysinit` | ownership, every boot |
| flcomp (desktop) | `desktop.sh` on tty1 | see [11](11-boot-process.md) |
| flsocketd (for CUPS) | the `cups` service | see above |
| bluetoothd | the `bluetooth` service and a udev rule | see above |

## 14.5 Session services (not in rc.conf)

The desktop's own services are started by the session script `/usr/lib/flcomp/session` when a person logs in, and stopped when the session ends (flcomp sends SIGTERM):

| Program | Started | Notes |
|---|---|---|
| D-Bus session bus (`dbus-daemon --session`) | at the start of the session, at the fixed address `$XDG_RUNTIME_DIR/bus` | every desktop program finds the bus |
| `pipewire` | first | the sound server; its socket must exist before the others start |
| `wireplumber` | after pipewire | the sound policy |
| `pipewire-pulse` | after wireplumber | for programs that speak PulseAudio |
| `flupdate-notify` | at login | the update notice (see [13](13-packages-flpkg.md)) |
| `flsnap-notice` | 5 s after login | only on an earlier state (see [11](11-boot-process.md)) |
| Flathub setup | at the first session with network, not on the live system | `flatpak remote-add --user` |
| Autostart entries | 3 s after login | `~/.config/autostart` and `/etc/xdg/autostart` |
| `flpanel` | by flcomp | restarted up to five times |

## 14.6 udev and its FreeLFS rules

`udev` 261.2 comes from the systemd source with `logind=false`, `vconsole=false` (`recipes/udev`). The standard rules are in `/usr/lib/udev/rules.d/`. The hardware database is compiled by the `udev-hwdb` trigger (see [13](13-packages-flpkg.md)). FreeLFS adds these rules:

| File | Installed by | What it does |
|---|---|---|
| `90-flmount.rules` | `src/flmount` | on a partition or disk with a file system appearing, runs `flmount --hotplug`; on removal, `flmount --gone` (mounts and notices, see [12](12-filesystem-layout.md)) |
| `90-flsettings-backlight.rules` | `src/flsettings` | every backlight device: group `seat` may write its `brightness` file |
| `90-flbluetooth.rules` | `recipes/bluez` (from `config/bluez`) | a Bluetooth adapter appears: `rc` service `bluetooth start`; it goes away: `maybe-stop` |
| `71-freelfs-gaming.rules` | `recipes/gaming-config` | game controllers, hidraw nodes of the controller makers, `/dev/uinput`, `/dev/uhid` and `/dev/ntsync` go to group `seat`, mode 0660 |

The rules run after the standard ones. The ownership for `seat` is the reason a person in that group can use the pads and the virtual input devices without root.

## 14.7 Enable, disable, start, stop

Run as root.

| Task | Command |
|---|---|
| See which services are enabled | `grep ^SERVICES /etc/rc.conf` |
| Enable at the next boot | add the name to `SERVICES` in `/etc/rc.conf` |
| Disable at the next boot | put `!` before the name in `SERVICES` |
| Start now | `/etc/rc.d/init.d/NAME start` |
| Stop now | `/etc/rc.d/init.d/NAME stop` |
| Restart | `/etc/rc.d/init.d/NAME restart` |
| Status | `/etc/rc.d/init.d/NAME status` (returns 3 when a daemon is stopped) |
| Status of the network | `/etc/rc.d/init.d/network status` (shows `ip -brief addr`) |

Two rules to know:

- `rc 0` and `rc 6` stop only the **enabled** services (`enabled_services` skips `!names`). A service that is running but disabled is not stopped at shutdown, so stop it by hand first.
- The order in `SERVICES` is the start order. Put a service after the one it needs: `dbus` before `iwd`, `seatd` before the desktop.

## 14.8 Adding a service

A daemon that writes its own pid file is one short script, using `daemon_service` from `functions`:

```sh
#!/bin/bash
# /etc/rc.d/init.d/mydaemon -- one line for start, stop, restart, status
. /etc/rc.d/functions

start_mydaemon() {
	setsid /usr/sbin/mydaemon < /dev/null > /var/log/mydaemon.log 2>&1 &
	echo $! > /run/mydaemon.pid
	sleep 0.2
	pid_alive /run/mydaemon.pid
}

daemon_service "$1" "My daemon" /run/mydaemon.pid start_mydaemon
```

Then add `mydaemon` to `SERVICES`. Make the file executable (mode 755) and, to keep it, add it to `config/etc/rc.d/init.d/` in the source tree so the initscripts package installs it.

## 14.9 Known issues

| Issue | Status |
|---|---|
| Shutdown on real hardware (round 17) | open, round 18; see [11](11-boot-process.md) |
| A running, disabled service is not stopped by `rc 0` | by design (see 14.7); a note for admins |
| The runlevel 5 graphical target is named but not used | by design for now (the desktop starts from tty1) |
| `/etc/rc.conf` `MODULES` is empty by default; built-in drivers cover the boot | by design (`docs/HARDWARE.md`) |
| Log file names of sysklogd | not yet verified |
