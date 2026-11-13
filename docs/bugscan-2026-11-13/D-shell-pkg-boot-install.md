# Bug scan D: shell, package manager, snapshots, boot loader, initrd, installer

Scope: src/flsh, src/flpkg, src/flsnap, src/flboot, src/flinitrd, src/flinstall (incl. helper.c, the root helper). Read-only scan, nothing built or run.

Coverage (be aware of the gaps):
- Read in full: src/flinstall/helper.c, common.h; src/flpkg/txn.c, tar.c, pkgfile.c, sysusers.c, snapshot.c, zst.c; src/flpkg/sign.c (verification half), trust.c (freshness half), repo.c (fetch/sync half); src/flboot conf.c, fs.c, start.c, boot.c, main.c, lib.c (allocator and strings); src/flsh/guard.c; src/flpackages/flpkg-seat.c (the seat-group setuid helper for flpkg).
- Read in part: src/flsnap/flsnap.c (config, setup, mount, rollback, delete paths), src/flinitrd/flinitrd.c (search, mount, switch_root), src/flpkg/db.c (SQL is checked via grep plus key functions), src/flpkg/cmd_pkg.c (install path only), src/flboot/menu.c (grep only), src/flinstall/flinstall.c (window, grep only; it runs unprivileged).
- Not read (grep only): src/flsh parse.c, exec.c, expand.c, builtin.c, edit.c, job.c, var.c, complete.c, highlight.c, hist.c, prompt.c, test.c, arith.c, util.c. src/flpkg build.c, pack.c, solve.c, config.c, spawn.c, hash.c, version.c (only the split function), util.c (partial).
- So the flsh findings are thin. There is no memory-safety verdict for the shell.

Severity rules used: HIGH = privilege boundary or data loss reachable without an extra malicious step; MEDIUM = real bug with a plausible trigger; LOW = hardening or a trigger that needs signed/malicious input or unusual state.

---

## HIGH

1. **flsnap is setuid root for the group seat, and rollback/delete have no check beyond group membership.**
   /home/TortinaZ/Dokumente/FreeLFS/src/flsnap/flsnap.c:903-906 (dispatch to cmd_delete, cmd_prune, cmd_rollback), 839-846 (the rename and copy that switch the default subvolume), recipe owner line /home/TortinaZ/Dokumente/FreeLFS/recipes/flsnap/recipe:12 (`owner=("/usr/bin/flsnap 0 98 4750")`).
   Any seat member can make a different snapshot the next boot state, or delete snapshots. flinstall adds the first account to group seat (src/flinstall/common.h and helper.c join list), so an ordinary installed user can do this.
   Trigger: a normal desktop user runs `flsnap rollback 3`, or deletes the snapshots that were the safety net.
   Action: confirm this is intended. If not, gate rollback and delete behind authentication (polkit or the root password).

2. **flpkg-seat lets any seat member install or remove packages as root without a password.**
   /home/TortinaZ/Dokumente/FreeLFS/src/flpackages/flpkg-seat.c:63-107 (setuid root, then execv of `flpkg install|remove NAME... -y`), recipe /home/TortinaZ/Dokumente/FreeLFS/recipes/flpackages/recipe:14.
   The argument checks are sound (valid_name, env cleared, no leading `-`, no .flpkg). The policy is the problem: `-y` skips confirmation, and nothing here checks the package against dependents or core packages (cmd_remove in cmd_pkg.c was not read).
   Trigger: a non-admin seat user runs `flpkg-seat remove glibc` or installs any repo package.
   Action: confirm the design (the header says "without a password" on purpose). Consider requiring authentication for remove.

---

## MEDIUM

3. **PAX record length is not range-checked, so the parser reads outside its buffer.**
   /home/TortinaZ/Dokumente/FreeLFS/src/flpkg/tar.c:305-308 (parse_pax): `n` is `unsigned long` from strtoul, and the check is `off + n > len`. A second record with `n` near ULONG_MAX makes `off + n` wrap to a small value, so the check passes. Then `stop = buf + off + n - 1` and `memchr(kv, '=', stop - kv)` run far outside the buffer.
   Trigger: a crafted .flpkg given to `flpkg install` (local files are not signature-checked, see 4), or a repo package signed by a compromised key (signature is checked before parsing, repo.c:245). Result: out-of-bounds read, crash as root.
   Fix: compare `n > len - off` after checking `off <= len`.

