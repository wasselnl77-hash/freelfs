/* flauth-check.h -- the password policy of the setuid helpers (flauth, flpkg-seat, flsplash-set, fltime-set,
 * flmount, flpersist, flsnap, flinstall-helper). Vendored: the same file is in each component that needs it
 * (keep the copies identical; flauth installs one to /usr/include).
 *
 * A root action of some CLASS is allowed to the real user UID when one of these holds:
 *   - UID is root;
 *   - the account has an empty password AND this is the live system (/run/live exists) AND the account is
 *     "tester" (the live session's passwordless tester). An empty password anywhere else never passes;
 *   - flauth stamped UID for CLASS less than FLAUTH_VALID_S seconds ago, and the stamp was made for the same
 *     executable that now calls the helper (the process that started flauth, which is the process that started
 *     the helper, resolved through /proc/PPID/exe). A stamp of a one-shot CLASS is used up by the check that passes.
 *
 * Classes (flauth_class): pkg, time, splash, mount-internal are reusable for 120 s by the same caller;
 * disk-write, persist-create, snap-rollback and snap-delete are one-shot: each needs a new password.
 *
 * Stamp: /run/flauth/ok-UID.CLASS, three lines: CLASS, EXE, TIME (epoch seconds). Owned by the effective uid
 * (root when installed), mode 0600, one link, in a directory owned by the effective uid with mode 0700.
 *
 * Tests (only for a process that is not setuid root, i.e. geteuid() != 0): FLAUTH_DIR, FLAUTH_SHADOW,
 * FLAUTH_LIVE (the live marker path), FLAUTH_USER (the account name). A setuid copy ignores all of them.
 * SPDX-License-Identifier: MIT */
#ifndef FLAUTH_CHECK_H
#define FLAUTH_CHECK_H
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef FLAUTH_DIR
#define FLAUTH_DIR "/run/flauth"
#endif
#ifndef FLAUTH_LIVE_MARK
#define FLAUTH_LIVE_MARK "/run/live"
#endif
#ifndef FLAUTH_BIN
#define FLAUTH_BIN "/usr/bin/flauth"
#endif
#define FLAUTH_VALID_S 120
#define FLAUTH_LIVE_USER "tester"

/* a test hook's value, only when this process is not setuid root */
static inline const char *flauth_env(const char *name, const char *dflt)
{
	const char *e = geteuid() != 0 ? getenv(name) : NULL;
	return e && *e ? e : dflt;
}

static inline const char *flauth_dir(void)
{
	return flauth_env("FLAUTH_DIR", FLAUTH_DIR);
}

/* 1 for a known class (oneshot gets 1 for the one-shot ones, 0 for the reusable ones), 0 for anything else */
static inline int flauth_class(const char *cls, int *oneshot)
{
	static const struct {
		const char *name;
		int oneshot;
	} tab[] = {
		{"pkg", 0},          {"time", 0},         {"splash", 0},       {"mount-internal", 0},
		{"disk-write", 1},   {"persist-create", 1}, {"snap-rollback", 1}, {"snap-delete", 1},
	};
	for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
		if (!strcmp(cls, tab[i].name)) {
			if (oneshot)
				*oneshot = tab[i].oneshot;
			return 1;
		}
	return 0;
}

/* the account name of UID; 0 on success */
static inline int flauth_user(uid_t uid, char *name, size_t cap)
{
	const char *alt = geteuid() != 0 ? getenv("FLAUTH_USER") : NULL;
	if (alt && *alt) {
		snprintf(name, cap, "%s", alt);
		return 0;
	}
	struct passwd *pw = getpwuid(uid);
	if (!pw)
		return -1;
	snprintf(name, cap, "%s", pw->pw_name);
	return 0;
}

/* 1 when the account's shadow entry exists and has an empty password */
static inline int flauth_empty_password(const char *name)
{
	const char *alt = geteuid() != 0 ? getenv("FLAUTH_SHADOW") : NULL;
	if (!alt) {
		struct spwd *sp = getspnam(name);
		return sp && sp->sp_pwdp[0] == 0;
	}
	FILE *f = fopen(alt, "r");
	if (!f)
		return 0;
	int empty = 0;
	struct spwd *sp;
	while ((sp = fgetspent(f)))
		if (!strcmp(sp->sp_namp, name)) {
			empty = sp->sp_pwdp[0] == 0;
			break;
		}
	fclose(f);
	return empty;
}

/* 1 when the live marker exists: this is the live session, not an installed system */
static inline int flauth_live(void)
{
	return access(flauth_env("FLAUTH_LIVE", FLAUTH_LIVE_MARK), F_OK) == 0;
}

