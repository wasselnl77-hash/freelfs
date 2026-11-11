# RAM profile of the live session (measured 2026-11-11, nothing optimized yet)

Question: the user saw about 350 MB used at an idle desktop on the laptop with the live ISO. Where does it go?
This file only records what was measured; the trimming is a later job (target in INTEGRATION-TODO: 200-250 MB).

## How

`build/iso-test.py --iso freelfs-live.iso --mem 1G --profile FILE` (QEMU/OVMF, KVM, 2 CPUs, no GPU, user networking).
It boots the live ISO, answers the keyboard question, logs in as `tester` on the screen, waits 15 s, and 60 s
later runs the measuring commands as root over the serial console (so their own cost is in the numbers: one
shell and a `sort`). The ISO was the "twin" of the final one (same root, `console=ttyS0,115200` added to the command line, because the
normal command line has no readable serial console). Raw outputs: `~/freelfs-work/scratch/ram-profile-1G.txt` and `-4G.txt`
(the commands are the `PROFILE` list in build/iso-test.py).

## Headline numbers (idle desktop: flcomp, flpanel, pipewire, wireplumber, flupdate-notify; nothing else started)

| | 1 GB VM | 4 GB VM |
|---|---|---|
| MemTotal | 985 344 kB | 4 010 996 kB |
| **MemTotal - MemAvailable** | **110 MB** (`free`: used 107) | **269 MB** (`free`: used 262) |
| MemTotal - MemFree (page cache counted) | 450 MB | 583 MB |
| Cached (squashfs and libraries read so far) | 383 MB | 495 MB |
| Shmem (tmpfs: the overlay's upper layer) | 35 MB | 147 MB |
| AnonPages | 32 MB | 32 MB |
| Slab | 18 MB | 38 MB |
| sum of all processes' PSS (30 processes) | 83 MB | 83 MB |

The processes are the same size at 1 GB and 4 GB (83 MB PSS). The difference between the two sizes is the live
overlay, see below. Kernel memory beyond slab, stacks and page tables is not visible in `/proc/meminfo`; a real
GPU (amdgpu: firmware, ring buffers, GTT) and the laptop's drivers and Wi-Fi add their own, that cannot be seen in QEMU.

## Per-process PSS, top 15 (1 GB run; kB: Pss, anonymous part, file part, shmem part)

| PSS | anon | file | process |
|---|---|---|---|
| 33 293 | 12 268 | 21 025 | flcomp |
| 11 808 | 3 792 | 8 016 | wireplumber |
| 9 210 | 4 496 | 4 378 | flpanel |
| 5 193 | 2 400 | 2 781 | pipewire |
| 5 125 | 1 908 | 3 217 | udevd |
| 2 593 | 900 | 1 693 | pipewire-pulse |
| 2 523 | 425 | 2 097 | dhcpcd |
| 1 608 | 236 | 1 372 | iwd |
| 1 208 | 351 | 857 | bash (the serial root shell) |
| 932 | 334 | 597 | dhcpcd (second process) |
| 930 | 504 | 426 | session |
| 915 | 488 | 427 | flupdate-notify |
| 651 | 353 | 298 | bash |
| 649 | 276 | 373 | dbus-daemon |
| 580 | 268 | 312 | sort (the measuring command) |

Reading: most of the "file" part is shared library pages (libc, libwayland, libxkbcommon, EGL/GBM and Mesa for flcomp,
which links Mesa for the dmabuf/GL path; not itemised here). flcomp is the single biggest process, wireplumber second
(its Bluetooth, libcamera and logind monitors fail to load in session.log, the scripting engine stays).

## tmpfs and overlay

`/run/live/rom` is the squashfs (loop device, read-only, not counted as used; its pages come back as `Cached`).
`/` is an overlay whose upper layer is the tmpfs `/run/live/rw`; `/tmp`, `/run`, `/dev/shm` are further tmpfs mounts
(each limited to 50 % of RAM, only the part really written counts).

| | 1 GB VM | 4 GB VM |
|---|---|---|
| overlay upper layer (`/run/live/rw`) | 22.6 MB | 134.7 MB |
| ... `usr/tester/.local/share/flatpak/appstream/flathub` | not there yet | **111.6 MB** |
| ... `usr/tester/.local/share/flatpak/repo` (tmp 7.7 MB, objects) | 7.8 MB | 8.2 MB |
| ... `sysflfs/data/lib/flpkg` (the flpkg database, copied up on first write) | **14.2 MB** | 14.2 MB |
| /tmp, /dev/shm | 0 | 0 |
| /run | 0.3 MB | 0.3 MB |

## Findings (to act on later)

1. The biggest single item is not a process: the session adds the Flathub remote at the first login that has a network
   (`session: flathub added` in session.log) and Flatpak pulls the appstream metadata (111 MB unpacked) into
   `~/.local/share/flatpak`, which on the live ISO is RAM. At 1 GB the download had not finished 75 s after the login
   (only the 7.7 MB `repo/tmp` was there), at 4 GB it had. On the laptop with Wi-Fi this will be the same: expect
   +100-150 MB of tmpfs a minute or two after login. Candidates: do not add/refresh Flathub until the Store is opened,
   or keep the appstream cache off RAM (on the live system there is nowhere else but the host disk, which is forbidden).
2. flpkg's database (14 MB) is copied up into the overlay as soon as anything writes it (flupdate-notify's check,
   the first `flpkg` run). A smaller database, or opening it read-only where possible, saves that.
3. Without those two the live desktop is about 110 MB used at 1 GB, of which 83 MB is processes (flcomp 33, wireplumber 12,
   flpanel 9, pipewire 5, udevd 5) and the rest kernel/slab. The user's 350 MB on 7 GB is consistent with: ~110 MB
   base + Flathub appstream 110-150 MB + kernel structures that grow with RAM (slab was 18 MB at 1 GB and 38 MB at 4 GB;
   page tables, struct page are outside MemTotal) + real GPU/Wi-Fi driver memory. The next measurement that would settle it is
   `build/iso-test.py --profile` equivalent on the laptop itself: `flreport` does not yet record PSS or the overlay
   size; adding the two commands (`du -k -d 3 /run/live/rw/up`, the PSS loop) to flreport would let the user send the numbers.
4. At idle there are 5 `dhcpcd` and 5 `agetty` processes; small (3.5 MB PSS together) but easy to count down.

## The installed image at the login screen (m10-features run, 1 GB)

`free -m`: used 107 MB, buff/cache 227 MB, available 854 MB; at the desktop with Files, Store, etc. closed
about the same. FreeBWS with one page: 263-355 MB PSS in total (docs/m7/freebws-*-1G.png, logs/m8-freebws-1G.log).
