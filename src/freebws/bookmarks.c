/* bookmarks -- see bookmarks.h. SPDX-License-Identifier: MIT */
#include "bookmarks.h"

#include <ctype.h>
#include <string.h>

#include "config.h"

unsigned bm_serial = 1;
static GPtrArray *items;	/* struct bm * */
static int next_id = 1;

static char *path(void) { return g_build_filename(data_dir(), "bookmarks.json", NULL); }

static void bm_free(gpointer p)
{
	struct bm *b = p;
	g_free(b->title);
	g_free(b->url);
	g_free(b);
}

static void json_q(GString *o, const char *s)
{
	g_string_append_c(o, '"');
	for (; s && *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
			g_string_append_printf(o, "\\%c", c);
		else if (c < 0x20)
			g_string_append_printf(o, "\\u%04x", c);
		else
			g_string_append_c(o, (char)c);
	}
	g_string_append_c(o, '"');
}

char *bm_json(void)
{
	GString *o = g_string_new("[");
	for (guint i = 0; items && i < items->len; i++) {
		const struct bm *b = g_ptr_array_index(items, i);
		g_string_append_printf(o, "%s{\"id\":%d,\"parent\":%d,\"folder\":%s,\"title\":", i ? "," : "", b->id, b->parent,
				       b->folder ? "true" : "false");
		json_q(o, b->title);
		g_string_append(o, ",\"url\":");
		json_q(o, b->url ? b->url : "");
		g_string_append_c(o, '}');
	}
	g_string_append_c(o, ']');
	return g_string_free(o, FALSE);
}

static void save(void)
{
	bm_serial++;
	char *j = bm_json();
	GString *o = g_string_new(NULL);
	g_string_append_printf(o, "{\"version\":1,\"next\":%d,\"items\":%s}\n", next_id, j);
	char *p = path();
	write_file_atomic(p, o->str, o->len);
	g_free(p);
	g_free(j);
	g_string_free(o, TRUE);
}

/* a tiny reader for exactly what save() writes (and a little tolerance) */
static const char *skip_ws(const char *p)
{
	while (*p && isspace((unsigned char)*p))
		p++;
	return p;
}

static char *read_str(const char **pp)
{
	const char *p = *pp;
	if (*p != '"')
		return NULL;
	p++;
	GString *o = g_string_new(NULL);
	while (*p && *p != '"') {
		if (*p == '\\' && p[1]) {
			p++;
			if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
				char hex[5] = { p[1], p[2], p[3], p[4], 0 };
				g_string_append_unichar(o, (gunichar)strtoul(hex, NULL, 16));
				p += 4;
			} else if (*p == 'n') {
				g_string_append_c(o, '\n');
			} else if (*p == 't') {
				g_string_append_c(o, '\t');
			} else {
				g_string_append_c(o, *p);
			}
			p++;
		} else {
			g_string_append_c(o, *p++);
		}
	}
	if (*p == '"')
		p++;
	*pp = p;
	return g_string_free(o, FALSE);
}

void bm_load(void)
{
	if (items)
		g_ptr_array_free(items, TRUE);
	items = g_ptr_array_new_with_free_func(bm_free);
	next_id = 1;
	char *p = path(), *text = NULL;
	if (!g_file_get_contents(p, &text, NULL, NULL)) {
		g_free(p);
		return;
	}
	g_free(p);
	const char *s = strstr(text, "\"next\"");
	if (s)
		next_id = atoi(strchr(s, ':') + 1);
	s = strstr(text, "\"items\"");
	s = s ? strchr(s, '[') : NULL;
	while (s && (s = strchr(s, '{'))) {
		struct bm *b = g_new0(struct bm, 1);
		const char *e = s + 1;
		b->parent = 0;
		while (*e && *e != '}') {
			e = skip_ws(e);
			if (*e == ',') {
				e++;
				continue;
			}
			char *k = read_str(&e);
			if (!k)
				break;
			e = skip_ws(e);
			if (*e == ':')
				e++;
			e = skip_ws(e);
			if (*e == '"') {
				char *v = read_str(&e);
				if (!strcmp(k, "title"))
					b->title = v;
				else if (!strcmp(k, "url") && *v)
					b->url = v;
				else
					g_free(v);
			} else {
				int num = !strncmp(e, "true", 4) ? 1 : atoi(e);
				if (!strcmp(k, "id"))
					b->id = num;
				else if (!strcmp(k, "parent"))
					b->parent = num;
				else if (!strcmp(k, "folder"))
					b->folder = num;
				while (*e && *e != ',' && *e != '}')
					e++;
			}
			g_free(k);
		}
		if (b->id > 0 && b->title) {
			if (!b->folder && !b->url)
				b->folder = 1;
			g_ptr_array_add(items, b);
			if (b->id >= next_id)
				next_id = b->id + 1;
		} else {
			bm_free(b);
		}
		s = *e ? e + 1 : e;
	}
	g_free(text);
}

