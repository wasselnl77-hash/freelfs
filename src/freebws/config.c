/* config -- see config.h. SPDX-License-Identifier: MIT */
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *dirs[3];
static GHashTable *kv;

static const char *mkdir_for(int i, const char *env, const char *xdg_env, const char *rel, const char *sub)
{
	if (dirs[i])
		return dirs[i];
	const char *root = getenv("FREEBWS_HOME");
	char *base;
	if (root && *root)
		base = g_build_filename(root, sub, NULL);
	else if (xdg_env && *xdg_env)
		base = g_build_filename(xdg_env, "freebws", NULL);
	else
		base = g_build_filename(g_get_home_dir(), rel, "freebws", NULL);
	g_mkdir_with_parents(base, 0700);
	dirs[i] = base;
	return base;
}

const char *cfg_dir(void) { return mkdir_for(0, NULL, getenv("XDG_CONFIG_HOME"), ".config", "config"); }
const char *data_dir(void) { return mkdir_for(1, NULL, getenv("XDG_DATA_HOME"), ".local/share", "data"); }
const char *cache_dir(void) { return mkdir_for(2, NULL, getenv("XDG_CACHE_HOME"), ".cache", "cache"); }

const char *share_dir(void)
{
	const char *e = getenv("FREEBWS_DATADIR");
	return e && *e ? e : "/usr/share/freebws";
}

int write_file_atomic(const char *path, const char *data, gsize len)
{
	char *tmp = g_strdup_printf("%s.tmp%d", path, (int)getpid());
	FILE *f = fopen(tmp, "w");
	int rc = -1;
	if (f) {
		rc = fwrite(data, 1, len, f) == len ? 0 : -1;
		if (fclose(f))
			rc = -1;
		if (!rc)
			rc = rename(tmp, path);
		if (rc)
			unlink(tmp);
	}
	g_free(tmp);
	return rc;
}

static char *cfg_path(void) { return g_build_filename(cfg_dir(), "config", NULL); }

void cfg_load(void)
{
	if (kv)
		g_hash_table_destroy(kv);
	kv = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	char *p = cfg_path(), *text = NULL;
	if (g_file_get_contents(p, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", 0);
		for (int i = 0; lines[i]; i++) {
			char *l = g_strstrip(lines[i]);
			if (!*l || *l == '#')
				continue;
			char *eq = strchr(l, '=');
			if (!eq)
				continue;
			*eq = 0;
			g_hash_table_replace(kv, g_strdup(g_strstrip(l)), g_strdup(g_strstrip(eq + 1)));
		}
		g_strfreev(lines);
		g_free(text);
	}
	g_free(p);
}

static void cfg_save(void)
{
	GString *o = g_string_new("# FreeBWS settings (written by the browser)\n");
	GList *keys = g_hash_table_get_keys(kv);
	keys = g_list_sort(keys, (GCompareFunc)strcmp);
	for (GList *k = keys; k; k = k->next)
		g_string_append_printf(o, "%s=%s\n", (char *)k->data, (char *)g_hash_table_lookup(kv, k->data));
	g_list_free(keys);
	char *p = cfg_path();
	write_file_atomic(p, o->str, o->len);
	g_free(p);
	g_string_free(o, TRUE);
}

const char *cfg_get(const char *key, const char *def)
{
	if (!kv)
		cfg_load();
	const char *v = g_hash_table_lookup(kv, key);
	return v ? v : def;
}

int cfg_int(const char *key, int def)
{
	const char *v = cfg_get(key, NULL);
	return v && *v ? atoi(v) : def;
}

void cfg_set(const char *key, const char *val)
{
	if (!kv)
		cfg_load();
	g_hash_table_replace(kv, g_strdup(key), g_strdup(val));
	cfg_save();
}

void cfg_set_int(const char *key, int val)
{
	char b[24];
	snprintf(b, sizeof b, "%d", val);
	cfg_set(key, b);
}

gboolean cfg_list_has(const char *key, const char *item)
{
	char **v = g_strsplit(cfg_get(key, ""), ",", 0);
	gboolean has = FALSE;
	for (int i = 0; v[i]; i++)
		if (!strcmp(v[i], item))
			has = TRUE;
	g_strfreev(v);
	return has;
}

void cfg_list_set(const char *key, const char *item, gboolean on)
{
	char **v = g_strsplit(cfg_get(key, ""), ",", 0);
	GString *o = g_string_new(NULL);
	for (int i = 0; v[i]; i++)
		if (*v[i] && strcmp(v[i], item))
			g_string_append_printf(o, "%s%s", o->len ? "," : "", v[i]);
	if (on)
		g_string_append_printf(o, "%s%s", o->len ? "," : "", item);
	g_strfreev(v);
	cfg_set(key, o->str);
	g_string_free(o, TRUE);
}
