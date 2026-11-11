# FreeLFS security notes

What is protected today, how the signing key is kept, how to rotate it, how known
vulnerabilities are tracked, the browser sandbox, and what Secure Boot would take.
Written 2026-11-11. Nothing here is audited by anyone but the authors.

Short list of what exists:

| Area | State |
|---|---|
| Package repo | minisign-format Ed25519 signatures on every package and on the index; the index carries a serial and an expiry (`valid-until`) and flpkg refuses expired or older ones |
| Signing key | one key, offline custody rules below; a rotation mechanism with a transition period |
| CVE tracking | `build/cve-check.py` + `build/cve-names.conf`, report in `docs/CVE-REPORT.md` |
| Browser | WebKit's bubblewrap sandbox for the web processes (needs the rebuilt wpewebkit, see below) |
| Boot | not signed: Secure Boot must be off |

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
| Kernel: the 6.18 stable line | monthly, and within 14 days for a high one that applies to the config | most kernel CVEs concern drivers FreeLFS does not build; the report lists them all, triage by config |
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
`bubblewrap` and `xdg-dbus-proxy` exist and are in the image). Until wpewebkit release 1 it was built **off**:
the web processes, the part of a browser that runs hostile JavaScript and image/font/video parsers, ran with the
user's full rights. **wpewebkit release 2** builds it **on**, freebws release 10 handles it.

How it works (WebKit's own design, `BubblewrapLauncher.cpp`):

* In the WPE 2.0 API the sandbox is enabled by default in the web context.
  `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` turns it off.
* Each *web* process is started as `bwrap --args ... -- WPEWebProcess`: new user, pid, ipc, uts namespaces,
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
