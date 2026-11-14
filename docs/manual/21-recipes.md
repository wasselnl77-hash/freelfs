# 21. Recipes: how packages are described and built

Summary: a recipe is a bash file in `recipes/NAME/recipe` that says what to download, how to build it and what
goes into the package; `flpkg build` runs it and writes `NAME-VERSION-RELEASE-ARCH.flpkg`.

## Contents

1. Where recipes live and what `flpkg` does with one
2. The fields (variables)
3. Functions: `prepare`, `build`, `check`, `package`, `package_NAME`
4. The build environment
5. Standard build styles (`build_style`)
6. Options (`options=`)
7. Owners, users and groups (`owner=`, `sysusers=`, `sysgroups=`)
8. Dependencies, provides, conflicts, replaces
9. Groups
10. Versions and releases
11. Example 1: a plain library, `zlib`
12. Example 2: an application from this tree, `flview`
13. Writing a new package, step by step
14. Checks that flpkg makes for you

---

## 1. Where recipes live and what `flpkg` does with one

There are 274 recipe directories in `recipes/`, one per package (or per split recipe). A recipe directory holds
the file `recipe` and any local files it needs, such as patches. A local source is a plain file name next to the
recipe (a path with `/` is refused).

`flpkg build [-o DIR] [-s DIR] [--nocheck] [--keep] RECIPE_DIR` does this (`src/flpkg/build.c`):

1. Runs `src/flpkg/recipe.sh dump RECIPE`, a bash helper that sources the recipe in a clean environment and prints
   its variables and functions. Nothing from the recipe runs as the build yet.
2. Checks the fields (`validate()`): `pkgname` set and valid; `version` set and free of `-`, `:`, `/` and spaces;
   `release` a number; `summary` set; one `sha256` per `source`, each 64 lowercase hex characters; `arch` lists
   `x86_64` or `any`; a `package()` function, or a `package_NAME()` for each name.
3. Finds each source: first in the directories given with `-s` and in the cache (`$cfg.cache/sources`, default
   `/var/cache/flpkg/sources`), then downloads it with `curl` if it is a URL, and checks its SHA256 (a mismatch
   stops the build). `NAME::URL` in `source=()` saves the download under the name `NAME`. `file://` URLs are used
   in place.
4. Unpacks archives (`.tar`, `.tar.gz`, `.tar.xz`, `.tar.bz2`, `.tar.zst`, `.tar.lz`, and their short forms) into `$srcdir`.
   Other files are copied.
5. Runs `prepare()`, `build()` and `check()` in order, each through `recipe.sh run`. `check()` is skipped with `--nocheck`.
6. For each package name, runs `package()` (or `package_NAME()`) into `$pkgdir`, then cleans the package root (section 6).
7. Refuses an empty package and the forbidden paths (`/bin`, `/lib`, `/sbin`, `/usr/local`, `/var/run`, `/var/lock`,
   `/run`, `/tmp`, `/dev`, `/proc`, `/sys`, `/home`, `/root`, `/mnt`, `/media`, `lost+found`), unless `!pathcheck`.
8. Writes `OUT/NAME-VERSION-RELEASE-ARCH.flpkg`, where VERSION-RELEASE is `version-release`, and prints its SHA256.

## 2. The fields

These are the variables `recipe.sh` reports (`scalars` and `arrays` in `src/flpkg/recipe.sh`). A scalar is one
value; an array is a bash array.

