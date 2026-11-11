/* Command history: kept in memory, appended to a file as commands run.
 *
 * The file is ~/.local/share/flsh/history ($XDG_DATA_HOME), one command per
 * line with newlines written as \n. Several shells can share it: each only
 * appends, and the file is rewritten (deduplicated) when it grows too big.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HIST_MAX 10000

static struct strlist hist;
static char *hist_path;

static char *escape(const char *s)
{
	struct strbuf sb = {0};
	for (; *s; s++)
		if (*s == '\n')
			sb_addstr(&sb, "\\n");
		else if (*s == '\\')
			sb_addstr(&sb, "\\\\");
		else
			sb_addc(&sb, *s);
	return sb_steal(&sb);
}

static char *unescape_line(const char *s, size_t n)
{
	struct strbuf sb = {0};
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '\\' && i + 1 < n) {
			i++;
			sb_addc(&sb, s[i] == 'n' ? '\n' : s[i]);
		} else
			sb_addc(&sb, s[i]);
	}
	return sb_steal(&sb);
}

/* Drop an older copy of line, so each command appears once (newest wins) */
static void remove_dup(const char *line)
{
	for (size_t i = hist.n; i-- > 0;)
		if (!strcmp(hist.v[i], line)) {
			free(hist.v[i]);
			memmove(hist.v + i, hist.v + i + 1, (hist.n - i) * sizeof(char *));
			hist.n--;
			return;
		}
}

static void mkdirs(const char *path)
{
	char *p = xstrdup(path);
	for (char *s = p + 1; *s; s++)
		if (*s == '/') {
			*s = '\0';
			mkdir(p, 0700);
			*s = '/';
		}
	free(p);
}

static void rewrite_file(void)
{
	char *tmp = xasprintf("%s.%ld.tmp", hist_path, (long)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		free(tmp);
		return;
	}
	struct strbuf sb = {0};
	for (size_t i = 0; i < hist.n; i++) {
		char *e = escape(hist.v[i]);
		sb_addstr(&sb, e);
		sb_addc(&sb, '\n');
		free(e);
	}
	bool ok = write_all(fd, sb.buf ? sb.buf : "", sb.len) == 0;
	close(fd);
	sb_free(&sb);
	if (!ok || rename(tmp, hist_path) < 0)
		unlink(tmp);
	free(tmp);
}

void hist_init(void)
{
	const char *data = getenv("XDG_DATA_HOME");
	const char *home = var_get("HOME");
	if (data && *data)
		hist_path = xasprintf("%s/flsh/history", data);
	else if (home && *home)
		hist_path = xasprintf("%s/.local/share/flsh/history", home);
	else
		return;
	size_t len;
	char *text = read_file(hist_path, &len);
	if (!text)
		return;
	size_t lines = 0;
	for (char *p = text, *end = text + len; p < end;) {
		char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
		if (n) {
			char *line = unescape_line(p, n);
			remove_dup(line);
			sl_push(&hist, line);
			lines++;
		}
		p += n + 1;
	}
	free(text);
	if (hist.n > HIST_MAX) {
		size_t drop = hist.n - HIST_MAX;
		for (size_t i = 0; i < drop; i++)
			free(hist.v[i]);
		memmove(hist.v, hist.v + drop, (hist.n - drop + 1) * sizeof(char *));
		hist.n -= drop;
	}
	if (lines > hist.n + HIST_MAX / 2)
		rewrite_file(); /* many duplicates or old lines: compact the file */
}

void hist_add(const char *line)
{
	if (!*line || line[0] == ' ')
		return; /* a leading space keeps a command out of the history */
	size_t n = strlen(line);
	while (n && (line[n - 1] == '\n' || line[n - 1] == ' '))
		n--;
	if (!n)
		return;
	char *l = xstrndup(line, n);
	remove_dup(l);
	sl_push(&hist, l);
	if (!hist_path)
		return;
	mkdirs(hist_path);
	int fd = open(hist_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (fd < 0)
		return;
	char *e = escape(l);
	char *rec = xasprintf("%s\n", e);
	write_all(fd, rec, strlen(rec));
	close(fd);
	free(rec);
	free(e);
}

size_t hist_count(void)
{
	return hist.n;
}

const char *hist_at(size_t i)
{
	return i < hist.n ? hist.v[i] : NULL;
}

/* The newest command that starts with prefix and is longer than it */
const char *hist_suggest(const char *prefix)
{
	size_t n = strlen(prefix);
	if (!n)
		return NULL;
	for (size_t i = hist.n; i-- > 0;)
		if (strlen(hist.v[i]) > n && !strncmp(hist.v[i], prefix, n) && !strchr(hist.v[i] + n, '\n'))
			return hist.v[i];
	return NULL;
}

int bi_history(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "clear")) {
		sl_free(&hist);
		if (hist_path && unlink(hist_path) < 0 && errno != ENOENT) {
			sh_warn("history: %s: %s", hist_path, strerror(errno));
			return 1;
		}
		return 0;
	}
	const char *search = NULL;
	size_t first = 0;
	if (argc > 2 && !strcmp(argv[1], "search"))
		search = argv[2];
	else if (argc > 1) {
		long n;
		if (str_to_int(argv[1], &n) < 0 || n < 0) {
			sh_warn("history: %s: not a number (try: history 20, history search TEXT, history clear)",
			        argv[1]);
			return 2;
		}
		first = (size_t)n < hist.n ? hist.n - (size_t)n : 0;
	}
	for (size_t i = first; i < hist.n; i++) {
		if (search && !strstr(hist.v[i], search))
			continue;
		char *e = escape(hist.v[i]);
		printf("%5zu  %s\n", i + 1, e);
		free(e);
	}
	return 0;
}
