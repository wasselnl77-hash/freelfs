# 16. Networking: Wi-Fi, wired, DNS and the Settings Wi-Fi page

This chapter describes how FreeLFS connects to networks: iwd for Wi-Fi, dhcpcd for wired connections, the DNS file, the time service, and the Wi-Fi page in Settings that talks to iwd.

Status: written from `config/iwd`, `config/etc`, `recipes/iwd`, `recipes/dhcpcd`, and `src/flsettings` at round 17 (2026-11-14). Nothing here has been run on a real Wi-Fi card in this round. The laptop's Wi-Fi (Realtek rtw89) was tested in earlier rounds only (see [17](17-hardware-drivers.md)).

## Contents

1. [The stack at a glance](#161-the-stack-at-a-glance)
2. [Wi-Fi: iwd](#162-wi-fi-iwd)
3. [Wired: dhcpcd and the network service](#163-wired-dhcpcd-and-the-network-service)
4. [DNS](#164-dns)
5. [Time](#165-time)
6. [The Wi-Fi page in Settings](#166-the-wi-fi-page-in-settings)
7. [Command-line tools](#167-command-line-tools)
8. [Troubleshooting and logs](#168-troubleshooting-and-logs)
9. [What is not there](#169-what-is-not-there)
10. [Known issues](#1610-known-issues)

## 16.1 The stack at a glance

| Job | Program | Version (recipe) | Started by |
|---|---|---|---|
| Wi-Fi: scan, connect, keys, its own DHCP client on Wi-Fi interfaces | **iwd** (`/usr/libexec/iwd`), with `iwctl` | 3.12 r2 | rc service `iwd` |
| Wired DHCP | **dhcpcd** 10.5.2 r2 | 10.5.2 r2 | rc service `network` |
| Name resolution from Wi-Fi DHCP | a small `resolvconf` stand-in in `/usr/lib/iwd/bin` | in iwd's package | iwd, through its PATH |
| Wi-Fi regulatory rules | `wireless-regdb` | recipe `wireless-regdb` | installed as the regulatory database; read by the kernel (not verified here) |
| Clock | `fl-sntp` (fltime) | in `src/fltime` | rc service `ntp` |
| Interface and address tools | `ip`, `ss` from iproute2 | 7.1.0 | by hand |
| Printer discovery | avahi-daemon (only with printing on) | in `recipes/avahi` | flsocketd (see [14](14-services-init.md)) |

Wi-Fi and wired interfaces are kept apart on purpose. `/etc/dhcpcd.conf` denies `wl*` to dhcpcd (`recipes/dhcpcd`: "two DHCP clients on one interface fight over its address and routes"), and iwd's own DHCP client takes the Wi-Fi interfaces (`EnableNetworkConfiguration=true` in `/etc/iwd/main.conf`).

## 16.2 Wi-Fi: iwd

### Configuration

`/etc/iwd/main.conf` (`config/iwd/main.conf`):

```ini
[General]
EnableNetworkConfiguration=true   # iwd's own DHCP client on Wi-Fi interfaces

[Network]
NameResolvingService=resolvconf   # no systemd-resolved: iwd calls resolvconf (see 16.4)
```

Other iwd settings are the defaults of iwd 3.12. The file is replaced by the package on upgrade, so keep own changes in a separate file or track them after each upgrade (`flpkg` replaces `/etc` files, see [13](13-packages-flpkg.md)).

### Start and state

- rc service `iwd` (`config/etc/rc.d/init.d/iwd`) starts `/usr/libexec/iwd` with `/usr/lib/iwd/bin` first in the PATH. Its output goes to `/var/log/iwd.log`. Its pid is `/run/iwd.pid`. It needs the D-Bus system bus, so `dbus` must come before it in `SERVICES` (see [14](14-services-init.md)).
- Known networks are saved in `/var/lib/iwd` as `NAME.psk` (WPA passphrase), `NAME.open` or `NAME.8021x`. The installer copies them to the new system when the Wi-Fi step is used (`src/flinstall/README`).
- The D-Bus policy for iwd (`config/iwd/seat-dbus.conf`, installed to `/usr/share/dbus-1/system.d/iwd-seat.conf`) lets **every person in group `seat`** send messages to `net.connman.iwd`. That is how the Settings Wi-Fi page works without a password. Root and `wheel` are allowed too, by iwd's default policy (the comment in the file).

### Commands

`iwctl` (in the iwd package) is the command-line client. Its commands are iwd's own and are not described here (see `iwctl --help`; not verified against this build in this round). The Settings Wi-Fi page is the supported way.

## 16.3 Wired: dhcpcd and the network service

- rc service `network` (`config/etc/rc.d/init.d/network`) runs `dhcpcd -q -b`. With `NETWORK` empty in `/etc/rc.conf` it does this on **every** interface except the denied `wl*` ones.
- To configure interfaces by hand, list them in `NETWORK` in `/etc/rc.conf`, one entry per interface:

```sh
NETWORK=(
	"eth0 dhcp"
	"enp3s0 static 192.168.1.10/24 192.168.1.1"
)
```

  `dhcp` runs DHCP for the interface. `static ADDRESS/PREFIX GATEWAY` sets the address with `ip addr add` and the default route with `ip route add default via GATEWAY`. The gateway may be left empty.
- `/etc/rc.d/init.d/network stop` releases the DHCP leases (`dhcpcd -x`, 8 s timeout) and takes the addresses down. `status` shows `ip -brief addr`.
- dhcpcd runs with privilege separation under its own user `dhcpcd` (uid 52), and its state is in `/var/lib/dhcpcd` (`config/ownership.list`). The privilege-separation processes account for the extra `dhcpcd` lines in the process list (see [18](18-performance-ram.md)).
- The kernel's built-in Ethernet drivers are listed in [17](17-hardware-drivers.md) (e1000e, igb, igc, r8169, r8152, CDC tethering).

## 16.4 DNS

Two sources write `/etc/resolv.conf`:

- **dhcpcd** (wired) writes the servers from its DHCP lease, its default behaviour.
- **iwd** (Wi-Fi) calls `resolvconf -a IFACE.dns` and `-d`. FreeLFS has no `resolvconf` package. Its stand-in (`config/iwd/resolvconf`, installed to `/usr/lib/iwd/bin/resolvconf`) keeps one file per interface in `/run/iwd-resolv/` and writes `/etc/resolv.conf` from all of them. The first line is `# generated by iwd (FreeLFS resolvconf helper)`.
- The stand-in removes `/etc/resolv.conf` when no interface has servers left, **but only if the file was written by iwd** (it checks that first line). A file that dhcpcd wrote is not touched.

The stand-in is found only through iwd's PATH, so dhcpcd never uses it.

## 16.5 Time

The `ntp` service (`config/etc/rc.d/init.d/ntp`) runs `fl-sntp`, which keeps asking the time servers until the network is up. Its settings are in `/etc/fltime.conf`:

| Setting | Meaning |
|---|---|
| `NTP=0` | off; the service does nothing |
| `SERVERS="a b"` | the servers to ask; the default is `pool.ntp.org` |

Settings > Date & time sets these through `fltime-set ntp on|off` and `fltime-set sync` (setuid, group `seat`; see [15](15-users-security.md)). With NTP off, the time can be set by hand (`fltime-set time EPOCH`). The clock is saved to the hardware clock at shutdown (see [11](11-boot-process.md)).

Verified in QEMU on 2026-11-10 (`build/m9-features.sh`, feature A10 in `docs/ROADMAP-NEXT.md`): `fl-sntp` reached pool.ntp.org through QEMU's user networking; a clock set 2 hours off was corrected within seconds after `ntp on`.

## 16.6 The Wi-Fi page in Settings

The page (`src/flsettings/wifi.c`, opened by `flsettings --page wifi`) is a client of iwd's D-Bus API. Its design, from its header:

- One private connection to the **system** bus, polled by the toolkit's event loop, so an idle page costs nothing.
- It keeps a snapshot of iwd's objects (`GetManagedObjects`, then `GetOrderedNetworks` for the signal strengths). Every signal from iwd schedules a new snapshot.
- Passwords: iwd asks for them through `net.connman.iwd.Agent`, which the page exports at `/org/freelfs/flsettings/agent`. The typed password is kept in one buffer, wiped after it is sent, cancelled, or the prompt goes away. It is never logged or written to disk. (libdbus makes its own copy while sending, and does not wipe that copy.)

What the page shows and does:

| Item | Behaviour |
|---|---|
| Network list | names with signal bars, a lock for secured networks |
| Scan | asks iwd to scan |
| Connect | connects; asks for the passphrase through the agent when there is none saved |
| Disconnect | disconnects the current network |
| Forget | removes the saved network |
| Wi-Fi switch | turns the Wi-Fi device on or off |
| Wired status | one line about the wired interfaces |
| Errors | "Without D-Bus, iwd, permission or hardware the page says so" (`src/flsettings/README`) |

The person needs access to iwd on the bus. Group `seat` has it through `iwd-seat.conf` (see 16.2).

## 16.7 Command-line tools

| Tool | Use | Source |
|---|---|---|
| `iwctl` | iwd's client: devices, scanning, connecting from a console | iwd package |
| `ip` | addresses, routes, links (`ip -brief addr`, `ip route`) | iproute2 |
| `ss` | sockets | iproute2 |
| `dhcpcd` | manual DHCP; `dhcpcd -q -b IFACE` starts it, `dhcpcd -q -x IFACE` stops it (the way the network script does it) | dhcpcd |
| `/etc/rc.d/init.d/network status` | the addresses, short form | FreeLFS script |
| `fl-sntp -1 -q SERVER` | one-off time query (`-q` checks a server) | `src/fltime` |
| `flreport` | a report with the Wi-Fi log, the network names, addresses and the configuration | [19](19-admin-reference.md) |

The `iwctl` and `dhcpcd` options listed above are the upstream ones and were not re-checked against these builds in this round.

## 16.8 Troubleshooting and logs

| Symptom | Where to look |
|---|---|
| Wi-Fi does not appear | `dmesg` (driver and firmware: see [17](17-hardware-drivers.md)); `ls /sys/class/net` for a `wl*` interface |
| Wi-Fi connects but no address | `/var/log/iwd.log` (the DHCP lines); the iwd service status |
| Address but no DNS | `/etc/resolv.conf` (whose first line says who wrote it); `/run/iwd-resolv/` |
| Wired gets no address | `/etc/rc.conf` `NETWORK`; `/etc/rc.d/init.d/network status`; the dhcpcd output in the terminal |
| Settings Wi-Fi page says "no D-Bus" or "no permission" | `dbus` service running; the user is in group `seat` |
| Clock is wrong | `/etc/fltime.conf`; `/etc/rc.d/init.d/ntp status`; `fltime-set sync` |

`flreport` (in the Settings > About page too) collects `/var/log/iwd.log`, the network status and the kernel messages in one file that can be copied to a stick (`src/flreport/README`).

## 16.9 What is not there

No NetworkManager, no connman, no systemd-networkd or systemd-resolved, no wpa_supplicant, no VPN client (no WireGuard or OpenVPN recipe). The only regulatory data is `wireless-regdb`. A VPN is not supported by the tree yet. An IPv6 setting was not checked in this round.

## 16.10 Known issues

| Issue | Status |
|---|---|
| Wi-Fi on the real laptop (Realtek rtw89) | tested in earlier rounds; not re-run after the round 15 kernel change (`docs/HARDWARE.md`) |
| Intel Wi-Fi (iwlwifi) and MediaTek (mt79xx) on real hardware | not yet verified (`docs/HARDWARE.md`) |
| The Settings Wi-Fi page on real hardware | not yet verified |
| Known networks copied by the installer | not yet verified on a real disk |
| No VPN, no GUI for wired static addresses (the `NETWORK` entries are edited by hand) | open |
| iwd and dhcpcd both handle DHCP for their own interfaces | by design (16.1) |