| Field | Kind | Meaning |
|---|---|---|
| `pkgname` | array | the package name or names. One name is normal; several names make a split recipe (`gcc` builds `gcc` and `gcc-libs`). |
| `pkgbase` | scalar | the base name of a split recipe. Default: the first `pkgname`. |
| `version` | scalar | upstream version. No `-`, `:`, `/` or spaces. |
| `release` | scalar | the package revision, a number (section 10). |
| `summary` | scalar | one line, shown in the package manager. Required. |
| `url` | scalar | project home page. Information only. |
| `license` | array | the licence names, for example `(MIT)` or `(GPL-2.0-only)`. |
| `arch` | array | `(x86_64)` for compiled packages, `(any)` for pure Python packages (32 recipes use `any`). |
| `groups` | array | the groups the package belongs to (section 9). |
| `source` | array | URLs or plain file names (patches). Empty `()` when the package's source is a tree in this repository. |
| `sha256` | array | one lowercase SHA256 per `source`, in the same order. `SKIP` is not allowed. |
| `worksrc` | scalar | the directory the archive unpacks to, when it is not `$pkgbase-$version`. |
| `depends` | array | runtime dependencies (section 8). Version constraints allowed. |
| `makedepends` | array | needed only to build. |
| `checkdepends` | array | needed only to run the tests. `flpkg` reads it into the recipe; `flpkg build` does not use it (a search of `src/flpkg` finds no other use). |
| `provides` | array | names this package also satisfies, for example `gawk` provides `awk` and `bash` provides `sh`. |
| `conflicts` | array | packages that cannot be installed with this one. |
| `replaces` | array | packages this one takes over from. |
| `sysusers` | array | system users to add, see section 7. |
| `sysgroups` | array | system groups to add, see section 7. |
| `owner` | array | owners and modes of files, see section 7. |
| `options` | array | build options, see section 6. |
| `build_style` | scalar | `autotools`, `meson`, `cmake` or `python`; supplies `build`, `check` and `package` when the recipe leaves them out (section 5). |
| `configure_args` | array | extra options for the configure, meson or cmake step of a `build_style`. |
| `pyname` | scalar | for `build_style=python`: the Python module name to install, when it is not `pkgbase`. |

Recipes may also keep variables of their own. `recipes/mupdf/recipe` has `_opts`, a list of build options, and
`recipes/freebws/recipe` has `dssl=`, the version of the DSSL style library it bundles. Only the fields above have a
meaning to `flpkg`.

## 3. Functions

| Function | When it runs | Typical content |
|---|---|---|
| `prepare()` | first, optional | patches, `sed` edits, copying files |
| `build()` | after `prepare` | configure and compile |
| `check()` | after `build`, unless `--nocheck` | the test suite (`make check`) |
| `package()` | one package | `make DESTDIR="$pkgdir" install` and removal of unwanted files |
| `package_NAME()` | for a split recipe, one per name | the same, for each package of the split |

A split package function may override `summary`, `depends`, `provides`, `conflicts`, `replaces`, `groups`, `owner`
and `sysusers` for its own package (`dump` is run again after it, and its variables are read back).

The `build_style` fills in any function the recipe leaves out:

- `autotools`: `./configure --prefix=/usr ${configure_args[@]}`, `make`; check is `make check`; package is
  `make DESTDIR="$pkgdir" install`.
- `meson`: `meson setup build --prefix=/usr --buildtype=release`, then `ninja -C build`; check is `ninja -C build test`.
- `cmake`: `cmake -S . -B build -G Ninja` with `CMAKE_INSTALL_PREFIX=/usr` and Release; then `ninja -C build`.
- `python`: `pip3 wheel` in the source tree, then `pip3 install --root="$pkgdir" --no-deps --no-index` from `dist/`,
  for the module `${pyname:-$pkgbase}`.

In the tree, 102 recipes use `autotools`, 48 `meson`, 11 `cmake`, and 10 `python`. The rest write their own functions.

## 4. The build environment

Every function runs under `set -e -o pipefail`, in a clean environment (`env -i` style). The variables a recipe sees:

- `srcdir`: where the sources are unpacked. Functions start in `$srcdir/$worksrc` (or `$srcdir` when that does not exist).
- `pkgdir`: the package root to install into. Only `package()` functions have it.
- `startdir`: the recipe's own directory (for patches and for files of the tree, as `flview` does).
- `pkgname`: the package name in a `package_NAME()` call.
- `CARCH` (the machine, `x86_64`), `TAR_OPTIONS=--no-same-owner`, `HOME` (the work directory),
  `TERM`, `LC_ALL=POSIX`, `PATH=/usr/bin:/usr/sbin`.
