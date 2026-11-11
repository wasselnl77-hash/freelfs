/* The boot menu's entries: \flboot\flboot.conf, the kernels found on the
 * ESP, and the btrfs snapshots listed in \flboot\snapshots.
 *
 * flboot.conf (all optional):
 *   timeout SECONDS          menu countdown; 0 boots at once, -1 waits
 *   cmdline OPTIONS...       kernel command line for every kernel found
 *   default TEXT             a kernel version or entry title to preselect
 *   entry TITLE|PATH|OPTIONS an extra entry (PATH on the ESP, \ separated)
 * \flboot\snapshots, written by flsnap: "SUBVOLUME TITLE..." per line; each
 * gets an entry (listed by the menu under "Start an earlier state") that boots
 * the newest kernel with that root subvolume.
 * SPDX-License-Identifier: MIT */
#include "flboot.h"

static bool space(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

/* Split TEXT into lines in place; calls FN with each trimmed, non-comment line. */
static void each_line(char *text, void (*fn)(char *line, void *ctx), void *ctx)
{
	char *p = text;
	while (*p) {
		char *line = p;
		while (*p && *p != '\n')
			p++;
		if (*p)
			*p++ = '\0';
		while (space(*line))
			line++;
		size_t n = strlen(line);
		while (n && space(line[n - 1]))
			line[--n] = '\0';
		if (*line && *line != '#')
			fn(line, ctx);
	}
}

static bool starts(const char *s, const char *word)
{
	size_t n = strlen(word);
	return !memcmp(s, word, n) && (space(s[n]) || !s[n]);
}

static const char *arg(const char *s, const char *word)
{
	s += strlen(word);
	while (space(*s))
		s++;
	return s;
}

static int atoi_s(const char *s)
{
	int sign = 1, v = 0;
	if (*s == '-')
		sign = -1, s++;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return sign * v;
}

static struct entry *add(struct config *c)
{
	return c->n < MAX_ENTRIES ? &c->entries[c->n++] : NULL;
}

static void conf_line(char *line, void *ctx)
{
	struct config *c = ctx;
	if (starts(line, "timeout")) {
		c->timeout = atoi_s(arg(line, "timeout"));
	} else if (starts(line, "cmdline")) {
		const char *v = arg(line, "cmdline");
		c->cmdline = ascii_dup(v, strlen(v));
	} else if (starts(line, "default")) {
		const char *v = arg(line, "default");
		c->def = ascii_dup(v, strlen(v));
	} else if (starts(line, "entry")) {
		char *v = (char *)arg(line, "entry"), *f[3] = {v, NULL, NULL};
		for (int i = 1; i < 3; i++) {
			char *bar = f[i - 1];
			while (*bar && *bar != '|')
				bar++;
			if (!*bar)
				break;
			*bar = '\0';
			f[i] = bar + 1;
		}
		struct entry *e = f[1] ? add(c) : NULL;
		if (e) {
			e->title = ascii_dup(f[0], strlen(f[0]));
			e->kernel = ascii_to16(f[1]);
			e->cmdline = f[2] ? ascii_dup(f[2], strlen(f[2])) : NULL;
		}
	}
}

/* ---- kernels on the ESP ----------------------------------------------------- */

struct kernels {
	char *version[MAX_ENTRIES];
	int n;
};

static void kernel_file(const CHAR16 *name, void *ctx)
{
	struct kernels *k = ctx;
	static const CHAR16 prefix[] = L"vmlinuz-";
	size_t n = wcslen16(name), p = sizeof(prefix) / sizeof(CHAR16) - 1;
	if (n <= p || memcmp(name, prefix, p * sizeof(CHAR16)) || k->n == MAX_ENTRIES)
		return;
	char *v = xalloc(n - p + 1);
	for (size_t i = p; i < n; i++)
		v[i - p] = name[i] < 128 ? (char)name[i] : '?';
	k->version[k->n++] = v;
}

static char *join(const char *a, const char *b, const char *c)
{
	size_t na = strlen(a), nb = strlen(b), nc = strlen(c);
	char *s = xalloc(na + nb + nc + 1);
	memcpy(s, a, na);
	memcpy(s + na, b, nb);
	memcpy(s + na + nb, c, nc);
	return s;
}

/* The command line with "rootflags=..." replaced by "rootflags=subvol=SUBVOL". */
static char *with_subvol(const char *cmdline, const char *subvol)
{
	size_t n = strlen(cmdline);
	char *out = xalloc(n + strlen(subvol) + 32), *o = out;
	const char *p = cmdline;
	while (*p) {
		while (space(*p))
			p++;
		const char *w = p;
		while (*p && !space(*p))
			p++;
		if (p - w >= 10 && !memcmp(w, "rootflags=", 10))
			continue;
		if (p > w) {
			memcpy(o, w, (size_t)(p - w));
			o += p - w;
			*o++ = ' ';
		}
	}
	memcpy(o, "rootflags=subvol=", 17);
	memcpy(o + 17, subvol, strlen(subvol));
	return out;
}

struct snap_ctx {
	struct config *c;
	const char *kernel_version;
};

static void snapshot_line(char *line, void *ctx)
{
	struct snap_ctx *s = ctx;
	char *t = line;
	while (*t && !space(*t))
		t++;
	char *title = *t ? t + 1 : t;
	*t = '\0';
	while (space(*title))
		title++;
	struct entry *e = add(s->c);
	if (!e)
		return;
	/* "@snapshots/7" and "DATE  REASON" make "#7  DATE  REASON" */
	const char *num = line, *slash = line;
	for (; *num; num++)
		if (*num == '/')
			slash = num + 1;
	e->title = join(join("#", slash, "  "), *title ? title : line, "");
	e->snapshot = true;
	e->kernel = ascii_to16(join("\\vmlinuz-", s->kernel_version, ""));
	e->cmdline = with_subvol(s->c->cmdline ? s->c->cmdline : "", line);
	e->version = ascii_dup(s->kernel_version, strlen(s->kernel_version));
}

static void reverse(struct entry *v, int from, int to)
{
	for (to--; from < to; from++, to--) {
		struct entry t = v[from];
		v[from] = v[to];
		v[to] = t;
	}
}

void conf_load(struct config *c)
{
	memset(c, 0, sizeof(*c));
	c->timeout = 5;
	char *text;
	if (!EFI_ERROR(fs_read(L"\\flboot\\flboot.conf", &text, NULL)))
		each_line(text, conf_line, c);

	/* every kernel on the ESP, newest first */
	struct kernels k = {0};
	fs_list(L"\\", kernel_file, &k);
	for (int i = 1; i < k.n; i++)
		for (int j = i; j > 0 && vercmp(k.version[j - 1], k.version[j]) < 0; j--) {
			char *t = k.version[j];
			k.version[j] = k.version[j - 1];
			k.version[j - 1] = t;
		}
	int first = c->n;
	for (int i = 0; i < k.n; i++) {
		struct entry *e = add(c);
		if (!e)
			break;
		e->title = join("FreeLFS", i ? " (older kernel " : " (kernel ", join(k.version[i], ")", ""));
		e->kernel = ascii_to16(join("\\vmlinuz-", k.version[i], ""));
		e->version = k.version[i];
	}
	/* the found kernels go before the manual entries: rotate [0, n) left by first */
	reverse(c->entries, 0, first);
	reverse(c->entries, first, c->n);
	reverse(c->entries, 0, c->n);

	if (k.n && !EFI_ERROR(fs_read(L"\\flboot\\snapshots", &text, NULL))) {
		struct snap_ctx s = {c, k.version[0]};
		each_line(text, snapshot_line, &s);
	}
	for (int i = 0; i < c->n; i++)
		if (!c->entries[i].cmdline)
			c->entries[i].cmdline = c->cmdline ? c->cmdline : "";
}
