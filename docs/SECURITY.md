# FreeLFS security notes

What is protected today, how the signing key is kept, how to rotate it, how known
vulnerabilities are tracked, the browser sandbox, and what Secure Boot would take.
Written 2026-11-11, updated 2026-11-12 (security update round), 2026-11-18 (password policy for the setuid helpers). Nothing here is audited by anyone but the authors.

Short list of what exists:

| Area | State |
|---|---|
| Package repo | minisign-format Ed25519 signatures on every package and on the index; the index carries a serial and an expiry (`valid-until`) and flpkg refuses expired or older ones |
| Signing key | one key, offline custody rules below; a rotation mechanism with a transition period |
| Release model | rolling: updates follow fixed upstream releases (section 0) |
| CVE tracking | `build/cve-check.py` + `build/cve-names.conf`, report in `docs/CVE-REPORT.md`; kernel triage against the config: `build/cve-kernel.py`, `docs/CVE-KERNEL.md` |
| Browser | WebKit's bubblewrap sandbox for the web processes (section 3; WPE WebKit 2.54.2) |
| Local root actions | setuid helpers in group `seat`; a password per destructive action, purpose-bound two-minute stamps, one-shot for disk writes and snapshot rollback/delete (section 5) |
| Boot | not signed: Secure Boot must be off |

---

## 0. Rolling release

FreeLFS is a **rolling release**. There are no numbered versions to move between, and nothing is backported: a
vulnerability is fixed by taking the **newest stable upstream release of the series that fixes it** (a point release in
the same series when one exists, the next stable series when the old one gets no more fixes: the WPE WebKit 2.52 series,
for example, stopped receiving the fixes of the advisory of 29 September 2026, so the browser engine moved to 2.54).
Sources are pinned by hash in `sources.lock`, verified against the upstream signature or published checksum when the
update is made, and the recipe `version` and `release` are bumped.

* **Kernel.** Package `linux` is the newest stable kernel from kernel.org (7.3.x in Nov 2026). The config is
  `config/kernel/freelfs.config`, carried to every new kernel with `make olddefconfig`; `recipes/linux` refuses to build
  when a setting of the fragment is silently dropped (a renamed or removed symbol), so a kernel update cannot lose the
  gamepad, uinput/uhid/ntsync, btrfs, rtw89, amdgpu or live-ISO settings unnoticed. Package `linux-lts` is the newest
  6.18.y built from the same fragment with the local version `-freelfs-lts`; it is **optional** (not in the live ISO or the
  default image: `flpkg install linux-lts`), and flboot lists it after the other kernels as "FreeLFS (LTS kernel ...)".
  It is the fallback for hardware a new kernel breaks.
* **Safety net.** `flpkg` takes a btrfs snapshot before every transaction (src/flsnap); flboot's "Start an earlier
  state" starts any of them read-only, and `flsnap rollback` makes it the current state. Kernels live on the ESP and
  are not part of a snapshot: an earlier state started by flboot uses the newest kernel on the ESP with the modules of
  the snapshot. If the snapshot predates the kernel's modules (a kernel update lies between), the boot has no loadable
  modules (the live-path file systems, disks, USB and simpledrm are built in, so it comes up with a plain framebuffer;
  GPU, Wi-Fi and sound modules are missing). Then pick the LTS entry only if it was installed at that time, or boot the
  state with a matching kernel: `docs/INTEGRATION-TODO.md` records this as open.