- `MAKEFLAGS=-jN`, `NINJAJOBS=N`, `TESTSUITEFLAGS=-jN` from the job count.
- `SOURCE_DATE_EPOCH` (chapter 20, section 10).
- `CFLAGS`, `CXXFLAGS` and `LDFLAGS` from `/etc/flpkg.conf` (empty by default, so upstream defaults apply).

The shell variables `srcdir`, `pkgdir`, `startdir` and `pkgname` are not exported. A program a recipe runs does not
see them, so a configure script cannot take `$srcdir` for its own.

## 5. Standard build styles

See section 3 for what each style supplies. A recipe with `build_style` only needs `package()` when the default
is not right, and needs a `build()` when the configure step has unusual parts. `configure_args` is the
easiest way to pass options.

## 6. Options (`options=`)

`options` is a list. Prefix an entry with `!` to turn a default off.

| Option | Default | Effect of `!option` |
|---|---|---|
| `strip` | on | `!strip`: keep debug symbols (the kernel and PE files use this) |
| `purge_la` | on | `!purge_la`: keep libtool `.la` files |
| `purge_infodir` | on | `!purge_infodir`: keep `/usr/share/info/dir` |
| `pathcheck` | on | `!pathcheck`: allow the forbidden paths of section 1 (only the base file system may own `/bin` and the like) |

The cleaning runs after `package()`: ELF relocatable objects (ELF type 1) and static archives get `--strip-debug`,
other ELF files get `--strip-unneeded`. Libtool `.la` files are removed, and `/usr/share/info/dir` is removed. Recipes
that need unstripped files use `options=(!strip)`. In the tree, six recipes have `!strip` (among them the Linux
kernels, and one PE image), and one recipe has `!pathcheck`.

## 7. Owners, users and groups

Four fields change files and accounts on the installed system:

- `owner=("PATH UID GID MODE")`: sets the owner and mode of a file in the package. The format is parsed with
  `sscanf` as `%4095s %u %u %o`. Without it, every file is owned by 0:0. Examples in the tree: `/usr/bin/flpkg-seat 0 98 4750`
  (a setuid helper for the `seat` group), `/usr/bin/wall 0 5 2755` (setgid `tty`), `/var/lib/dhcpcd 52 52 0700`.
- `sysusers=("NAME UID GID HOME SHELL 'COMMENT'")`: a system user, added to `/etc/passwd`, `/etc/group`,
  `/etc/gshadow` and `/etc/shadow` when those exist. Example: `"dhcpcd 52 52 /var/lib/dhcpcd /usr/bin/false 'dhcpcd PrivSep'"`.
- `sysgroups=("NAME GID")`: a system group. Example: `"avahi 84"`.
- The owners are recorded in the package when it is made (`pack.c`). If a file cannot get its owner when it is installed
  (the rootless image and the live system cannot set arbitrary owners), flpkg records it as pending, and `flpkg fix-owners`
  sets it later. `fix-owners --keep` is the form used on the live system, where the database must stay read-only.

The group `seat` (GID 98) is created by the `base-files` recipe. Helpers that need the group take it through `owner=`.

## 8. Dependencies, provides, conflicts, replaces

- `depends`, `makedepends`, `checkdepends` take names with optional constraints: `name`, `name>=1.2`, `name<=1.2`,
  `name=1.2-3`, `name>1`, `name<2`. The solver (`src/flpkg/solve.c`) uses them when installing.
- Build order: `60-packages.sh` sorts the recipes it builds with `tsort`, using `depends` and `makedepends`, and only
  among the names it was given (`order_recipes`). A build dependency must therefore be built before the recipe that
  needs it, in the same run or an earlier one.
- Desktop and live packages are installed into the build root as soon as they are built (`--nodeps`), so later
  packages can compile against them (chapter 20, section 5).
