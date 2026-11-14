# 18. Performance and RAM: the budget, the measurements, and how to keep it small

This chapter sets the memory budget, shows what was measured and where the memory goes, explains why the system is small, and lists what an admin can switch off.

Status: round 17 (2026-11-14). The numbers come from QEMU runs of the live ISO at 1 GB and 4 GB, as recorded in `docs/RAM-PROFILE.md` and `docs/ROADMAP-NEXT.md`. Nothing has been measured on the laptop with its real GPU and Wi-Fi driver yet.

## Contents

1. [The budget](#181-the-budget)
2. [The measurements](#182-the-measurements)
3. [Where the memory goes](#183-where-the-memory-goes)
4. [Why the system is small](#184-why-the-system-is-small)
5. [Services that start only when needed](#185-services-that-start-only-when-needed)
6. [The live system and RAM](#186-the-live-system-and-ram)
7. [Tuning for your machine](#187-tuning-for-your-machine)
8. [How to measure](#188-how-to-measure)
9. [Known gaps](#189-known-gaps)

## 18.1 The budget

| Target | Value | Source |
|---|---|---|
| Whole desktop on 1 GB of RAM | must work | `docs/PLAN.md`, `docs/ROADMAP-NEXT.md` |
| Idle app memory | apps idle under about 25 MB RSS where possible | `docs/AGENT-RULES.md`, "Low end" |
| Compositor | about 20 MB at 1080p (design figure); 11.6 MB PSS measured at idle | `src/flcomp/README`, `docs/RAM-PROFILE.md` |
| Idle desktop, live ISO, 1 GB | 85 MB used (MemTotal minus MemAvailable) | `docs/ROADMAP-NEXT.md`, round 14 and 15 |
| Browser with one page | 263 to 355 MB PSS in total (FreeBWS, WebKit) | `docs/RAM-PROFILE.md` |

The idle figure leaves room on 1 GB for the browser and a few apps. The browser is the big user: it is WebKit with several processes.

## 18.2 The measurements

All figures are from `build/iso-test.py --mem 1G --profile` (QEMU/OVMF, KVM, two CPUs, no GPU), 75 seconds after login, with `MemTotal - MemAvailable` as the main figure. The "twin" ISO differs from the final one only by a serial console.

| Measurement | Before the first trim | After the first trim (2026-11-11) | Later rounds |
|---|---|---|---|
| MemTotal minus MemAvailable | **110.4 MB** | **84.7 MB** | 84.3 MB (animations, 2026-11-11), 85.4 MB (round 14), 85.8 MB (round 15) |
| `free -m` used | 107 MB | 81 MB | |
| Cached (squashfs and libraries read so far) | 395 MB | 193 MB | |
| Shared memory (tmpfs) | 35 MB | 13 MB | |
| Overlay upper layer (RAM) | 22.6 MB | 0.6 MB | |
| Sum of PSS of all processes | 83.4 MB (30 processes) | 61.6 MB (26 processes) | 61.3 MB |
| flcomp PSS | 33.4 MB | 11.6 MB | 11.5 MB |

The 4 GB run, after the trim: 120.6 MB used, down from 269 MB. The difference between the 1 GB and 4 GB runs is kernel memory that grows with RAM (slab, page tables), not the overlay or the processes. The process total is 61.7 MB PSS at both sizes.

The idle desktop after the animation work (round 11) took no extra resident memory: the picture of a closing window exists for at most about 200 ms and is then freed (`docs/RAM-PROFILE.md`, "After animations and Themes").

## 18.3 Where the memory goes

Largest processes at idle on the 1 GB run, after the first trim (PSS in kilobytes, from `docs/RAM-PROFILE.md`):

| PSS | Process | Note |
|---|---|---|
| 11 854 | wireplumber | the scripting engine (Lua) and its shared libraries; the next candidate for trimming |
| 11 565 | flcomp | the compositor; its libraries are mostly shared pages |
| 9 955 | flpanel | the panel; a 4 MB heap |
| 5 203 | udevd | device manager |
| 5 083 | pipewire | sound server |
| 2 607 | pipewire-pulse | PulseAudio layer |
| 2 434 and 981 | dhcpcd (two processes) | privilege separation |
| 1 604 | iwd | Wi-Fi |

Before the trim, flcomp was 33 MB because it linked EGL, GBM and GLES, and those pull in libgallium, LLVM and SPIRV-Tools, all mapped in every session. Now it loads them only when a client needs a GPU buffer (`src/flcomp/README`).

Kernel memory: slab about 17 MB at 1 GB, page tables about 2 MB.

## 18.4 Why the system is small

- **No systemd.** The init is sysvinit with shell scripts (see [14](14-services-init.md)). udev is built without logind.
- **Own compositor on the CPU.** flcomp composites in software and needs no GPU driver for shared-memory clients. It does not link Mesa or EGL for them.
- **Lazy GPU path.** A GPU client makes flcomp load EGL through `dlopen`, and only then.
- **Own toolkit (libflui).** The panel, Settings, the apps and the installer use one small C library, not GTK or Qt.
- **Read-only system.** The live system's squashfs is read-only and mostly in the page cache. The flpkg database stays in the squashfs when nothing is pending (`flpkg fix-owners` opens it read-only).
- **No Flathub on the live system.** Its appstream data is about 110 MB unpacked (see 18.6).
- **Few gettys on the live system.** tty3 to tty6 get no login prompt there (four `agetty` processes fewer).
- **Animations do not keep memory.** Their pictures are freed after about 200 ms, and the animations can be reduced or switched off (18.7).
- **Services start on demand** where it matters (18.5).

Tried and not kept: `MALLOC_ARENA_MAX=2` for the session daemons (no measurable change).

## 18.5 Services that start only when needed

| Service | When it runs | Memory when idle |
|---|---|---|
| bluetoothd | only while a Bluetooth adapter exists (rc service `bluetooth`, udev rule) | 0 without an adapter |
| CUPS (cupsd and avahi) | only on the first print job or printer query; stops after 60 s without a job | flsocketd only (about 0.2 MB) |
| Flathub catalogue | on request on the live system ("Try one small app anyway"); on installed systems the appstream data is fetched after login with network | 0 on the live system until asked |
| Flatpak apps | when the person starts them | their own size |
| flsnap-notice | five seconds after login, exits at once on a normal boot | 0 |
| Autostart entries | as listed in `~/.config/autostart` | their own size |
| flsocketd | always (the CUPS socket) | 0.2 MB |

## 18.6 The live system and RAM

The live ISO runs in RAM, so its disk is memory. Its root is an overlay with a tmpfs upper layer (`/run/live/rw`). `/tmp`, `/run` and `/dev/shm` are tmpfs mounts, each limited to half of RAM (only the part written counts).

| Item (1 GB run) | Size |
|---|---|
| Overlay upper layer, before the trim | 22.6 MB |
| Overlay upper layer, after the trim | 0.6 MB |
| Flathub appstream data (4 GB run, before the trim, after login with network) | 111.6 MB |
| Flatpak repo (temporary files and objects) | 7.8 MB |
| flpkg database (copied up on first write, before the trim) | 14.2 MB |

What was changed for the live system (`docs/RAM-PROFILE.md`):

1. flcomp loads EGL and GBM only on demand.
2. `flpkg fix-owners --keep` leaves the database in the squashfs on the live system.
3. The session adds no Flathub remote on the live system; the Store offers the installer, and "Try one small app anyway" loads the catalogue on request, showing the room left first.
4. tty3 to tty6 get no login prompt.

The Store's Explore page says that an app can be installed after installing FreeLFS. A Flatpak app can take 1 to 2 GB on the live system, and the RAM for it is the live overlay's, which is at most half of RAM. A second big app fails with "Not enough disk space" (`docs/INTEGRATION-TODO.md`).

Persistence (`flpersist`, see [12](12-filesystem-layout.md)) moves the home folder to a file on the stick. The root overlay stays in RAM, so persistence does not reduce the memory used by the system.

## 18.7 Tuning for your machine

These are the settings that change the memory and CPU use. Edit as root where it says so; the desktop settings are per person.

| Setting | Where | Effect |
|---|---|---|
| Services off | `SERVICES` in `/etc/rc.conf`, prefix `!` (for example `!bluetooth`, `!cups`) | fewer daemons; Bluetooth and printing stop working. See [14](14-services-init.md). Note: a running disabled service is not stopped at shutdown |
| Animations | Settings > General > Themes, or `animations=on\|reduced\|off` and `animation_speed=` in `~/.config/freelfs/desktop.conf` | `off` and `reduced` cost less CPU; no resident memory either way |
| Power saving | `power_saver=1` in `desktop.conf`, or the battery flyout | at most 30 frames per second and a dimmer screen |
| Performance mode | `performance=auto\|on\|off` | `on` removes the frame cap; `auto` turns it on while a fullscreen window is on top |
| Browser | FreeBWS: fewer open tabs | each web process costs memory; the sandbox is on by default (see [15](15-users-security.md)) |
| Swap | installer Manual mode, or by hand | the installer adds a swap line only for a swap partition made in Manual mode; none is created by default. Not measured here |
| Lid lock | `lock_on_lid` in `desktop.conf` | no memory effect |

Commands that show the state:

```sh
free -m                                        # used, buff/cache, available
grep -E '^(MemTotal|MemAvailable|Cached|Shmem):' /proc/meminfo
grep VmRSS /proc/$(pidof flcomp)/status        # resident memory of the compositor
flsnap status                                  # which snapshot is booted, if any
```

## 18.8 How to measure

- **`flreport`** writes `memory.txt` into its tarball: used memory, the PSS totals of the processes the person may read, the size of the overlay's upper layer and its biggest folders on the live system. It is the figure to send when a machine uses more memory than expected (`src/flreport/README`, `docs/RAM-PROFILE.md`).
- **`free -m`** for the total picture.
- **PSS** (proportional set size) counts shared pages fairly. The project measures it with `tests/pss.sh` in `src/flcomp` and with the `--profile` option of `build/iso-test.py`. Per process, `/proc/PID/smaps_rollup` has the `Pss:` line.
- **RSS** (`VmRSS` in `/proc/PID/status`) counts shared pages in full, so it overstates the total. Use it for one process only.

## 18.9 Known gaps

| Gap | Status |
|---|---|
| The laptop's own numbers (real GPU, real Wi-Fi driver) | not measured; the real GPU adds driver memory that QEMU cannot show (`docs/RAM-PROFILE.md`) |
| The user's report of about 350 MB used on the live ISO on a 7 GB machine | partly explained (the Flathub data, the kernel's memory on larger RAM); not confirmed on the machine |
| Next candidates for trimming | wireplumber's scripting engine and libraries (about 8 MB of shared pages), flpanel's heap, the five dhcpcd processes, udevd. Not done |
| Browser memory with several tabs | not measured in this round |
| Memory on installed systems (not the live ISO) | measured on the live ISO; an installed system adds its own services (not measured) |
| The Intel and AMD driver memory | not measured |
