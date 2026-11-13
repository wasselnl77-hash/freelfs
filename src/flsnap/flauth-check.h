/* flauth-check.h -- for the setuid helpers that gate a root action behind flauth (flpkg-seat, flsnap).
 * flauth_ok(uid): 1 when this user may do root actions now, i.e. she is root, or has an empty password (the live
 * session's tester), or flauth made her stamp /run/flauth/ok-UID (root owned, mode 600, in a root owned 0700
 * directory) within the last 5 minutes. Call it with euid 0, before anything else is done.
 * SPDX-License-Identifier: MIT */
#ifndef FLAUTH_CHECK_H
#define FLAUTH_CHECK_H
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define FLAUTH_DIR "/run/flauth"
#define FLAUTH_VALID_S 300

static inline int flauth_ok(uid_t uid)
{
	if (uid == 0)
		return 1;
	struct passwd *pw = getpwuid(uid);
	if (!pw)
		return 0;
	struct spwd *sp = getspnam(pw->pw_name);
	if (sp && sp->sp_pwdp[0] == 0)
		return 1;
	struct stat st;
	if (lstat(FLAUTH_DIR, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 077))
		return 0;
	char p[64];
	snprintf(p, sizeof p, FLAUTH_DIR "/ok-%u", (unsigned)uid);
	if (lstat(p, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 077))
		return 0;
	time_t now = time(NULL);
	return st.st_mtime <= now + 2 && now - st.st_mtime <= FLAUTH_VALID_S;
}
#endif