- `provides` lets one name satisfy another: `gawk` provides `awk`, `bash` provides `sh`, `pkgconf` provides `pkg-config`.
- `conflicts` are stored with the package and shown by `flpkg info`; whether they block an install is not verified in this pass.
  `replaces` is used by the solver (`solve.c`) for renames and merges: a package that replaces an installed one may take its place.

## 9. Groups

The `groups` field decides which set of packages a recipe belongs to. The tree uses these (counted from the recipes):

| Group | Recipes | Used by |
|---|---|---|
| `base` | 94 | the base system: `flpkg install base` installs it; the image root installs it |
| `desktop` | 171 | the desktop: the image root installs it; built packages go into the build root at once |
| `live` | 5 | the live ISO tools (`flinitrd`, `libburn`, `libisofs`, `libisoburn`, `squashfs-tools`); built and installed into the build root, not in the image |
| `lts` | 1 | `linux-lts` (the 6.18 fallback kernel); not in the default image |
| `deferred` | 2 | `libclc` and `spirv-llvm-translator`: skipped by `60-packages.sh` unless named on its command line |
| (none) | 1 | `flpackages`: built, in no group. It is kept as the provider of `flpkg-seat`. |

The exact group of every recipe is in its own `recipe` file. A recipe may list more than one group in the same
array, for example `groups=(desktop live)`.

## 10. Versions and releases

- `version` is the upstream version (`7.3.4`, `2.4.20`). It must not contain `-`. Upstream names that do not fit go into
  `worksrc` (for example `btrfs-progs` unpacks to `btrfs-progs-v$version`).
- `release` is the package revision, a number. Raise it when the recipe or the package changes but the upstream
  version does not, so that installed systems see a newer package. The round notes in `docs/ROADMAP-NEXT.md` record the
  bumps, for example `flcomp` release 11 and `flpanel` release 13.
- The file name and the package version are `version-release`, for example `zlib-1.3.2-1-x86_64.flpkg`.
- Version comparison is segment by segment (`flpkg`'s `version.c`), after an optional `epoch:`.
- A new upstream version is a `version` change plus new `source` and `sha256` values. Run `update-lock.sh` if the
  source is also in `sources.list` (chapter 20, section 7).

## 11. Example 1: a plain library, `zlib`

This is the real recipe `recipes/zlib/recipe`, unchanged:

```sh
pkgname=zlib
version=1.3.2
release=1
summary="Compression library implementing the deflate method"
url=https://zlib.net/
license=(Zlib)
arch=(x86_64)
groups=(base)
depends=(glibc)
source=(https://zlib.net/fossils/zlib-$version.tar.gz)
sha256=(bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16)

build() {
	./configure --prefix=/usr
	make
}

check() {
	make check
}

package() {
	make DESTDIR="$pkgdir" install
	rm -f "$pkgdir/usr/lib/libz.a"
}
```

What each part does:

- `groups=(base)`: part of the base system, so the image has it.
- `depends=(glibc)`: installed with it.
- `source=(URL)` and `sha256=(...)`: downloaded once, checked on every build.
- `build()` and `check()` are written out. The recipe does not use `build_style`. `check()` is not run by
  `60-packages.sh`, which passes `--nocheck`.
- `package()` installs into `$pkgdir`, then removes the static library. The rest is cleaned by flpkg.

The same recipe with `build_style=autotools` would drop `build()`, `check()` and `package()`, because the style
supplies them. Keep the explicit functions when the project needs something else.

## 12. Example 2: an application from this tree, `flview`

Applications written in this repository are built from `src/<name>`, not from a download. The recipe copies the
tree into the build directory, builds it there, and installs it. This is the real `recipes/flview/recipe`:

```sh
pkgname=flview
version=0.1
release=5
summary="FreeLFS image viewer: folder browsing, zoom, rotate, trash, wallpaper"
license=(MIT)
arch=(x86_64)
groups=(desktop)
depends=(glibc flui wayland libpng libjpeg-turbo libwebp)
makedepends=(flui libpng libjpeg-turbo libwebp)
source=()
sha256=()

build() {
	cp -r "$startdir/../../src/flview" "$srcdir/flview"
	cd "$srcdir/flview"
	make clean
	make
}

package() {
	cd "$srcdir/flview"
	make DESTDIR="$pkgdir" install
}
```

Points to notice:

- `source=()` and `sha256=()` are empty, which is correct because there is nothing to download. The arrays must still be
  present (flpkg reads them).
- `$startdir/../../src/flview` is the tree's application directory, relative to the recipe (`recipes/flview/` is two
  levels below the root).