guint bm_count(void) { return items ? items->len : 0; }
const struct bm *bm_at(guint i) { return items && i < items->len ? g_ptr_array_index(items, i) : NULL; }

const struct bm *bm_get(int id)
{
	for (guint i = 0; items && i < items->len; i++)
		if (((struct bm *)g_ptr_array_index(items, i))->id == id)
			return g_ptr_array_index(items, i);
	return NULL;
}

const struct bm *bm_find_url(const char *url)
{
	for (guint i = 0; items && url && i < items->len; i++) {
		const struct bm *b = g_ptr_array_index(items, i);
		if (b->url && !strcmp(b->url, url))
			return b;
	}
	return NULL;
}

static int add_item(const char *title, const char *url, int parent, int folder)
{
	if (!items)
		bm_load();
	if (parent && (!bm_get(parent) || !bm_get(parent)->folder))
		parent = 0;
	struct bm *b = g_new0(struct bm, 1);
	b->id = next_id++;
	b->parent = parent;
	b->folder = folder;
	b->title = g_strdup(title && *title ? title : (url ? url : "Folder"));
	b->url = folder ? NULL : g_strdup(url);
	g_ptr_array_add(items, b);
	return b->id;
}

int bm_add(const char *title, const char *url, int parent)
{
	int id = add_item(title, url, parent, 0);
	save();
	return id;
}

int bm_add_folder(const char *title, int parent)
{
	int id = add_item(title, NULL, parent, 1);
	save();
	return id;
}

static void remove_rec(int id)
{
	for (guint i = 0; i < items->len; i++) {
		const struct bm *b = g_ptr_array_index(items, i);
		if (b->parent == id) {
			remove_rec(b->id);
			i = (guint)-1;	/* the array changed: start again */
		}
	}
	for (guint i = 0; i < items->len; i++)
		if (((struct bm *)g_ptr_array_index(items, i))->id == id) {
			g_ptr_array_remove_index(items, i);
			break;
		}
}

void bm_remove(int id)
{
	if (!items || !bm_get(id))
		return;
	remove_rec(id);
	save();
}

void bm_update(int id, const char *title, const char *url)
{
	struct bm *b = (struct bm *)bm_get(id);
	if (!b)
		return;
	if (title && *title) {
		g_free(b->title);
		b->title = g_strdup(title);
	}
	if (url && !b->folder && *url) {
		g_free(b->url);
		b->url = g_strdup(url);
	}
	save();
}

void bm_move(int id, int parent)
{
	struct bm *b = (struct bm *)bm_get(id);
	if (!b || id == parent)
		return;
	if (parent) {	/* not into itself or one of its own folders */
		const struct bm *p = bm_get(parent);
		if (!p || !p->folder)
			return;
		for (const struct bm *q = p; q; q = q->parent ? bm_get(q->parent) : NULL)
			if (q->id == id)
				return;
	}
	b->parent = parent;
	guint idx;
	if (g_ptr_array_find(items, b, &idx)) {	/* to the end: the end of its new folder */
		g_ptr_array_steal_index(items, idx);
		g_ptr_array_add(items, b);
	}
	save();
}

GPtrArray *bm_children(int parent)
{
	GPtrArray *r = g_ptr_array_new();
	for (guint i = 0; items && i < items->len; i++) {
		struct bm *b = g_ptr_array_index(items, i);
		if (b->parent == parent)
			g_ptr_array_add(r, b);
	}
	return r;
}

/* ---- import: Netscape bookmark file ------------------------------------------------- */

static char *unent(const char *s, gsize n)
{
	GString *o = g_string_new(NULL);
	for (gsize i = 0; i < n; i++) {
		if (s[i] == '&') {
			const char *e = memchr(s + i, ';', n - i < 10 ? n - i : 10);
			if (e) {
				gsize l = (gsize)(e - (s + i)) - 1;
				const char *t = s + i + 1;
				if (l == 3 && !strncmp(t, "amp", 3)) {
					g_string_append_c(o, '&');
				} else if (l == 2 && !strncmp(t, "lt", 2)) {
					g_string_append_c(o, '<');
				} else if (l == 2 && !strncmp(t, "gt", 2)) {
					g_string_append_c(o, '>');
				} else if (l == 4 && !strncmp(t, "quot", 4)) {
					g_string_append_c(o, '"');
				} else if (l == 4 && !strncmp(t, "apos", 4)) {
					g_string_append_c(o, '\'');
				} else if (l >= 2 && t[0] == '#') {
					gunichar c = t[1] == 'x' || t[1] == 'X' ? (gunichar)strtoul(t + 2, NULL, 16) : (gunichar)strtoul(t + 1, NULL, 10);
					if (c)
						g_string_append_unichar(o, c);
				} else {
					g_string_append_len(o, s + i, (gssize)(e - (s + i) + 1));
				}
				i = (gsize)(e - s);
				continue;
			}
		}
		g_string_append_c(o, s[i]);
	}
	return g_string_free(o, FALSE);
}

