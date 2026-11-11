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
# Parallel jobs: one per GB of memory left after keeping 4 GB for the desktop
# (an integrated GPU's video memory comes out of the same RAM), at most one
# per CPU. Big C++ builds like LLVM need about 1 GB per job.
if [ -z "${FLFS_JOBS:-}" ]; then
	FLFS_JOBS=$(awk '/^MemTotal:/ {print int(($2 / 1024 - 4096) / 1024)}' /proc/meminfo)
	[ "$FLFS_JOBS" -ge 1 ] 2> /dev/null || FLFS_JOBS=1
	[ "$FLFS_JOBS" -le "$(nproc)" ] || FLFS_JOBS=$(nproc)
fi
export MAKEFLAGS="-j$FLFS_JOBS" NINJAJOBS=$FLFS_JOBS TESTSUITEFLAGS="-j$FLFS_JOBS"
export LC_ALL=POSIX
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1767225600}  # 2026-01-01, for reproducible builds

export FLFS FLFS_TGT FLFS_ARCH
