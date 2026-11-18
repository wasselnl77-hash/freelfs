# FreeLFS shared build settings. Sourced by every build stage, never executed.
# Override any value from the environment, e.g. FLFS_ARCH=aarch64 ./build/...
#
# The whole build is rootless: the target root is a plain directory owned by
# the building user, the chroot stages run in a user namespace, and the
# bootable disk image is assembled from that directory at the end.

FLFS_VERSION=0.1-dev

# Target
FLFS_ARCH=${FLFS_ARCH:-x86_64}
FLFS_TGT=${FLFS_ARCH}-freelfs-linux-gnu

# Work area on the host
FLFS_WORK=${FLFS_WORK:-$HOME/freelfs-work}
FLFS_CACHE=${FLFS_CACHE:-$FLFS_WORK/cache/sources}
FLFS_LOGS=$FLFS_WORK/logs

# Target root directory (what LFS calls $LFS). Inside the chroot it is /,
# and logs go to a bind mount of the host's log directory.
if [ -n "${FLFS_IN_CHROOT:-}" ]; then
	FLFS=
	FLFS_LOGS=/.bootstrap/logs
else
	FLFS=${FLFS:-$FLFS_WORK/rootfs-$FLFS_ARCH}
fi
FLFS_SOURCES=$FLFS/sources
FLFS_TOOLS=$FLFS/tools
FLFS_BOOTSTRAP=$FLFS/.bootstrap        # stamps + scratch build dir; removed after M2
FLFS_STAMPS=$FLFS_BOOTSTRAP/stamps
FLFS_BUILD=$FLFS_BOOTSTRAP/build

# Bootable disk image (GPT: ESP + btrfs), assembled from the root that
# 60-packages.sh installs from packages
FLFS_IMAGE_ROOT=${FLFS_IMAGE_ROOT:-$FLFS/.bootstrap/pkg/root}
FLFS_IMG=${FLFS_IMG:-$FLFS_WORK/freelfs-$FLFS_ARCH.img}
FLFS_IMG_SIZE=${FLFS_IMG_SIZE:-80G}

# Package test suites: none, core (glibc, binutils, gmp, mpfr, mpc) or all
FLFS_CHECK=${FLFS_CHECK:-core}

# Build environment
# Parallel jobs come in two classes. flpkg picks one per package from the
# recipe's jobs= line (jobs=heavy, otherwise light):
#   heavy  C++ giants (llvm, gcc, mesa, wpewebkit, icu, spirv-llvm-translator),
#          about 1 GB per job: one per GB of memory left after keeping 4 GB
#          for the desktop (an integrated GPU's video memory comes out of the
#          same RAM), at most one per CPU.
#   light  everything else, the kernel included, about 0.4 GB per job: one per
#          400 MB left after keeping 2 GB for the desktop, zram swap counted at
#          half its size as extra memory, at most one per CPU.
# FLFS_JOBS_HEAVY and FLFS_JOBS_LIGHT override one class. FLFS_JOBS sets both
# classes (unless the class is set too) and the general value below.
# The stage scripts and chroot use FLFS_JOBS, which is the heavy value.
flfs_ncpu=$(nproc)
flfs_mem_mb=$(awk '/^MemTotal:/ {print int($2 / 1024)}' /proc/meminfo)
flfs_zram_mb=$(awk '/\/dev\/zram/ {s += $3} END {print int(s / 2048)}' /proc/swaps)
flfs_cap() { # flfs_cap N -- N, at least 1 and at most the CPU count
	local n=$1
	[ "$n" -ge 1 ] 2> /dev/null || n=1
	[ "$n" -le "$flfs_ncpu" ] || n=$flfs_ncpu
	echo "$n"
}
flfs_heavy_auto=$(flfs_cap "$(awk '/^MemTotal:/ {print int(($2 / 1024 - 4096) / 1024)}' /proc/meminfo)")
flfs_light_auto=$(flfs_cap $(( (flfs_mem_mb + flfs_zram_mb - 2048) / 400 )))
# below 12 GB RAM keep headroom for the desktop and agents: at most 6 light jobs
[ "$flfs_mem_mb" -ge 12288 ] || [ "$flfs_light_auto" -le 6 ] || flfs_light_auto=6
FLFS_JOBS_HEAVY=${FLFS_JOBS_HEAVY:-${FLFS_JOBS:-$flfs_heavy_auto}}
FLFS_JOBS_LIGHT=${FLFS_JOBS_LIGHT:-${FLFS_JOBS:-$flfs_light_auto}}
FLFS_JOBS=${FLFS_JOBS:-$FLFS_JOBS_HEAVY}
export FLFS_JOBS FLFS_JOBS_HEAVY FLFS_JOBS_LIGHT
export MAKEFLAGS="-j$FLFS_JOBS" NINJAJOBS=$FLFS_JOBS TESTSUITEFLAGS="-j$FLFS_JOBS"
export LC_ALL=POSIX
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1767225600}  # 2026-01-01, for reproducible builds

export FLFS FLFS_TGT FLFS_ARCH
