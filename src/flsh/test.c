/* The test (and [) builtin.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct t {
	char **v;
	int n, i;
	bool err;
};

static bool texpr(struct t *t);

static void terr(struct t *t, const char *fmt, const char *arg)
{
	if (!t->err)
		sh_warn(fmt, arg);
	t->err = true;
}

static bool int_arg(struct t *t, const char *s, long *out)
{
	while (*s == ' ' || *s == '\t')
		s++;
	char *end;
	long v = strtol(s, &end, 10);
	while (*end == ' ' || *end == '\t')
		end++;
	if (!*s || *end) {
		terr(t, "test: %s: a number is needed", s);
		*out = 0;
		return false;
	}
	*out = v;
	return true;
}

static bool is_unary(const char *s)
{
	return s[0] == '-' && s[1] && !s[2] && strchr("bcdefgGhLknOprsStuwxz", s[1]);
}

static bool is_binary(const char *s)
{
	static const char *const ops[] = {"=", "==", "!=", "<", ">", "-eq", "-ne", "-lt", "-le", "-gt",
	                                  "-ge", "-nt", "-ot", "-ef", NULL};
	for (int i = 0; ops[i]; i++)
		if (!strcmp(s, ops[i]))
			return true;
	return false;
}

static bool unary(struct t *t, char op, const char *a)
{
	struct stat st;
	switch (op) {
	case 'n': return *a != '\0';
	case 'z': return *a == '\0';
	case 't': {
		long fd;
		return int_arg(t, a, &fd) && isatty((int)fd);
	}
	case 'h':
	case 'L': return lstat(a, &st) == 0 && S_ISLNK(st.st_mode);
	case 'r': return access(a, R_OK) == 0;
	case 'w': return access(a, W_OK) == 0;
	case 'x': return access(a, X_OK) == 0;
	}
	if (stat(a, &st) < 0)
		return false;
	switch (op) {
	case 'b': return S_ISBLK(st.st_mode);
	case 'c': return S_ISCHR(st.st_mode);
	case 'd': return S_ISDIR(st.st_mode);
	case 'e': return true;
	case 'f': return S_ISREG(st.st_mode);
	case 'g': return st.st_mode & S_ISGID;
	case 'G': return st.st_gid == getegid();
	case 'k': return st.st_mode & S_ISVTX;
	case 'O': return st.st_uid == geteuid();
	case 'p': return S_ISFIFO(st.st_mode);
	case 's': return st.st_size > 0;
	case 'S': return S_ISSOCK(st.st_mode);
	case 'u': return st.st_mode & S_ISUID;
	}
	return false;
}

static bool newer(const struct stat *a, const struct stat *b)
{
	return a->st_mtim.tv_sec > b->st_mtim.tv_sec ||
	       (a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec > b->st_mtim.tv_nsec);
}

static bool binary(struct t *t, const char *a, const char *op, const char *b)
{
	if (!strcmp(op, "=") || !strcmp(op, "=="))
		return !strcmp(a, b);
	if (!strcmp(op, "!="))
		return strcmp(a, b) != 0;
	if (!strcmp(op, "<"))
		return strcoll(a, b) < 0;
	if (!strcmp(op, ">"))
		return strcoll(a, b) > 0;
	if (op[1] == 'n' || op[1] == 'o' || !strcmp(op, "-ef")) {
		struct stat sa, sb;
		bool ha = stat(a, &sa) == 0, hb = stat(b, &sb) == 0;
		if (!strcmp(op, "-nt"))
			return ha && (!hb || newer(&sa, &sb));
		if (!strcmp(op, "-ot"))
			return hb && (!ha || newer(&sb, &sa));
		if (!strcmp(op, "-ef"))
			return ha && hb && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
	}
	long x, y;
	if (!int_arg(t, a, &x) || !int_arg(t, b, &y))
		return false;
	if (!strcmp(op, "-eq")) return x == y;
	if (!strcmp(op, "-ne")) return x != y;
	if (!strcmp(op, "-lt")) return x < y;
	if (!strcmp(op, "-le")) return x <= y;
	if (!strcmp(op, "-gt")) return x > y;
	return x >= y;
}

static const char *next(struct t *t)
{
	if (t->i >= t->n) {
		terr(t, "test: %s", "argument expected");
		return "";
	}
	return t->v[t->i++];
}

static bool primary(struct t *t)
{
	if (t->i >= t->n) {
		terr(t, "test: %s", "argument expected");
		return false;
	}
	const char *a = t->v[t->i];
	if (!strcmp(a, "!")) {
		t->i++;
		return !primary(t);
	}
	if (!strcmp(a, "(")) {
		t->i++;
		bool r = texpr(t);
		if (t->i >= t->n || strcmp(t->v[t->i], ")"))
			terr(t, "test: %s", "missing )");
		else
			t->i++;
		return r;
	}
	if (t->i + 2 < t->n && is_binary(t->v[t->i + 1])) {
		t->i += 3;
		return binary(t, a, t->v[t->i - 2], t->v[t->i - 1]);
	}
	if (is_unary(a) && t->i + 1 < t->n) {
		t->i++;
		return unary(t, a[1], next(t));
	}
	t->i++;
	return *a != '\0';
}

static bool tand(struct t *t)
{
	bool r = primary(t);
	while (t->i < t->n && !strcmp(t->v[t->i], "-a")) {
		t->i++;
		bool r2 = primary(t);
		r = r && r2;
	}
	return r;
}

static bool texpr(struct t *t)
{
	bool r = tand(t);
	while (t->i < t->n && !strcmp(t->v[t->i], "-o")) {
		t->i++;
		bool r2 = tand(t);
		r = r || r2;
	}
	return r;
}

/* POSIX decides by the number of arguments up to four */
static bool by_count(struct t *t, char **v, int n)
{
	switch (n) {
	case 0: return false;
	case 1: return *v[0] != '\0';
	case 2:
		if (!strcmp(v[0], "!"))
			return *v[1] == '\0';
		if (is_unary(v[0]))
			return unary(t, v[0][1], v[1]);
		break;
	case 3:
		if (is_binary(v[1]))
			return binary(t, v[0], v[1], v[2]);
		if (!strcmp(v[0], "!"))
			return !by_count(t, v + 1, 2);
		if (!strcmp(v[0], "(") && !strcmp(v[2], ")"))
			return *v[1] != '\0';
		break;
	case 4:
		if (!strcmp(v[0], "!"))
			return !by_count(t, v + 1, 3);
		if (!strcmp(v[0], "(") && !strcmp(v[3], ")"))
			return by_count(t, v + 1, 2);
		break;
	}
	t->v = v;
	t->n = n;
	t->i = 0;
	bool r = texpr(t);
	if (t->i < t->n)
		terr(t, "test: %s: unexpected argument", t->v[t->i]);
	return r;
}

int bi_test(int argc, char **argv)
{
	int n = argc - 1;
	if (!strcmp(argv[0], "[")) {
		if (n < 1 || strcmp(argv[argc - 1], "]")) {
			sh_warn("[: missing ]");
			return 2;
		}
		n--;
	}
	struct t t = {0};
	bool r = by_count(&t, argv + 1, n);
	if (t.err)
		return 2;
	return r ? 0 : 1;
}