* **What a rolling release does not give you:** no staged testing between upstream and the repository (a QEMU boot and
  the test suites of this tree, not a distribution's testing period) and no backports. Check `build/cve-check.py` before
  every publication.

---

## 1. Repository signing

`build/70-repo.sh` signs packages and the index with the repo key; clients trust the
`*.pub` files in `/etc/flpkg/keys` (shipped by the flpkg package) plus keys they learned
from a rotation (below). Details of the repo format: `docs/flpkg.md`.

### Index expiry and replay protection

The signed comment of `index.zst.sig` carries two fields next to the file name:

    serial:1789912345    valid-until:2026-11-25T12:00:00Z

They sit in minisign's *trusted comment*, which the global signature covers: they
cannot be edited, and the signature cannot be moved to a different index.

* **valid-until** is set by `70-repo.sh` (`FLFS_REPO_VALID_DAYS`, default 14 days).
  `flpkg sync` and `flpkg check-updates` refuse an expired index:
  `repo freelfs: the signed index EXPIRED on ... Nothing was changed. The repository owner
  must re-sign and republish it (build/70-repo.sh) ... If the repository is current and only
  this machine's clock is wrong, set the date and run 'flpkg sync' again.`
  Without expiry a mirror (or anyone on the path) can keep serving an old, validly signed
  index forever, and the machine never learns that security updates exist.
* **serial** never goes down: `flpkg repo add` makes it `max(previous serial + 1, unix time)`,
  so even a repo directory that was lost and rebuilt keeps counting upward. A client keeps
  the last serial it accepted in `/var/lib/flpkg/index-serial.REPO` and refuses a lower one:
  `ROLLBACK refused: the index has serial N but this machine already accepted serial M ...`
  An old index that has not expired yet (a replay) is stopped by this. Same serial again is
  fine. `check-updates` reads the serial but, having no root, never writes it. If the owner
  really must go back to an older repository state, they publish it with `--serial` above the
  last one issued (the error message says so).
* A cached index that expired since the last sync only produces a warning in other commands
  (`search`, `install` from cache), so a machine without network can still install from its
  cache; the warning says to run `flpkg sync`.
* An index without these fields (made by an old flpkg) is refused when its signature is
  checked: `flpkg repo add` with the current flpkg makes new ones. (No repo is published yet,
  so nobody is locked out by this.)

**Operational consequence:** the repo has to be re-signed before `valid-until`: run
`build/70-repo.sh` at least every 10 days even when no package changed (it re-signs the index
every time). The script prints the date to sign again before. If the key is kept offline
(below) this means plugging it in every ten days; with a slower rhythm use
`FLFS_REPO_VALID_DAYS=30` and accept that a frozen mirror is noticed after up to 30 days.

### Key custody

Where it is: `~/freelfs-work/repo-key/freelfs.key` (secret) and `freelfs.pub`, made by
the first run of `70-repo.sh` (`flpkg key generate`). The directory is 0700, the key file
0600, **outside the source tree** (the script refuses a path inside it), and
`build/backup.sh` leaves it out on purpose. At the time of writing the key has not been
generated yet (no repo has been built): make it on the machine and in the medium described here.
Only the public half is committed (`config/flpkg/keys/freelfs-repo.pub`). The secret key is
never printed, copied into the repo directory, the image or logs (flpkg only reads it to sign;
`flpkg key introduce` writes only public material and a signature).

Rules:

1. **Offline.** The key does not live on a machine that browses the web or runs builds
   of untrusted code. Keep it on an encrypted removable medium (a LUKS-formatted stick) and
   mount it only for a signing run. A signing run is `70-repo.sh` with
   `FLFS_REPO_KEY=/path/on/the/stick/freelfs`; packages are built elsewhere, the stick is only
   needed for the last step (copy `.bootstrap/pkg/out` over, or run the script on the signing
   machine).
2. **Passphrase.** flpkg's key file is *not encrypted* (`flpkg` cannot read minisign's
   scrypt-encrypted keys yet; implementing that is a small job if wanted). So the protection is
   the container: a LUKS volume (or a `gpg --symmetric` / `age -p` encrypted copy that is
   unpacked into `$XDG_RUNTIME_DIR`, a tmpfs, for the run and deleted after) with a passphrase
   of at least six random diceware words or 20 random characters that is not used anywhere else
   and not stored on the same machine. Never put the passphrase in a shell history, env file or script.
3. **Backup.** Two encrypted copies on different media in different places (an offline
   stick at home, one elsewhere), made right after key generation, plus the public key and
   its key id (`flpkg key list`, 16 hex digits) written down. Losing the key means every installed
   system has to get a new public key from a new image: a rotation cannot be started without the old
   key. Check once a quarter that a backup still decrypts and still signs (sign a scratch repo).
4. **Publish the key id** on the project page and in the release notes, so people can compare it
   with `flpkg key list` on their machine.