/* the resolved executable of the process that started this one (the helper's caller, or flauth's) */
static inline int flauth_caller_exe(char *buf, size_t cap)
{
	char p[64];
	snprintf(p, sizeof p, "/proc/%ld/exe", (long)getppid());
	ssize_t n = readlink(p, buf, cap - 1);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return 0;
}

static inline int flauth_stamp_path(uid_t uid, const char *cls, char *p, size_t cap)
{
	int n = snprintf(p, cap, "%s/ok-%u.%s", flauth_dir(), (unsigned)uid, cls);
	return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* 1 when the stamp of UID for CLS is fresh and was made for EXE; a one-shot stamp is removed here */
static inline int flauth_stamp_ok(uid_t uid, const char *cls, int oneshot, const char *exe)
{
	char p[PATH_MAX + 64];
	if (flauth_stamp_path(uid, cls, p, sizeof p) < 0)
		return 0;
	struct stat st;
	if (lstat(flauth_dir(), &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077))
		return 0;
	if (lstat(p, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) || st.st_nlink != 1)
		return 0;
	int fd = open(p, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return 0;
	char buf[PATH_MAX + 128];
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	char *l1 = buf, *l2 = strchr(l1, '\n');
	if (!l2)
		return 0;
	*l2++ = 0;
	char *l3 = strchr(l2, '\n');
	if (!l3)
		return 0;
	*l3++ = 0;
	char *end = NULL;
	long long t = strtoll(l3, &end, 10);
	if (end == l3 || *end != '\n')
		return 0;
	time_t now = time(NULL);
	if (strcmp(l1, cls) || strcmp(l2, exe) || t > (long long)now + 2 || (long long)now - t > FLAUTH_VALID_S)
		return 0;
	if (oneshot)
		return unlink(p) == 0; /* used up: a second check of the same stamp fails */
	return 1;
}

/* 1 when the process whose real user is UID may do the root action of CLS now (see the top of this file) */
static inline int flauth_allowed(uid_t uid, const char *cls)
{
	if (uid == 0)
		return 1;
	int oneshot = 0;
	if (!flauth_class(cls, &oneshot))
		return 0;
	char name[64], exe[PATH_MAX];
	if (flauth_user(uid, name, sizeof name) < 0)
		return 0;
	if (flauth_empty_password(name))
		return flauth_live() && !strcmp(name, FLAUTH_LIVE_USER);
	if (flauth_caller_exe(exe, sizeof exe) < 0)
		return 0;
	return flauth_stamp_ok(uid, cls, oneshot, exe);
}

/* A helper that asks for the password itself. 1: allowed now. Otherwise, when this is not already the second
 * run (TRIED: FLAUTH_TRIED was set when the helper started, read before the environment was cleared), it starts
 * flauth as the person with CLS and the helper's own arguments, and does not return. 0: that could not be done;
 * the caller says the password is needed and exits 77. SELF is the helper's installed path. */
static inline int flauth_gate(int tried, const char *cls, const char *reason, const char *self, int argc, char **argv)
{
	uid_t u = getuid();
	gid_t g = getgid();
	if (flauth_allowed(u, cls))
		return 1;
	if (tried || argc < 1 || argc > 40 || setresgid(g, g, g) < 0 || setresuid(u, u, u) < 0)
		return 0;
	setenv("FLAUTH_TRIED", "1", 1);
	char *args[48];
	int k = 0;
	args[k++] = (char *)"flauth";
	args[k++] = (char *)"--class";
	args[k++] = (char *)cls;
	args[k++] = (char *)"--reason";
	args[k++] = (char *)reason;
	args[k++] = (char *)"--";
	args[k++] = (char *)self;
	for (int i = 1; i < argc; i++)
		args[k++] = argv[i];
	args[k] = NULL;
	execv(FLAUTH_BIN, args);
	return 0;
}

/* flauth's stamp for CLS and UID, written by flauth (root) after the password was right: tmp file, then rename */
static inline int flauth_write_stamp(uid_t uid, const char *cls, const char *exe)
{
	char p[PATH_MAX + 64], tmp[PATH_MAX + 96];
	if (flauth_stamp_path(uid, cls, p, sizeof p) < 0)
		return -1;
	snprintf(tmp, sizeof tmp, "%s.new.%ld", p, (long)getpid());
	unlink(tmp);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	dprintf(fd, "%s\n%s\n%lld\n", cls, exe, (long long)time(NULL));
	int ok = fsync(fd) == 0;
	if (close(fd) < 0)
		ok = 0;
	if (!ok || rename(tmp, p) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}
#endif