4. **Local `flpkg install FILE.flpkg` skips the signature check that `signatures=required` promises.**
   /home/TortinaZ/Dokumente/FreeLFS/src/flpkg/cmd_pkg.c:101 (txn_add_file for file arguments), txn.c:332-337 (txn_add_file calls pkgfile_open only), pkgfile.c:35-71 (no verification). The default policy is at config.c:50; the signature check exists only on the repo path (repo.c:202-245, repo_fetch).
   Trigger: root runs `flpkg install ~/Downloads/x.flpkg` on a file from the internet. It installs unverified, and finding 3 and item 6 become reachable from that input.
   Fix: verify local files too (or print a loud warning and require `--unsigned`).

5. **migrate_to_link runs before the journal exists, and before the "nothing was changed" promise.**
   /home/TortinaZ/Dokumente/FreeLFS/src/flpkg/txn.c:579-605 (check_files calls migrate_to_link at line 603), 538-566 (merge_dir moves files with rename), 987-988 (`die("file conflicts; nothing was changed")`), 1006 (write_journal, after the check).
   Trigger 1: an upgrade where a package brings a link over a real directory and merge_dir hits a name clash at txn.c:525. It dies with half the files moved, and the message "nothing was changed" is false.
   Trigger 2: power loss in the middle of merge_dir. Files are split between the old and the new path, and there is no journal entry to roll back.
   Fix: plan the merge as journaled ops, or move it after write_journal.

6. **Paths are not canonicalized, so `..` and aliases defeat the owner, conflict and remove checks.**
   /home/TortinaZ/Dokumente/FreeLFS/src/flpkg/pkginfo.c:262-288 (manifest_parse accepts any path), tar.c:412 (clean_path strips only leading `./` and `/`, keeps `..`), txn.c:606-611 (owner lookup by string), 697-702 (plan_remove by string), 723-729 (OP_FILE), 912 (apply_ops rename).
   Note: with root `/`, a leading `..` cannot leave `/`. This is an ownership bypass, not an escape.
   Trigger A: package B lists `usr/../etc/x` while `/etc/x` belongs to package A and was created after B's install. The owner check misses it. When B is later removed, plan_remove deletes A's file.
   Trigger B (layout leftover, see the layout section): a DB row says `sysflfs/bin/x` (the old real directory, now a link to usr). The new package lists `usr/bin/x`. `flpkg install --overwrite '/usr/*' new.flpkg` then runs OP_FILE (installs the new file at usr/bin/x), followed by OP_REMOVE for `sysflfs/bin/x` (plan_remove, txn.c:697-702). That OP_REMOVE renames the new file to a backup (apply_ops, 912), and finish_op (182-194) deletes the backup. Result: the newly installed file is gone.
   Whether Trigger B can happen depends on whether installed DB rows still contain `sysflfs/` paths (uncertain, see the uncertain list).
   Fix: reject `..` and absolute names in manifest_parse; canonicalize `sysflfs/` to `usr/` on read, or resolve each path through the real directory before ownership lookup.