5. **Compromise or loss.** A rotation statement is signed by the *old* key, so it helps with
   planned rotation, not with a stolen key (the thief can sign statements too). If the key may have
   leaked: stop publishing, announce on every channel, publish a new image with a new public key and
   tell people to replace `/etc/flpkg/keys/freelfs-repo.pub` by hand (the repo URL changes too, or
   the old key's thief could still serve their own index to old clients).

### Key rotation

flpkg trusts a small set of keys: all `*.pub` in the keys directory, plus the keys it learned,
in `/var/lib/flpkg/keys/`. A new key is introduced by the old one:

    flpkg key introduce --key OLD.key --new NEW.pub [--retire-days N] DIR

This writes `DIR/rotate.pub` (the new public key) and `DIR/rotate.pub.sig`, signed by the old key.
The signed comment says `rotate-from:OLDID rotate-to:NEWID retire-old:DATE`. Put both files next to
`index.zst` (the script does it). On every `flpkg sync`/`check-updates` the client fetches
`rotate.pub` if there is one, **before** it checks the index, and if the statement verifies
against a key it trusts now:

* it trusts the new key too, from that moment (`/var/lib/flpkg/keys/NEWID.pub`);
* it records that the old key stops being trusted on `DATE` (`keys/retire.list`). After that date
  the old key is gone for indexes and packages, even if its `.pub` file is still in
  `/etc/flpkg/keys`. A statement that does not verify is ignored with a warning; one for a key
  the machine already has changes nothing except the retirement date (images that ship both keys still
  retire the old one).

`flpkg key list` shows what is trusted and when each key retires. `check-updates` uses a
statement for its own run but saves nothing (no root).

Procedure (every number is a minimum, not a deadline to hit exactly):

1. **Day 0.** `flpkg key generate ~/freelfs-work/repo-key/freelfs-2027` (on the signing medium).
   `FLFS_REPO_INTRODUCE=~/freelfs-work/repo-key/freelfs-2027 FLFS_REPO_RETIRE_DAYS=90 build/70-repo.sh`:
   the repo is still signed by the *old* key and now carries `rotate.pub(.sig)`; the new public key
   goes to `config/flpkg/keys/freelfs-2027.pub`, so the next image ships both. Publish.
2. **Transition** (at least as long as clients take to sync, say 30 days; the retirement date is 90 days
   out by default). Every machine that syncs learns the new key. Rebuild flpkg and the image with both
   keys. Keep re-signing with the old key.
3. **Switch.** `FLFS_REPO_KEY=~/freelfs-work/repo-key/freelfs-2027 build/70-repo.sh`: index and packages
   signed by the new key. Machines that learned it accept it; a machine that only has the old key
   learns the new key from the unchanged `rotate.pub` in the repo first, then accepts the index.
   Leave `rotate.pub` in the repo until the retirement date.
4. **After the retirement date.** Remove the old `.pub` from `config/flpkg/keys`, rebuild flpkg. Machines
   that never synced during the whole transition (old images) no longer verify anything; they need
   a new image or the new key by hand. Destroy the old secret key after the backups of it are
   retired too.

What it does not do: it cannot recover from a *compromised* old key (see rule 5 above), and it follows one
pending rotation at a time (a machine that missed rotation A to B and sees B to C has no way to
verify the second statement: keep a transition at least as long as the longest gap you want to support).

Tests: `src/flpkg/tests/test-trust.sh` (expired index, replay, good index, a rotation with transition,
a forged or tampered statement, retirement).

---

## 2. Known vulnerabilities (CVE tracking)

`build/cve-check.py` reads `recipes/*/recipe` (name, version) and asks public sources, with no
token: **OSV.dev** `querybatch` for PyPI packages, and the **NVD** CPE matching (the recipe version as
a virtual CPE, NVD does the range matching) for the rest, because OSV does not index C libraries by upstream name.
(OSV's Linux ecosystem returned nothing for kernel versions in this test, so the kernel is checked through the NVD too.)
Recipe names are not always upstream names, so `build/cve-names.conf` maps them (`cpe=vendor:product`, `pypi=Name`,
`strip=` for odd version strings, `skip=`/`own` for what is not checked). A recipe that is not
listed is reported as "no entry", never as clean; a CPE the NVD does not know is reported
as such instead of "0 findings" (the NVD only has CPEs for products that had a CVE, so for about 60 small
libraries and tools this means "none ever filed" or "wrong name": the list is in the report, and the
guesses that turned out wrong were fixed with `--check-map`). Entries where the product is only the platform
something else runs on (`vulnerable:false`) or where no version range contains the recipe version are dropped.

    build/cve-check.py                           # report, worst first
    build/cve-check.py --md docs/CVE-REPORT.md   # also as markdown
    build/cve-check.py --only curl openssl       # a few recipes
    build/cve-check.py --check-map               # are the CPE names in the map real?
    build/cve-check.py --offline                 # cache only

**Triage rules** (added in the security round of 2026-11-12; the report counts what is left and lists what it set aside):

* `ignore RECIPE ID reason` lines in `build/cve-names.conf` set aside one finding that was read and does not apply
  (ruby's GitLab entry that NVD files under the ruby CPE by mistake; ffmpeg's RTSP, DASH, SFTP and mbedtls entries, because
  ffmpeg is built with `--disable-network --disable-autodetect` and `strings` on `libavformat` shows none of those; the
  Windows-only Python advisories; glibc's deprecated resolver functions that only `libresolv` defines).
* The `buildtool` token (bison, flex, binutils, gcc, autoconf, cmake, ...): such findings are about hostile *source
  files given to a tool the builder runs*, not about the running system. They are listed under "triaged", not counted.
* The `optional` token (`linux-lts`): the package is not in the default image or the live ISO; its candidates are counted apart.
* A "fixed in" bound that is a date (`2025-01-13`: an ffmpeg git-master or gcc commit date) is a development snapshot, not a
  release, and cannot match a release number: dropped by the script itself.
* Kernel: `build/cve-kernel.py` follows the fix commit of every candidate to the files it changes, walks the Makefiles of the
  new kernel tree and looks every Kconfig symbol up in the build config: result in `docs/CVE-KERNEL.md`.
* NVD's data for WebKit lags: it listed no WPE finding for 2.52.5, while the WPE advisory of 29 September 2026
  (WSA-2026-0006, about 230 CVEs) names the 2.52 series as affected. For the browser engine the advisories at
  wpewebkit.org/security are the source, not `cve-check.py`.

Results are cached (12 h; `$FLFS_CACHE/cve`) because the NVD allows 5 requests in 30 s without a key; a full
run takes about an hour the first time (minutes once cached). Read a finding before acting on it: a match on the version is a candidate
(NVD entries without a version range match everything and are marked `?`; the kernel list includes
drivers this config leaves out; distros backport fixes, FreeLFS does not, so the fix is a newer upstream
version). Latest result: `docs/CVE-REPORT.md`.

### Proposed update cadence

| What | When | Why |
|---|---|---|
| Run `cve-check.py`, read the diff to the last report, commit `docs/CVE-REPORT.md` | every Monday (10 minutes), and before every release | the data changes daily; the run is cheap once cached |
| Critical or high, in a package that touches untrusted data (wpewebkit, curl, openssl, libxml2/libxslt, libpng, libjpeg-turbo, libwebp, freetype, harfbuzz, ffmpeg, gstreamer + plugins, glib/libsoup/glib-networking, sqlite, expat, zlib, xz, brotli, nghttp2, libarchive, mupdf, mesa, pipewire, flatpak/bubblewrap/ostree) | rebuild and publish within **7 days** of the upstream release | these are reachable from a web page, a download or a file the user opens |
| WebKit: every WPE stable release (they follow the WebKitGTK/WPE security advisories, roughly every 3 to 4 weeks) | within **14 days**, or 7 for an exploited-in-the-wild advisory | the browser is the largest attack surface; wpewebkit takes hours to build, so plan the machine time |
| Kernel: the newest stable series (`linux`); the 6.18 LTS (`linux-lts`) when it is installed | monthly, and within 14 days for a high one that applies to the config (`build/cve-kernel.py` says which do) | most kernel CVEs concern drivers FreeLFS does not build (75 of 132 in the first triage); the rest is fixed by the next stable point release |
| Everything else in the image (base system, desktop libraries) | one monthly batch rebuild | lower exposure |
| Build-only tools (autoconf, cmake, meson, ...) | at the next regular rebuild | not in the attack path of a running system |
| `flpkg repo` re-sign | at least every 10 days (index valid 14) | see "Index expiry" |

Process: `cve-check.py` → pick the packages by the table → bump `version` and `release` in the recipe
and the URL in `sources.list`, `build/update-lock.sh` → the rootfs owner rebuilds (60-packages.sh) → `70-repo.sh` →
publish. Sources to read besides the report: kernel.org stable announcements, oss-security, the
WPE security advisories (wpewebkit.org/security), the curl and OpenSSL advisories. The tool is for triage, not a
guarantee that nothing is missing: CVEs without a CPE, and bugs nobody reported, are not in it.

---

## 3. FreeBWS web process sandbox

WPE WebKit has a bubblewrap sandbox for its web processes, switched on at build time with
`ENABLE_BUBBLEWRAP_SANDBOX` (needs libseccomp, `bwrap` and `xdg-dbus-proxy`; recipes `libseccomp`,
`bubblewrap` and `xdg-dbus-proxy` exist and are in the image). Until the 2.52.5-1 build it was **off**:
the web processes, the part of a browser that runs hostile JavaScript and image/font/video parsers, ran with the
user's full rights. Since the security round of 2026-11-12 it is **on** (WPE WebKit 2.54.2, `freebws` release 13: the probe fix, and an explicit download policy because the 2.54 default ended a download navigation without starting a download).

How it works (WebKit's own design, `BubblewrapLauncher.cpp`):

* In the WPE 2.0 API the sandbox is enabled by default in the web context.
  `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` turns it off.
* Each *web* process is started as `bwrap --args ... -- WPEWebProcess`: new user, mount, pid and uts namespaces (the IPC namespace is *not* unshared for a WPE web process: WebKit does that only for GTK/Wayland; measured in QEMU: `ns ipc SAME`),
  a seccomp filter, a private `/tmp` and an empty runtime dir, `/etc`, `/usr`, `/lib*` read-only, `/sys` bits for
  device discovery, the GPU nodes (`/dev/dri`, `/dev/udmabuf`), fonts, GStreamer data, the session bus only
  through `xdg-dbus-proxy`. The home directory, `~/.ssh`, the rest of `/run`, `/mnt`, `/media` are not visible.
* The **network process is not sandboxed by WebKit**: it handles cache, cookies, TLS and writes downloads
  (so the downloads folder needs no rule). It has no JavaScript.
* The nested Wayland display (wpebackend-fdo) is a socket whose fd goes to the web process over
  the IPC connection, so it is not a path in the sandbox; the dmabuf and shm paths of the recent fix only need
  the render node, which `/dev/dri` covers (the permission to open it comes from the user's group `seat`;
  supplementary groups stay valid for file access inside an unprivileged user namespace, as Flatpak's
  `--device=dri` relies on, but this is exactly the thing to confirm in the QEMU test below).

What FreeBWS does:

* Before it creates the web context, `freebws` runs bwrap once itself with the same kinds of namespaces (a probe
  `bwrap ... freebws --sandbox-child`) and checks that `xdg-dbus-proxy` exists. WebKit does not check this and
  *aborts the browser* at the first web process if bwrap cannot start.
* **The probe and the FreeLFS folder layout.** The first probe (freebws release 10) failed in the image with
  `bwrap: execvp /sysflfs/bin/freebws: No such file or directory`. On FreeLFS (until base-files 4, 2026-11-13) `/usr` was the *homes* directory (the
  `@home` subvolume) and `/usr/bin`, `/usr/lib`, `/usr/share` were relative links to `../sysflfs/...`. The probe bound `/usr`
  as a whole and then ran `/proc/self/exe`, whose real path is `/sysflfs/bin/freebws`, which does not exist in bwrap's
  new root. That was a bug of the probe, not a sign that bwrap cannot work; binding `/usr` would also have shown every
  user's home inside the sandbox. The probe now makes the binds WebKit makes itself (`/usr/lib`, `/usr/bin`, `/lib`,
  `/lib64`, `/bin` one by one: bwrap resolves the links of a source path) and binds the directory of the real
  executable read-only at its real path. WebKit's own binds (`BubblewrapLauncher.cpp`) never bind `/usr` as a whole
  either, and bind the real path of every symlinked helper (`/usr/libexec/gstreamer-1.0/...`), so the web processes
  are not affected by the layout. Checked in QEMU: the old probe reproduces the error, the new one exits 0 and
  `/usr/<user>` is not visible inside.
* `sandbox=auto` (default, key in `~/.config/freebws/config` or `FREEBWS_SANDBOX`): if the probe works, the sandbox is on;
  if not (no user namespaces, a container, a missing program) the web processes run **without** a sandbox, a
  `freebws: WARNING: the web process sandbox is NOT available (reason)` line goes to stderr (the freebws log) and
  Settings > About says "OFF". `sandbox=on` refuses to start instead. `sandbox=off` is for comparison and
  warned about as well.
* `freebws --sandbox-check` prints the verdict (exit 0 or 1).
* Added to the sandbox on top of WebKit's list: `/usr/share/freebws` read-only and `FREEBWS_SANDBOX_RO=/a:/b`.

Test steps for the machine that has the rebuilt packages: `docs/INTEGRATION-TODO.md`, "Sandbox".

Limits: a sandbox escape of the kernel (user namespaces enlarge the kernel attack surface a little; flatpak and
every other browser accept that), the network process outside it, the GPU driver is reachable from the web process by
design (render node), `/etc` is readable by the web process (keep secrets out of it; `/etc/shadow` is not readable by
the user anyway).

---

## 4. Secure Boot (not implemented)

State: `flboot` (the boot loader) and the kernel are not signed, so Secure Boot must be off in the firmware
(already stated for the USB and ISO images). The kernel has `CONFIG_EFI_STUB`, and flboot starts kernels through their
EFI stub with the initrd named on the command line. Realistic options, in order of effort:

1. **Own keys, enrolled in the firmware** (smallest, a day or two): generate PK/KEK/db, sign `BOOTX64.EFI` (flboot)
   and `vmlinuz` with `sbsign`, sign the kernel modules (the kernel needs `CONFIG_MODULE_SIG` with a key kept
   from the kernel build: modules like xpad and hid-playstation are built `=m`), have the installer or a
   `flboot-install` option put the `.auth` files on the ESP. The user must put the firmware in Setup Mode and enroll them (or use
   `sbctl`/KeyTool, one boot with the firmware menu). Fine for enthusiasts; no third party trusted; does not
   work for someone who wants to dual boot Windows with its Microsoft-signed chain unchanged (the user
   keeps Microsoft's db entries and adds ours, which is possible: enroll *into* db, not replacing it).
2. **Existing signed shim + MOK** (more work, about a week plus testing on several machines): ship a
   distribution's Microsoft-signed `shim` as the first stage, flboot as its second stage under the file name shim
   expects (or a shim build option), and our certificate enrolled through MOK (`mokutil --import` and the blue MokManager
   screen on the next boot: needs physical presence and a one-time password). flboot must then load the kernel with
   `LoadImage` so that shim's verification hook applies, and the kernel and modules are signed by our MOK key. Works on
   unmodified machines with Secure Boot on. The trademark and redistribution terms of the shim binary have to be read; every
   shim update (revocations: SBAT) means a new image.
3. **Our own shim signed by Microsoft**: needs a shim-review submission and an extended-validation code signing certificate
   for the Microsoft portal; not realistic for a hobby distribution at this stage.
4. Not a replacement but needed with any of these: Secure Boot verifies only the PE images. The initrd and
   the command line that flboot passes are **not** verified (the kernel stub reads `initrd=` from the ESP), and the root file
   system has no dm-verity. A signed unified kernel image (kernel + initrd + command line in one signed PE) closes the
   first gap; verity would be a project of its own. Kernel lockdown (`lockdown=integrity`) comes on by itself under
   Secure Boot and disables unsigned modules, `/dev/mem` writes, and hibernation to an unverified image.

Recommendation: keep "Secure Boot off" in the release notes now; do option 1 first (it also builds the signing
infrastructure option 2 needs); decide on option 2 once there is a second tester with a locked-down laptop.

---

## 5. Local root actions: the password policy

The setuid helpers (`root:seat 4750`) run root actions for everybody in group `seat`, which is every person at
the machine. Any program that runs as the person (a browser renderer, a document parser) can start them. So each
helper that changes the system asks for the person's password before it acts, unless the action is harmless
(the same idea as the polkit defaults on desktops). The policy is in code, in `src/flauth/flauth-check.h`,
and in each helper's `flauth_gate` or `flauth_allowed` call.

### 5.1 The policy

| Action | Helper | Password | Class (`flauth --class`) | Reuse |
|---|---|---|---|---|
| Mount, unmount, eject a removable drive, the live boot stick | `flmount` | none | - | - |
| Mount, unmount, eject an internal disk (`--internal`) | `flmount` | every time | `mount-internal` | 2 min, same program |
| Power off, restart, log out, power saving | `flpower` | none | - | - |
| Lock screen password check | `fllock-check` | none (checks the person's own password, one try a second) | - | - |
| Install, remove, upgrade system packages | `flpkg-seat` | every time | `pkg` | 2 min, same program |
| Install (disk write, format, partition) | `flinstall-helper` | every time; only when started by `/usr/bin/flinstall` | `disk-write` | one-shot |
| Create the persistent home file | `flpersist create` | every time | `persist-create` | one-shot |
| Snapshot rollback | `flsnap rollback` | every time | `snap-rollback` | one-shot |
| Snapshot delete, rm, prune | `flsnap` | every time | `snap-delete` | one-shot |
| Snapshot create, list, show, status | `flsnap` | none; an elevated create never prunes | - | - |
| Date, time zone, automatic time | `fltime-set` | every time | `time` | 2 min, same program |
| Boot screen style | `flsplash-set` | every time | `splash` | 2 min, same program |

"Every time" means: no password is asked again while a stamp of that class is fresh for the same program.

### 5.2 The stamp

`flauth --class CLASS ...` checks the password, then writes `/run/flauth/ok-UID.CLASS` (root owned, mode 0600, in
a root owned 0700 directory) with three lines: the class, the executable of the program that started flauth
(resolved through `/proc/PPID/exe`), and the time. A helper accepts the stamp only when all of these hold:

- the class is the one the helper asks for (the file name and the content both);
- the executable of the program that started the helper is the one the stamp names (the same program, so a stamp of
  the Store cannot be used by another program);
- the stamp is at most 120 seconds old and not from the future;
- the file is private: owned by root, mode 0600, one link, in a private directory.

Reusable classes (`pkg`, `time`, `splash`, `mount-internal`) keep the stamp for those 120 seconds. One-shot classes
(`disk-write`, `persist-create`, `snap-rollback`, `snap-delete`) remove it as the check passes, so the next
destructive action asks for the password again. A check that fails does not use the stamp up.

### 5.3 The live tester and empty passwords

An empty password never passes on an installed system: flauth says `set a password first`, and the helpers
refuse. The one exception is the live session's `tester` account, and only while `/run/live` exists (the live
system). On that account no question is asked for any class, including the installer's disk write. That is the
behaviour the live session needs; the installed system is not affected.

### 5.4 Other arguments and environment

- `flauth` closes every descriptor it inherits, and the helpers that run children close theirs (`fltime-set`,
  `flsplash-set`, `flsnap` when it runs with more rights, `flmount`, `flpersist`, `flpower`). The environment is cleared
  before anything runs as root (`flpersist`'s child, `flpkg-seat`, `flsplash-set`, `fltime-set`, `flsnap`, `flmount`).
- Package names, zone names, boot-screen words, snapshot numbers and disk names are checked before any action. A zone
  name that is not a zoneinfo file is refused before anybody is asked for a password.
- `flinstall-helper`: its start is refused unless its parent is `/usr/bin/flinstall` (resolved). Its disk write needs the
  stamp. A refused install answers `NEEDAUTH disk-write` and the window asks the person to authenticate
  (`src/flinstall/README`).
- `flsnap create` by a person does not prune old snapshots any more. Removing them is `rm` or `prune`, which need the password.
  The hooks run as root and keep the old retention.

### 5.5 Tests

- `make -C src/flauth check` (no root): `src/flauth/tests/policy.sh` checks the stamp class and caller, the
  reuse window, one-shot consumption, stale and mis-moded stamps, wrong passwords, and the live-only empty-password rule.
  Each rule was also broken on purpose to confirm the test fails.
- `src/flpackages/tests/seat.sh`, `src/fltime/tests/test.sh`, `FLSPLASH_SET_TEST` (`src/flsplash`): the existing
  helper tests still pass with the gates compiled in.
- The root paths (a real setuid start, a real install, a real rollback) are not run here: they need root and disks.
  They were compiled and read, not run.
- `src/flsnap/tests/test-flsnap.sh` already fails 39 checks on the committed version as well; that is not from this change.

### 5.6 What this does not protect against

- A program the person runs can reuse a `pkg`, `time`, `splash` or `mount-internal` stamp for two minutes if it is the same
  executable as the one that asked. Destructive actions still need a fresh password each time.
- The stamp is bound to the calling program's executable, not to the window. A program that can run the same
  executable (a second copy of the Store, for instance) inherits the two minutes.
- `fllock-check` remains a one-try-a-second oracle for the person's own password, as before.
- On the live system the `tester` account needs no password, so any seat member there can run the installer's disk write
  through `/usr/bin/flinstall` without a question. This is the live session's design, not a fix.
- Nothing here has been run as root on hardware.


## 6. The root account, login, kernel hardening, firewall and setuid files (round 23)

Source of the findings: `docs/bugscan-2026-11-18/security-audit.md`. Decisions below are the user's (root accounts) or the
audit's with the changes named. Every statement here says how it was checked; section 6.8 lists what was not done.

### 6.1 The root account

| Image | Root | How |
|---|---|---|
| Live ISO (released) | **locked** (`!` in `/etc/shadow`), no login on any terminal | `build/53-iso.sh` sets the hash and the build **fails** if the root hash is empty or not locked; `build/50-image.sh` and `53-iso.sh` call `check_root_locked` (`build/lib.sh`) first |
| Live user `tester` | empty password, group `seat`, **only on `/run/live`**: flauth, the setuid helpers, the greeter (`flgreet-ui`) and the lock screen (`fllock-check`) accept the empty hash for exactly this account while the live marker exists; anywhere else an empty hash opens nothing | section 5.3; `src/flgreet/flgreet-ui.c`, `src/fllock/fllock-check.c` |
| Installed system | **your choice in the installer's account step**: "Use the same password for the administrator (root)" (default, as before), or a second password with confirmation. Root is never installed empty. Minimum length is **8** characters (was 4) | `src/flinstall/` (`set rootmode same|own`, `set rootpassword`) |
| Dev / test images | empty root hash, `ttyS0` in `/etc/securetty`, `/etc/issue` says "Development image", and `bootstrap.pub` stays trusted | **only** with `FLFS_DEV_ROOT=1` (`build/root-lock.sh --dev`, `build/53-iso.sh`): the serial-console test harnesses log in as root |

`build/60-packages.sh` now locks root by default (`passwd -l root`; `FLFS_DEV_ROOT=1` keeps the old empty root). `build/root-lock.sh`
switches the build root between the two states without rebuilding packages. `build/40-system-config.sh` follows the same flag.

### 6.2 Where root may log in

- `/etc/securetty` (package `base-files`) lists tty1 to tty6. `CONFIG` in `/etc/login.defs` is `CONSOLE /etc/securetty`, so `login` refuses root on
  any other terminal, the serial port included. The installed system gets this file from the same package. (The agetty on the serial port is
  started only when a UART exists, `rc.d/serial-getty`, round 22.)
- `su` is setuid but for group `wheel` only (`SU_WHEEL_ONLY yes`; the installer puts the first user in `wheel`). `LOGIN_RETRIES` is 3.
- The graphical greeter lists accounts from uid 1000 up, so root never appears there.
- There is no faillock: each `login` run has three tries, and agetty restarts it. This is **not** a rate limit (section 6.8).

### 6.3 Kernel and sysctl hardening

`/etc/sysctl.d/10-freelfs-hardening.conf` (package `initscripts`) is applied by `rc.sysinit` (`sysctl -q --system`):

| Setting | Value | Why |
|---|---|---|
| `kernel.kptr_restrict` | 1 | kernel addresses hidden from users |
| `kernel.dmesg_restrict` | 1 | `dmesg` for root only (`/var/log/dmesg.log` is now 0640 root:adm) |
| `kernel.unprivileged_bpf_disabled` | 1 | no BPF for users |
| `net.core.bpf_jit_harden` | 2 | |
| `kernel.yama.ptrace_scope` | 1 | children only |
| `fs.protected_symlinks`, `protected_hardlinks`, `protected_fifos`, `protected_regular` | 1, 1, 2, 2 | |
| `fs.suid_dumpable` | 0 | |
| `kernel.kexec_load_disabled` | 1 | no new kernel at run time (the kernel keeps KEXEC built: planned) |
| `kernel.io_uring_disabled` | 2 | io_uring off at run time (stays built; `0` turns it back on) |
| `net.ipv4.conf.*.rp_filter`, `accept_redirects`, `send_redirects`, `accept_source_route`, `secure_redirects`, IPv6 redirects and source route, `tcp_syncookies`, `icmp_echo_ignore_broadcasts` | strict / off / on | |
| `net.ipv4.ping_group_range` | `0 2147483647` | `ping` without setuid |

User namespaces stay **on** (Flatpak, bubblewrap and the browser sandbox need them; `user.max_user_namespaces` is not touched).

Kernel config (`config/kernel/freelfs.config`, both `linux` 7.3.4 and `linux-lts` 6.18.63 build with it; the recipe fails if a line is not applied):
`SECURITY`, `SECURITY_YAMA`, `SECURITY_LOCKDOWN_LSM` (+`_EARLY`, `LOCK_DOWN_KERNEL_FORCE_NONE`: available, mode **none** until the command line says
`lockdown=`), `SECURITY_DMESG_RESTRICT`, `HARDENED_USERCOPY`, `INIT_ON_ALLOC_DEFAULT_ON`, `FORTIFY_SOURCE`, `BPF_UNPRIV_DEFAULT_OFF`,
`SLAB_FREELIST_RANDOM`, `SLAB_FREELIST_HARDENED`, `RANDOMIZE_KSTACK_OFFSET_DEFAULT`. `IKCONFIG` and `IKCONFIG_PROC` were already on: `/proc/config.gz`
shows the real configuration. securityfs is not built in this kernel, so `/sys/kernel/security/lockdown` does not exist; the lockdown mode is `none` by config (`LOCK_DOWN_KERNEL_FORCE_NONE`), and `lockdown=integrity` on the command line would still apply. The offline check (defconfig + fragment + olddefconfig on both source trees) applied every line.

### 6.4 The firewall

Package `nftables` 1.1.7 (GPL-2.0-only; with `libmnl` 1.0.5 LGPL-2.1+ and `libnftnl` 1.3.2 GPL-2.0+; the three add about 1 MB to the root, no CLI
shell, no JSON, no Python). `/etc/nftables.conf` (package `initscripts`) is applied by the rc service `firewall`, **first** in `SERVICES`, before the network:

- inbound: drop invalid; accept established/related, loopback, ICMP errors and echo, ICMPv6 errors, echo and neighbour discovery, DHCP and DHCPv6 replies;
  **mDNS (UDP 5353) only while avahi runs**: the chain `mdns` is empty, and `flcupsd` fills it (`/usr/libexec/fl-firewall-mdns on`) when it starts avahi and empties it when avahi ends
- forward: drop. Outbound: accept (the browser, Flatpak, downloads and Wi-Fi are outbound).
- `FIREWALL=0` in `/etc/rc.conf` turns it off. A system installed before round 23 keeps its `rc.conf`: add `firewall` to `SERVICES` there.

### 6.5 Setuid and setgid files, every one

After this round (list checked by `build/m23/sys.cmds` against the booted system, and `build/m23-static.sh` against the ISO):

| File | Mode, owner | Why it stays |
|---|---|---|
| `/usr/bin/passwd`, `chage`, `chfn`, `chsh`, `gpasswd`, `newgrp` | 4755 root | shadow-utils: a user changes their own password and groups |
| `/usr/bin/su` | 4755 root | `su` for group `wheel` only (`SU_WHEEL_ONLY yes`): the terminal way to become root on an installed system |
| `/usr/bin/fusermount3` | 4755 root | FUSE mounts for users (`user_allow_other` stays off) |
| `/usr/bin/traceroute` | 4750 root:seat | needs raw sockets and `flpkg` has no file capabilities (xattrs) yet; only group seat can run it |
| `/usr/bin/flauth`, `flpower`, `flmount`, `flpersist`, `fltime-set`, `flsplash-set`, `flpkg-seat`, `flsnap`, `/usr/libexec/flinstall-helper`, `fllock-check` | 4750 root:seat | section 5 and 15.4 |
| `/usr/libexec/dbus-daemon-launch-helper` | 4750 root:messagebus | system bus service activation |
| `/usr/bin/wall`, `write` | 2755 root:tty (setgid) | writing to terminals |

**Removed** this round: `ping`, `ping6` (now 0755; `net.ipv4.ping_group_range`), `mount`, `umount` (0755; `flmount` mounts for group seat),
`newuidmap`, `newgidmap` (0755: `/etc/subuid` and `/etc/subgid` are empty and no rootless container tool is shipped; put the bit back, `chmod 4755`, when one is).

### 6.6 Package trust

`bootstrap.pub` (the build repo's key) is **not** installed in release images: only `freelfs-repo.pub` is trusted (`build/60-packages.sh`, `53-iso.sh`
remove it; dev images keep it). The private bootstrap key never was in an image.

### 6.7 Tests (round 23)

`build/m23-all.sh` (table in the round 23 report): `build/m23-static.sh` (the ISO's files), `build/m23-real.py` (the real ISO: root refused on the serial port,
tester checks), `build/m23/sys.cmds` (the dev twin: sysctls, firewall with a namespace pair, setuid list, flauth behaviour),
`src/flauth/tests/policy.sh` (policy), `src/fllock/tests/check.sh`, `src/flinstall/tests/image-test.sh` (same password and a separate root password,
the 8-character rule).

### 6.8 Not done (audit items still open)

- **Module signing** (`MODULE_SIG`, `MODULE_SIG_FORCE`): needs a key decision (where the key lives, how drivers built later are signed). Planned.
- `KEXEC` and `IO_URING` stay built in; the sysctls switch them off. Removing them from the kernel is planned.
- No faillock or login rate limit (`login` has three tries, then agetty restarts it).
- The browser's network process is not sandboxed (M2), adblock lists are fetched unsigned (M5), the persistence image is plaintext (M6),
  the bluez Agent interfaces are open to every local process (M8), cups.sock is mode 0666 (L3), `/run` in the installed fstab (L1), test secrets in
  the repo (L2), the repo signing key has no passphrase (M1), `flpkg` has no file capabilities (so `traceroute` stays setuid).
- The installer hashes with SHA-512 crypt while `login.defs` says yescrypt: both work; not unified.
- Nothing here has been run on the laptop.
