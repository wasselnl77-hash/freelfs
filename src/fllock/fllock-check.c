/* fllock-check -- the password check of the lock screen, for people who are not root.
 * Installed setuid root and runnable only by the group seat (mode 4750, like flpower and
 * flpkg-seat). It does one thing: read a line from stdin and say whether it is the
 * password of the user who started it.
 *
 *   exit 0   the password is right (or the account has an empty password)
 *   exit 1   wrong password
 *   exit 2   no password can unlock this account (locked or expired, no shadow entry)
 *   exit 3   it was not used properly, or something broke
 *
 * Safety:
 *  - It only ever looks at the shadow entry of the real uid of its caller (getuid), never at
 *    a name it is given: there is no argument and no environment it reads, so it cannot be
 *    used to test the passwords of other accounts.
 *  - Attempts are slowed down for everybody together: they take turns on a lock file in
 *    /run, and a wrong password holds it for a second, so guessing through this program
 *    is one try a second however many copies run.
 *  - The line is read before the lock is taken, so a caller that never writes it cannot
 *    hold others up for longer than its own timeout (fllock closes the pipe).
 *  - No core dump, the password buffer and the crypt state are wiped.
 *
 * Tests only: when not running as root, FLLOCK_SHADOW=FILE reads the entries from a file
 * in shadow format and FLLOCK_LOCKFILE=FILE names the lock file; a privileged copy ignores both.
 * SPDX-License-Identifier: MIT */
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAXPW 512

static int ct_equal(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	unsigned char d = la != lb;
	for (size_t i = 0; i < la; i++)
		d |= (unsigned char)a[i] ^ (unsigned char)b[i % (lb ? lb : 1)];
	return d == 0;
}

/* the shadow entry of NAME: from /etc/shadow, or (not as root, tests) from $FLLOCK_SHADOW */
static struct spwd *find_spwd(const char *name)
{
	const char *alt = geteuid() != 0 ? getenv("FLLOCK_SHADOW") : NULL;
	if (!alt)
		return getspnam(name);
	FILE *f = fopen(alt, "r");
	if (!f)
		return NULL;
	struct spwd *sp;
	while ((sp = fgetspent(f)))
		if (!strcmp(sp->sp_namp, name))
			break;
	/* the entry lives in static memory of fgetspent: keep the file's data valid by not closing it */
	return sp;
}

int main(int argc, char **argv)
{
	if (argc != 1)
		return 3;
	struct rlimit z = {0, 0};
	setrlimit(RLIMIT_CORE, &z);
	prctl(PR_SET_DUMPABLE, 0);

	uid_t uid = getuid();
	struct passwd *pw = getpwuid(uid);
	if (!pw)
		return 3;
	char name[64];
	snprintf(name, sizeof name, "%s", pw->pw_name);

	/* the password: one line from stdin, read before any lock is taken */
	char buf[MAXPW];
	size_t len = 0;
	for (;;) {
		char c;
		ssize_t r = read(0, &c, 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0 || c == '\n')
			break;
		if (len + 1 >= sizeof buf) {
			explicit_bzero(buf, sizeof buf);
			return 1; /* longer than any password we accept */
		}
		buf[len++] = c;
	}
	buf[len] = 0;

	const char *lockfile = geteuid() != 0 && getenv("FLLOCK_LOCKFILE") ? getenv("FLLOCK_LOCKFILE")
	                                                                    : "/run/fllock-check.lock";
	int lk = open(lockfile, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (lk >= 0)
		flock(lk, LOCK_EX);

	int rc = 3;
	struct spwd *sp = find_spwd(name);
	if (!sp) {
		rc = 2;
	} else if (sp->sp_expire >= 0 && (long)(time(NULL) / 86400) > sp->sp_expire) {
		rc = 2;
	} else if (sp->sp_pwdp[0] == 0) {
		rc = 0; /* an empty password: Enter unlocks, as the login screen lets you in */
	} else if (sp->sp_pwdp[0] == '!' || sp->sp_pwdp[0] == '*') {
		rc = 2;
	} else {
		struct crypt_data *cd = calloc(1, sizeof *cd);
		if (cd) {
			char *r = crypt_r(buf, sp->sp_pwdp, cd);
			rc = r && r[0] != '*' && ct_equal(r, sp->sp_pwdp) ? 0 : 1;
			explicit_bzero(cd, sizeof *cd);
			free(cd);
		}
	}
	explicit_bzero(buf, sizeof buf);
	if (rc == 1) {
		struct timespec d = {1, 0};
		nanosleep(&d, NULL); /* still holding the lock: one wrong try a second, in all */
	}
	return rc;
}
