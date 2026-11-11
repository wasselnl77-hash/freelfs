/* System users and groups that packages declare (sysusers=, sysgroups=).
 *
 * They are written straight into the root's /etc files, so this works in an
 * empty root before shadow is installed, and in a user namespace. They are
 * never removed: files owned by them may outlive the package.
 * SPDX-License-Identifier: MIT */
#include "txn.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool has_entry(const char *text, const char *name)
{
	size_t n = strlen(name);
	for (const char *line = text; line && *line; line = strchr(line, '\n')) {
		if (*line == '\n')
			line++;
		if (!strncmp(line, name, n) && line[n] == ':')
			return true;
	}
	return false;
}

/* Append LINE to ROOT/FILE unless NAME is already there. With ONLY_IF_EXISTS,
 * a missing file is left alone (shadow, gshadow). */
static bool add_entry(const char *root, const char *file, const char *name, const char *line,
                      mode_t mode, bool only_if_exists)
{
	char *path = path_join(root, file);
	size_t len = 0;
	char *text = read_file(path, &len);
	if (!text) {
		if (errno != ENOENT)
			die("cannot read %s: %s", path, strerror(errno));
		if (only_if_exists) {
			free(path);
			return false;
		}
		text = xstrdup("");
	}
	bool added = false;
	if (!has_entry(text, name)) {
		struct stat st;
		if (stat(path, &st) == 0)
			mode = st.st_mode & 07777;
		struct strbuf sb = {0};
		sb_add(&sb, text, len);
		if (len && text[len - 1] != '\n')
			sb_addch(&sb, '\n');
		sb_addf(&sb, "%s\n", line);
		char *etc = path_join(root, "etc"), *tmp = xasprintf("%s.flpkg-new", path);
		if (mkdirs(etc, 0755) < 0 || write_file(tmp, sb.buf, sb.len, mode) < 0 ||
		    rename(tmp, path) < 0)
			die("cannot update %s: %s", path, strerror(errno));
		free(etc);
		free(tmp);
		sb_free(&sb);
		added = true;
	}
	free(text);
	free(path);
	return added;
}

void ensure_sysgroups(const char *root, const struct strlist *groups)
{
	for (size_t i = 0; i < groups->n; i++) {
		char name[256];
		unsigned gid;
		if (sscanf(groups->v[i], "%255s %u", name, &gid) != 2)
			die("bad sysgroup '%s' (want 'NAME GID')", groups->v[i]);
		char *line = xasprintf("%s:x:%u:", name, gid);
		if (add_entry(root, "etc/group", name, line, 0644, false))
			msg("added group %s (%u)", name, gid);
		free(line);
		line = xasprintf("%s:!::", name);
		add_entry(root, "etc/gshadow", name, line, 0600, true);
		free(line);
	}
}

void ensure_sysusers(const char *root, const struct strlist *users)
{
	for (size_t i = 0; i < users->n; i++) {
		char name[256], home[1024], shell[1024];
		unsigned uid, gid;
		int used = 0;
		if (sscanf(users->v[i], "%255s %u %u %1023s %1023s %n", name, &uid, &gid, home, shell,
		           &used) != 5)
			die("bad sysuser '%s' (want 'NAME UID GID HOME SHELL COMMENT')", users->v[i]);
		char *comment = xstrdup(used ? users->v[i] + used : "");
		/* drop the quotes the recipe used for a comment with spaces */
		size_t n = strlen(comment);
		if (n >= 2 && (comment[0] == '\'' || comment[0] == '"') && comment[n - 1] == comment[0]) {
			memmove(comment, comment + 1, n - 2);
			comment[n - 2] = '\0';
		}
		comment[strcspn(comment, ":\n")] = '\0';
		char *line = xasprintf("%s:x:%u:%u:%s:%s:%s", name, uid, gid, comment, home, shell);
		if (add_entry(root, "etc/passwd", name, line, 0644, false))
			msg("added user %s (%u)", name, uid);
		free(line);
		line = xasprintf("%s:!:::::::", name);
		add_entry(root, "etc/shadow", name, line, 0600, true);
		free(line);
		free(comment);
	}
}
