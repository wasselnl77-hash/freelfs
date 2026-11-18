/* flauth -- ask the person for her password before a root action.
 *
 *   flauth --class CLASS [--reason TEXT] [-- CMD ARGS...]
 *
 * Installed setuid root for the group seat (mode 4750), PAM-free: the password is checked against /etc/shadow with
 * crypt(). The window is flauth-dialog, run as the person (never as root); the password comes back through a pipe.
 * CLASS says which root action the stamp is for (flauth-check.h: pkg, time, splash, mount-internal are reusable
 * for 120 s by the same calling program; disk-write, persist-create, snap-rollback, snap-delete are one-shot).
 * On success flauth writes the stamp /run/flauth/ok-UID.CLASS, bound to the program that started flauth (its
 * parent, resolved through /proc), and, with a CMD, runs it as the person. The live session's tester (empty
 * password, only on the live system) needs no dialog. An empty password anywhere else is refused: set one first.
 * Exit codes: 0 right (or CMD ran), 1 wrong password three times, 2 cancelled, 3 locked out, broken or misused,
 * 4 no password can unlock this account (none set, locked or expired).
 * Rate limit: three wrong passwords in a row lock the account out of flauth for 30 s, then 60 s ... up to 10 minutes;
 * every wrong password also costs a second, and checks take turns on a lock file.
 * Tests (not as root): FLAUTH_SHADOW=FILE, FLAUTH_DIR=DIR, FLAUTH_DIALOG=PROGRAM, FLAUTH_LIVE=PATH, FLAUTH_USER=NAME
 * replace /etc/shadow, /run/flauth, /usr/libexec/flauth-dialog, /run/live and the account name; a privileged copy
 * ignores them.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "flauth-check.h"

#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>

#define MAXPW 512

static int ct_equal(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	unsigned char d = la != lb;
	for (size_t i = 0; i < la; i++)
		d |= (unsigned char)a[i] ^ (unsigned char)b[i % (lb ? lb : 1)];
	return d == 0;
}

static struct spwd *find_spwd(const char *name)
{
	const char *alt = geteuid() != 0 ? getenv("FLAUTH_SHADOW") : NULL;
	if (!alt)
		return getspnam(name);
	FILE *f = fopen(alt, "r");
	if (!f)
		return NULL;
	struct spwd *sp;
	while ((sp = fgetspent(f)))
		if (!strcmp(sp->sp_namp, name))
			break;
	return sp;
}

static int make_dir(void)
{
	const char *d = flauth_dir();
	if (mkdir(d, 0700) < 0 && errno != EEXIST)
		return -1;
	struct stat st;
	if (lstat(d, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077))
		return -1;
	return 0;
}

/* failures so far and when the last one was; the lock-out in seconds still to wait (0: none) */
static long lock_left(uid_t uid, int *n_out)
{
	char p[300];
	snprintf(p, sizeof p, "%s/fail-%u", flauth_dir(), (unsigned)uid);
	FILE *f = fopen(p, "r");
	int n = 0;
	long t = 0;
	if (f) {
		if (fscanf(f, "%d %ld", &n, &t) != 2)
			n = 0;
		fclose(f);
	}
	if (n_out)
		*n_out = n;
	if (n < 3)
		return 0;
	long wait = 30L << (n - 3 > 5 ? 5 : n - 3);
	if (wait > 600)
		wait = 600;
	long left = t + wait - (long)time(NULL);
	return left > 0 ? left : 0;
}

static void set_failures(uid_t uid, int n)
{
	char p[300];
	snprintf(p, sizeof p, "%s/fail-%u", flauth_dir(), (unsigned)uid);
	if (n == 0) {
		unlink(p);
		return;
	}
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd >= 0) {
		dprintf(fd, "%d %ld\n", n, (long)time(NULL));
		close(fd);
	}
}

/* run the dialog as the person; the password line comes back in buf. 0 got one, 2 cancelled, 3 broken */
static int ask(const char *reason, const char *error, const char *user, char *buf, size_t cap)
{
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0)
		return 3;
	uid_t ru = getuid();
	gid_t rg = getgid();
	pid_t pid = fork();
	if (pid < 0)
		return 3;
	if (pid == 0) {
		if (setresgid(rg, rg, rg) < 0 || setresuid(ru, ru, ru) < 0)
			_exit(126);
		prctl(PR_SET_DUMPABLE, 1);
		dup2(p[1], 1);
		const char *prog = geteuid() != 0 && getenv("FLAUTH_DIALOG") ? getenv("FLAUTH_DIALOG") : "/usr/libexec/flauth-dialog";
		execl(prog, "flauth-dialog", "--reason", reason, "--error", error, "--user", user, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	size_t n = 0;
	for (;;) {
		char c;
		ssize_t r = read(p[0], &c, 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0 || c == '\n')
			break;
		if (n + 1 >= cap) {
			n = 0;
			break;
		}
		buf[n++] = c;
	}
	buf[n] = 0;
	close(p[0]);
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 127)
		return 3;
	return n && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : 2;
}