- `makedepends` names the libraries the build needs (`flui` is this tree's toolkit, built and installed before it).
- `depends` names what the installed program needs at run time. `flui` appears in both lists.
- The Makefile of the program does the work (`make install` installs the binary, the `.desktop` file and the README
  under `$DESTDIR`). See chapter 24 for the Makefile pattern.
- `check` is not written, so there is no check step. `flview` has a `check` target in its Makefile, which you run by hand
  (chapter 25).

Other tree recipes that copy their program from `src/` in the same way include `fltime`, `fltheme`, `flcomp`, `flpanel`,
`freebws` and `flsettings`.

Split recipe, for contrast: `recipes/gcc/recipe` has `pkgname=(gcc gcc-libs)`, `pkgbase=gcc`, and two functions,
`package_gcc()` and `package_gcc-libs()`, each with its own `depends`, `summary` or `groups` when needed.

## 13. Writing a new package, step by step

1. **Choose a name.** It must match `valid_name()` in `build.c` (letters, digits and `+-._@`). Use the upstream
   name in lower case, or the FreeLFS name for a FreeLFS program (`fl*`).
2. **Find the source.** Note the version and its download URL. Prefer an HTTPS URL on the project's own site or a
   release archive. Download it once and take the SHA256: `sha256sum FILE`. Upstream checksum lists (for example
   kernel.org's) are better when they exist.
3. **Copy a similar recipe** into `recipes/NAME/` and change the fields. Find one with the same build system:
   `grep -l "build_style=meson" recipes/*/recipe`.
4. **Fill the fields** (section 2). Set `groups`, which decides whether the image gets the package. Use `base` only
   for what the base system needs; `desktop` for the desktop; leave it empty to build without installing it.
5. **List the dependencies** honestly: `makedepends` for build tools and libraries, `depends` for what the program needs
   when it runs. A missing dependency usually shows up as a failed build in a clean environment.
6. **Write the functions** only where the build style does not do the job.
7. **Add the source to the lock** if it is a new URL: add it to `sources.list`, then run `./build/update-lock.sh`.
   Recipe checksums and the lock must agree.
8. **Build the package alone**, without checks, in the same way as the image build:

   ```sh
   flpkg build --nocheck --sources ~/freelfs-work/cache/sources -o ~/freelfs-work/out recipes/NAME
   ```

   `flpkg build` prints the new package file and its SHA256.
9. **Add it to the image**: run `./build/60-packages.sh NAME` (chapter 20, section 5). It rebuilds the package in the
   chroot, the repository and the root. Check the result with `flpkg --root ROOT list`.
10. **Write a release note** in the commit message (chapter 26) and bump `release` when you change a recipe that already
    exists.

Checklist: name valid, `summary` present, one `sha256` per source, `arch` set, `groups` decided, dependencies listed,
`package()` installs into `$pkgdir` and nothing into `/bin`, `/lib`, `/usr/local` or `/var/run`, and any setuid or
setgid file has an `owner=` line.

## 14. Checks that flpkg makes for you

- A wrong checksum stops the build with both sums printed.
- A `package()` that installs nothing stops the build (`package NAME is empty`).
- A forbidden path (section 1) stops the build, unless `!pathcheck` is set.
- A missing `package()` (or `package_NAME()`) stops the build during validation.
- A source that is not in the cache is downloaded with `curl` (`-fL`, three retries), and renamed into place only when complete.

The test suite of `flpkg` itself (`src/flpkg/tests`, run by `make check`) covers the build, transactions, the repository
and trust. Chapter 25 lists the test targets.