/* the value of attr in a tag's text (between < and >), case-insensitive; NULL if absent */
static char *attr(const char *tag, gsize n, const char *name)
{
	size_t nl = strlen(name);
	for (gsize i = 0; i + nl < n; i++) {
		if ((i == 0 || isspace((unsigned char)tag[i - 1])) && !g_ascii_strncasecmp(tag + i, name, nl) && tag[i + nl] == '=') {
			const char *v = tag + i + nl + 1;
			char q = *v;
			if (q == '"' || q == '\'') {
				const char *e = memchr(v + 1, q, n - (gsize)(v + 1 - tag));
				return e ? unent(v + 1, (gsize)(e - v - 1)) : NULL;
			}
			gsize l = 0;
			while (v + l < tag + n && !isspace((unsigned char)v[l]))
				l++;
			return unent(v, l);
		}
	}
	return NULL;
}

int bm_import_html(const char *html, gsize len)
{
	if (!items)
		bm_load();
	if (!g_strstr_len(html, (gssize)(len < 2048 ? len : 2048), "NETSCAPE-Bookmark") &&
	    !g_strstr_len(html, (gssize)len, "<DT>") && !g_strstr_len(html, (gssize)len, "<dt>"))
		return -1;
	GArray *stack = g_array_new(FALSE, FALSE, sizeof(int));	/* parent ids; -1 marks the toolbar folder (flat) */
	int cur = 0, pending = 0, have_pending = 0, count = 0, toolbar_pending = 0;
	g_array_append_val(stack, cur);
	const char *p = html, *end = html + len;
	while (p < end) {
		const char *lt = memchr(p, '<', (gsize)(end - p));
		if (!lt)
			break;
		const char *gt = memchr(lt, '>', (gsize)(end - lt));
		if (!gt)
			break;
		const char *tag = lt + 1;
		gsize tl = (gsize)(gt - tag);
		if (tl >= 2 && !g_ascii_strncasecmp(tag, "a", 1) && (isspace((unsigned char)tag[1]))) {
			char *href = attr(tag, tl, "href");
			const char *close = g_strstr_len(gt, (gssize)(end - gt), "</");
			char *title = NULL;
			if (close) {
				/* the text up to the next tag, which should be </A> */
				title = unent(gt + 1, (gsize)(close - gt - 1));
			}
			if (href && *href && g_ascii_strncasecmp(href, "javascript:", 11) && g_ascii_strncasecmp(href, "place:", 6) &&
			    g_ascii_strncasecmp(href, "data:", 5)) {
				char *t = title ? g_strstrip(title) : NULL;
				add_item(t && *t ? t : href, href, cur, 0);
				count++;
			}
			g_free(href);
			g_free(title);
			p = close ? close : gt + 1;
			continue;
		}
		if (tl >= 2 && !g_ascii_strncasecmp(tag, "h3", 2) && (tl == 2 || isspace((unsigned char)tag[2]))) {
			char *tb = attr(tag, tl, "personal_toolbar_folder");
			toolbar_pending = tb != NULL;
			g_free(tb);
			const char *close = g_strstr_len(gt, (gssize)(end - gt), "</");
			char *title = close ? unent(gt + 1, (gsize)(close - gt - 1)) : g_strdup("Folder");
			if (toolbar_pending) {
				pending = cur;	/* the toolbar folder's links go straight to the bar (or the current folder) */
			} else {
				pending = add_item(g_strstrip(title), NULL, cur, 1);
			}
			have_pending = 1;
			g_free(title);
			p = close ? close : gt + 1;
			continue;
		}
		if (tl >= 2 && !g_ascii_strncasecmp(tag, "dl", 2) && (tl == 2 || isspace((unsigned char)tag[2]))) {
			g_array_append_val(stack, cur);	/* remember where we were */
			if (have_pending)
				cur = pending;
			have_pending = 0;
		} else if (tl >= 3 && !g_ascii_strncasecmp(tag, "/dl", 3)) {
			if (stack->len > 1) {
				cur = g_array_index(stack, int, stack->len - 1);
				g_array_set_size(stack, stack->len - 1);
			}
		}
		p = gt + 1;
	}
	g_array_free(stack, TRUE);
	if (count)
		save();
	return count;
}