static void usage(void)
{
	fputs("usage: flauth --class CLASS [--reason TEXT] [-- CMD ARGS...]\n"
	      "CLASS: pkg, time, splash, mount-internal (reusable 2 min); disk-write, persist-create,\n"
	      "       snap-rollback, snap-delete (one-shot)\n", stderr);
}

int main(int argc, char **argv)
{
	struct rlimit z = {0, 0};
	setrlimit(RLIMIT_CORE, &z);
	prctl(PR_SET_DUMPABLE, 0);
	/* the caller's descriptors must not reach the dialog or the command */
	for (int fd = 3; fd < 256; fd++)
		close(fd);
	const char *reason = "This needs your permission.";
	const char *cls = NULL;
	int a = 1;
	while (a < argc && argv[a][0] == '-') {
		if (!strcmp(argv[a], "--")) {
			a++;
			break;
		}
		if (!strcmp(argv[a], "--reason") && a + 1 < argc) {
			reason = argv[a + 1];
			a += 2;
		} else if (!strcmp(argv[a], "--class") && a + 1 < argc) {
			cls = argv[a + 1];
			a += 2;
		} else {
			usage();
			return 3;
		}
	}
	if (!cls || !flauth_class(cls, NULL)) {
		usage();
		return 3;
	}
	uid_t uid = getuid();
	char name[64];
	if (flauth_user(uid, name, sizeof name) < 0) {
		fputs("flauth: unknown user\n", stderr);
		return 3;
	}
	if (geteuid() != 0 && !getenv("FLAUTH_DIR")) {
		fputs("flauth: not installed setuid root\n", stderr);
		return 3;
	}
	/* which program asked: the stamp is only good for it (read now, while the parent is still the caller) */
	char exe[PATH_MAX] = "";
	if (uid != 0 && flauth_caller_exe(exe, sizeof exe) < 0) {
		fputs("flauth: cannot tell which program started it\n", stderr);
		return 3;
	}
	int ok = 0;
	if (uid == 0) {
		ok = 1;
	} else if (make_dir() < 0) {
		fputs("flauth: cannot use /run/flauth\n", stderr);
		return 3;
	} else {
		struct spwd *sp = find_spwd(name);
		if (!sp || (sp->sp_expire >= 0 && (long)(time(NULL) / 86400) > sp->sp_expire) ||
		    sp->sp_pwdp[0] == '!' || sp->sp_pwdp[0] == '*') {
			fputs("flauth: no password can unlock this account\n", stderr);
			return 4;
		}
		if (sp->sp_pwdp[0] == 0) {
			/* no password: only the live session's tester, and only on the live system */
			if (!(flauth_live() && !strcmp(name, FLAUTH_LIVE_USER))) {
				fputs("flauth: this account has no password; set one first\n", stderr);
				return 4;
			}
			ok = 1;
		} else {
			char hash[256];
			snprintf(hash, sizeof hash, "%s", sp->sp_pwdp);
			int lk = open(flauth_dir(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			int rc = 1;
			char err[96] = "";
			for (int tries = 0; tries < 3 && !ok; tries++) {
				long left = lock_left(uid, NULL);
				if (left > 0) {
					fprintf(stderr, "flauth: too many wrong passwords; try again in %ld seconds\n", left);
					rc = 3;
					break;
				}
				char pwbuf[MAXPW];
				int r = ask(reason, err, name, pwbuf, sizeof pwbuf);
				if (r != 0) {
					explicit_bzero(pwbuf, sizeof pwbuf);
					rc = r;
					break;
				}
				if (lk >= 0)
					flock(lk, LOCK_EX); /* one check at a time, in all */
				struct crypt_data *cd = calloc(1, sizeof *cd);
				char *h = cd ? crypt_r(pwbuf, hash, cd) : NULL;
				int good = h && h[0] != '*' && ct_equal(h, hash);
				if (cd) {
					explicit_bzero(cd, sizeof *cd);
					free(cd);
				}
				explicit_bzero(pwbuf, sizeof pwbuf);
				int n;
				lock_left(uid, &n);
				if (good) {
					set_failures(uid, 0);
					ok = 1;
				} else {
					set_failures(uid, n + 1);
					sleep(1);
					snprintf(err, sizeof err, "Wrong password.");
					rc = 1;
				}
				if (lk >= 0)
					flock(lk, LOCK_UN);
			}
			if (lk >= 0)
				close(lk);
			if (!ok)
				return rc;
		}
	}
	if (uid != 0 && flauth_write_stamp(uid, cls, exe) < 0) {
		fputs("flauth: cannot write the stamp\n", stderr);
		return 3;
	}
	if (a >= argc)
		return 0;
	/* the command runs as the person; if it is setuid itself (flpkg-seat, flsnap) it finds the stamp */
	gid_t rg = getgid();
	if (setresgid(rg, rg, rg) < 0 || setresuid(uid, uid, uid) < 0)
		return 3;
	execvp(argv[a], argv + a);
	fprintf(stderr, "flauth: %s: %s\n", argv[a], strerror(errno));
	return 127;
}
