/* common.h -- what the installer window and its helper agree on
 * SPDX-License-Identifier: MIT
 *
 * Sizes, the rules for the first account's name, and the reserved-name list
 * both sides read. The helper checks everything again; the window uses the
 * same functions so what it accepts the helper accepts.
 */
#ifndef FLINSTALL_COMMON_H
#define FLINSTALL_COMMON_H

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FI_MIB (1024LL * 1024)
#define FI_GIB (1024LL * FI_MIB)
#define FI_ESP_BYTES (512 * FI_MIB)
#define FI_MIN_BOOT_BYTES (200 * FI_MIB)  /* manual: smallest /boot */
#define FI_MIN_ROOT_BYTES (6 * FI_GIB)    /* the system is about 3 GB; leave room */
/* The one-line switch for the administrator (root) account: 1 = root gets the first user's
 * password, 0 = root is locked (shadow field "!": no login as root; administration then goes
 * through the account's own groups and the setuid helpers). The window's wording follows. */
#define FI_ROOT_SAME_PASSWORD 1
#if FI_ROOT_SAME_PASSWORD
#define FI_ROOT_NOTE "The administrator account (root) uses the same password."
#else
#define FI_ROOT_NOTE "The administrator account (root) is locked: nobody can log in as root."
#endif

#define FI_PW_MIN 4
#define FI_PW_MAX 128

#define FI_RESERVED_MAX 512
struct fi_reserved {
	int n;
	char name[FI_RESERVED_MAX][36];
};

static inline void fi_reserved_add(struct fi_reserved *r, const char *n)
{
	if (r->n < FI_RESERVED_MAX && n[0] && strlen(n) < sizeof r->name[0])
		strcpy(r->name[r->n++], n);
}

/* names in ROOT/etc/passwd and ROOT/etc/group (first field) and the entries of ROOT/usr;
 * paths are given in full by the caller (root may reach them through links) */
static inline void fi_reserved_file(struct fi_reserved *r, const char *path)
{
	FILE *f = fopen(path, "r");
	char line[512];
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *c = strchr(line, ':');
		if (c) {
			*c = 0;
			fi_reserved_add(r, line);
		}
	}
	fclose(f);
}

static inline void fi_reserved_dir(struct fi_reserved *r, const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;
	if (!d)
		return;
	while ((e = readdir(d)))
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
			fi_reserved_add(r, e->d_name);
	closedir(d);
}

/* NULL when NAME is fine, else a sentence for the person. The live user "tester" is not
 * reserved: it is not on the installed system. */
static inline const char *fi_user_error(const char *name, const struct fi_reserved *r, char *buf, size_t cap)
{
	size_t n = strlen(name);
	if (!n)
		return "Pick a name.";
	if (n > 32)
		return "At most 32 characters.";
	if (!(name[0] >= 'a' && name[0] <= 'z') && name[0] != '_')
		return "Start with a lowercase letter.";
	for (size_t i = 0; i < n; i++) {
		char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
			return "Use only lowercase letters, digits, - and _.";
	}
	if (name[0] == '_' && n == 1)
		return "Pick a longer name.";
	if (r)
		for (int i = 0; i < r->n; i++)
			if (!strcmp(name, r->name[i]) && strcmp(name, "tester")) {
				snprintf(buf, cap, "\"%s\" is already used by the system. Pick another name.", name);
				return buf;
			}
	return NULL;
}

static inline const char *fi_password_error(const char *pw)
{
	size_t n = strlen(pw);
	if (n < FI_PW_MIN)
		return "At least 4 characters.";
	if (n > FI_PW_MAX)
		return "Too long.";
	return NULL;
}

/* the keyboard ids flui knows, and locales the installer offers */
static inline int fi_keyboard_ok(const char *id)
{
	return !strcmp(id, "de") || !strcmp(id, "us") || !strcmp(id, "uk") || !strcmp(id, "fr");
}

static inline int fi_locale_ok(const char *l)
{
	/* aa_BB.UTF-8 or aaa_BB.UTF-8 */
	size_t i = 0;
	while (l[i] >= 'a' && l[i] <= 'z')
		i++;
	if (i < 2 || i > 3 || l[i] != '_')
		return 0;
	i++;
	if (!(l[i] >= 'A' && l[i] <= 'Z') || !(l[i + 1] >= 'A' && l[i + 1] <= 'Z'))
		return 0;
	return !strcmp(l + i + 2, ".UTF-8");
}

/* a zone name as fltime-set accepts it: letters, digits, _ + - . and /, no empty or dotted part */
static inline int fi_zone_ok(const char *z)
{
	size_t n = strlen(z);
	if (!n || n > 48 || z[0] == '/' || z[n - 1] == '/')
		return 0;
	if (!strncmp(z, "posix/", 6) || !strncmp(z, "right/", 6))
		return 0;
	int part = 0;
	for (size_t i = 0; i < n; i++) {
		char c = z[i];
		if (c == '/') {
			if (!part)
				return 0;
			part = 0;
			continue;
		}
		if (!part && c == '.')
			return 0;
		if (!isalnum((unsigned char)c) && c != '_' && c != '+' && c != '-' && c != '.')
			return 0;
		part++;
	}
	return part > 0;
}

#endif