7. **The installer's "in use" test does not see device-mapper, LVM, or md holders.**
   /home/TortinaZ/Dokumente/FreeLFS/src/flinstall/helper.c:470-490 (mounted_source: checks only /proc/mounts and /proc/swaps for each partition node), 906+ (build_plan's busy check, which relies on it). grep finds no use of `/sys/block/*/holders` anywhere in helper.c.
   Trigger: the user picks a disk that holds an unlocked LUKS container or an active LVM/md set on its partitions. Nothing is mounted, so busy stays 0, and the disk is repartitioned and wiped. The user chose the disk, so this is a safety-check gap, not a remote attack.
   Fix: check `/sys/block/<dev>/holders` and the whole-disk `busy` state.

8. **Crash durability: staged files and serial/passwd temp files are not fsync'd before the commit point.**
   /home/TortinaZ/Dokumente/FreeLFS/src/flpkg/txn.c:779-809 (stage_file: write, then close, no fsync), 956-981 (record: DB commit), 983-1040 (commit order). util.c:326-338 (write_file, no fsync) is used by trust.c:97-108 (write_atomic, serial file), sysusers.c:46-63 (passwd), and txn.c:852-875 (save_config). The only fsyncs are the journal (txn.c:126) and the mark (143).
   Trigger: power loss right after the DB commit. The database can record a package whose files are empty or missing. The header claim "every step before 4 is undone" covers names, not contents. Trust state can be lost the same way: an empty serial file makes serial_last return -1 and silently drops rollback protection (trust.c:111-128).
   Fix: fsync each staged file (and the directories) before record(), and fsync temp files before rename.

9. **flboot takes the configuration and kernels from any volume when the firmware gives no device handle, and passes the cmdline unauthenticated.**
   /home/TortinaZ/Dokumente/FreeLFS/src/flboot/fs.c:25-41 (fallback: first volume that has \flboot\flboot.conf becomes root and device), boot.c:25-27 (cmdline goes into LoadOptions), conf.c:201-215 (kernels and cmdline read from that volume).
   Trigger: a removable stick with `flboot\flboot.conf` (`cmdline init=/bin/sh ...`) and a `vmlinuz-*` file. Under Secure Boot LoadImage still checks the stub's signature, but the cmdline is not covered by that check. Whether that matters depends on the kernel's lockdown policy (uncertain).
   Fix: restrict the fallback to the volume the image came from, or measure/verify the config.

---

## LOW

10. **Range check overflows in the partition planner (window-supplied values).**
    /home/TortinaZ/Dokumente/FreeLFS/src/flinstall/helper.c:826 (`start + size > d->last_end / FI_MIB * FI_MIB` in add_new); start and size come from cmd_part via atoll (helper.c:2234).
    With start = 9223372036853776384 and size = 1048576 the sum wraps negative and the test passes. sfdisk is then the only backstop. Trigger: `part create 9223372036853776384 1048576 data` in manual mode.
    Fix: compare `size > last_end - start`.

11. **Parser recursion has no depth cap.**
    /home/TortinaZ/Dokumente/FreeLFS/src/flsh/parse.c:958-961 (`p->depth++` in parse_command; only ++ and -- found, no limit). Deeply nested `((((...` or `if if if ...` overflows the C stack, so flsh crashes. Only the shell's own crash, no escalation. flsh was otherwise not reviewed.

12. **sysusers and sysgroups entries are not validated.**
    /home/TortinaZ/Dokumente/FreeLFS/src/flpkg/sysusers.c:91-94 (`%255s` accepts `:`; uid and gid unchecked, so uid 0 is accepted), 103 (line built from raw fields), 46 (has_entry checks the name only). Signed packages only, but a signed package can add a second root-equivalent account.

13. **FLPKG_HOOKDIR overrides the hook directory even for the real root.**
    /home/TortinaZ/Dokumente/FreeLFS/src/flpkg/snapshot.c:60 (getenv before the default is chosen). The header at lines 9-11 says the override is for tests and other roots only. Trigger: root runs flpkg with an inherited FLPKG_HOOKDIR (sudo normally resets env, so this is low).

14. **Index filename check rejects `/` but not `.` or `..`.**
    /home/TortinaZ/Dokumente/FreeLFS/src/flpkg/repo.c:80. The dest path then becomes the cache directory, and the copy/rename/unlink hits the cache root. The index is signed, so this needs the repo key.

15. **Uninitialized `name` in the account rewrite.**
    /home/TortinaZ/Dokumente/FreeLFS/src/flinstall/helper.c, edit_accounts, is_group branch (about line 1266): `sscanf(line, "%63[^:]", name)` result is not checked, and the strcmp afterwards can read an uninitialized stack buffer.

16. **Silent fstab truncation with many partitions.**
    /home/TortinaZ/Dokumente/FreeLFS/src/flinstall/helper.c:1441 (`char buf[8192]`, lines appended with snprintf). With many data partitions (MAXPART 128) the tail of the file (/proc, /run, /tmp mounts) is dropped without an error.

17. **Base-256 tar sizes wrap silently.**
    /home/TortinaZ/Dokumente/FreeLFS/src/flpkg/tar.c:245-249 (get_number shifts a uint64 with no overflow check). A huge size desyncs the reader from the manifest. stage_file then fails the size check, so this is low.

18. **NULL dereference possible in the file-type column read.**
    /home/TortinaZ/Dokumente/FreeLFS/src/flpkg/db.c:311 (`((const char *)sqlite3_column_text(st, 1))[0]`, no NULL test). The database is root-owned and the schema is ours, so low.

19. **Target disk is checked once, before the wipe. Desktop automount can race it.**
    /home/TortinaZ/Dokumente/FreeLFS/src/flinstall/helper.c: build_plan (906) and do_install (2087) validate; format_one (helper.c:1862) and populate_root wipe and format later. A partition automounted by the live desktop in between is not re-checked. Uncertain whether udisks does this on the live image.

20. **access() tests the real uid in a setuid program.**
    /home/TortinaZ/Dokumente/FreeLFS/src/flsnap/flsnap.c:390 (`access("/run", W_OK)` picks /tmp for seat users). mkdtemp makes a root-owned 0700 directory, so not exploitable. Noted because the check is wrong for setuid code.

21. **Rollback window with no @ subvolume name.**
    /home/TortinaZ/Dokumente/FreeLFS/src/flsnap/flsnap.c:839-846: `@` is renamed aside before the new `@` exists. A power loss between the two leaves no `@` name. Booting still works because the default subvolume is chosen by id, but the state is inconsistent and the next rollback fails.

---

## Layout change (folder layout: /usr real, /sysflfs -> usr, homes in /home)

- The real risk is finding 6 (Trigger B). The string-based ownership model in txn.c treats `sysflfs/bin/x` and `usr/bin/x` as different files, but they are the same inode. Nothing in src/flpkg canonicalizes the prefix.
- The installer uses the new layout correctly: helper.c uses /home for the account and the skel copy (lines 1076, 1571, 1585), @home is mounted at /home (fstab_tail and customise(), helper.c:1424-1466), and the localtime link points into /usr/share/zoneinfo (helper.c:1494-1505). No code in these six directories assumes homes live under /usr.
- Stale comments only: txn.c:534-535 (`/var -> sysflfs/data`) and txn.c:599 (`usr/bin -> ../sysflfs/bin`) describe the old layout. cmd_pkg.c:444 says "the /sysflfs layout". Fix the wording so the next reader does not treat sysflfs as a real directory.
- guard.c:1 and :5 are written as if `/sysflfs` is the real vital directory. Now `rm -r /sysflfs/` (trailing slash) or `rm -r /sysflfs/*` may follow the link into usr. The vital-path list lives in flcrash, which is outside this scan (uncertain).

---

## Checked, no bug found (so the next scan can skip these)

- helper.c popen/exec: the only popen is `uname -m` (fixed command, helper.c:340). Every exec uses argv arrays (no system(), no shell). PATH and env are cleared when setuid (helper.c:2311-2315), and the test hooks (--image, --source, --kernel, --iwd, --workdir, FLINSTALL_FAKE) are refused when elevated (helper.c:2339).
- helper.c input from the window: disks must come from the helper's own list (find_disk); format fs is whitelisted; mount points are whitelisted (data_mp_ok, helper.c:895); locale, keyboard and time zone are validated (common.h:115-158, check_config). fstab values come from validated mount points or blkid output. The window never passes a path.
- helper.c dry-run: every write is behind `if (dry)` or cmdf-only, and run() returns before fork in dry mode.
- helper.c rpath (1114-1160): bounded copies, symlink depth limit 40, and `..` cannot climb above the root.
- db.c: all SQL uses prepared statements with bind; no string-built SQL (only one int formatted into a meta insert at db.c:100).
- sign.c verify_file_ex (348-424): prehashed ED only, key lookup by keynum, global and trusted-comment signatures both checked, file name bound.
- trust.c index_check (131-172): serial rollback and expiry checked before the index is used; index_commit only after a successful sync.
- sysusers/txn ordering and journal recovery: journal written and fsynced before any change (txn.c:112-138). Recovery decides from names on disk.
- zst.c: truncated streams detected (zr_read).
- flboot lib.c xalloc zeroes memory, so with_subvol (conf.c:138-160) is NUL-terminated. relocate() in start.c is bounded by the dynamic section.
- flinitrd remove_tree: uses fstatat with NOFOLLOW and skips other devices, so it does not follow links or delete mounts.
- flpkg-seat: arguments validated before any privilege change, env cleared, PATH fixed, fds closed.
- No system(), popen with user input, or sprintf into fixed buffers in my area apart from the items above (highlight.c:166 strcpy is length-guarded).

---

## Uncertain

- Whether installed flpkg databases still have `sysflfs/` paths (decides whether finding 6, Trigger B, is reachable in practice). Check with a read-only query of the packages/files tables.
- flcrash's vital-path list (not in this scan): whether `/sysflfs/` with a trailing slash or `/usr` is covered by the guard.
- Secure Boot policy on the target machines: whether an unauthenticated cmdline matters (finding 9).
- Whether the live desktop automounts newly visible partitions while the installer runs (finding 19).
- flpkg build.c, pack.c and solve.c were not read: symlink handling when packing, and the solver's handling of conflicting packages, are unknown.
- flsh: parse.c, exec.c, expand.c, builtin.c, edit.c, var.c and the rest were not read. Expect more findings there.
- flinitrd mounts squashfs from any file found on a stick (case 2). That is kernel-parser exposure and was not assessed.
- cmd_remove dependency handling (cmd_pkg.c) decides how far flpkg-seat remove (finding 2) can go; not read.
