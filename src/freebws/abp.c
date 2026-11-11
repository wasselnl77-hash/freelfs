/* abp -- see abp.h. SPDX-License-Identifier: MIT
 *
 * Site table format (abp_site_table), UTF-8 text, one fact per line, sorted by key
 * (byte order), lines of one key in the order of the lists:
 *     key TAB selector              hide this selector on the key's pages
 *     key TAB !selector             do not hide it there (unhide; also against generic ones)
 *     key TAB $css text             a style rule as is (selector { declarations })
 *     key TAB +js TAB name TAB arg  TAB arg ...    scriptlet call
 *     key TAB -js                   no scriptlets there (#@#+js())
 *     key TAB @elemhide | @generichide | @specifichide | @nofilter
 *                                   cosmetic filtering off / generic off / specific off /
 *                                   the whole blocker off for this site (@@...$document)
 * The key is a host name (matches the host and its subdomains), an entity "name.*"
 * (any public suffix) or "*" (all pages; only used for exceptions).
 */
#include "abp.h"

#include <ctype.h>
#include <string.h>

struct rule {
	char *json;	/* the rule objects of one filter, ready to join; may be two objects */
	char *orig;	/* the filter line, for $badfilter */
	int kind;	/* 0 block, 1 exception, 2 important block */
	int n;		/* number of objects in json */
};

struct abp {
	GPtrArray *rules;
	GHashTable *seen;	/* json -> 1 */
	GHashTable *bad;	/* original lines named by $badfilter */
	GPtrArray *gen;
	GHashTable *gen_seen;
	GHashTable *site;	/* key -> GPtrArray of char * (the part after the TAB) */
	GHashTable *site_seen;
	GPtrArray *blocks, *excepts, *imps;	/* GString-free views after abp_finish (struct rule *) */
	struct abp_stats st;
};

abp *abp_new(void)
{
	abp *a = g_new0(abp, 1);
	a->rules = g_ptr_array_new();
	a->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	a->bad = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	a->gen = g_ptr_array_new_with_free_func(g_free);
	a->gen_seen = g_hash_table_new(g_str_hash, g_str_equal);
	a->site = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	a->site_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	a->blocks = g_ptr_array_new();
	a->excepts = g_ptr_array_new();
	a->imps = g_ptr_array_new();
	return a;
}

void abp_free(abp *a)
{
	if (!a)
		return;
	for (guint i = 0; i < a->rules->len; i++) {
		struct rule *r = g_ptr_array_index(a->rules, i);
		g_free(r->json);
		g_free(r->orig);
		g_free(r);
	}
	g_ptr_array_free(a->rules, TRUE);
	g_hash_table_destroy(a->seen);
	g_hash_table_destroy(a->bad);
	g_ptr_array_free(a->gen, TRUE);
	g_hash_table_destroy(a->gen_seen);
	g_hash_table_destroy(a->site);
	g_hash_table_destroy(a->site_seen);
	g_ptr_array_free(a->blocks, TRUE);
	g_ptr_array_free(a->excepts, TRUE);
	g_ptr_array_free(a->imps, TRUE);
	g_free(a);
}

const struct abp_stats *abp_stats(const abp *a) { return &a->st; }

/* ---- small helpers ------------------------------------------------------------------ */

static void json_str(GString *o, const char *s)
{
	g_string_append_c(o, '"');
	for (; *s; s++) {
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

static int ascii_only(const char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s < 0x21 || (unsigned char)*s > 0x7e)
			return 0;
	return 1;
}

static int valid_domain(const char *d)
{
	if (!*d || *d == '.' || strlen(d) > 253)
		return 0;
	for (const char *p = d; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_')
			return 0;
	return 1;
}

static char *trim(char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static void site_add(abp *a, const char *key, const char *value)
{
	char *full = g_strconcat(key, "\t", value, NULL);
	if (g_hash_table_contains(a->site_seen, full)) {
		g_free(full);
		return;
	}
	g_hash_table_add(a->site_seen, full);
	GPtrArray *arr = g_hash_table_lookup(a->site, key);
	if (!arr) {
		arr = g_ptr_array_new_with_free_func(g_free);
		g_hash_table_insert(a->site, g_strdup(key), arr);
	}
	g_ptr_array_add(arr, g_strdup(value));
}

/* ---- network rules --------------------------------------------------------------------- */

enum {
	T_IMAGE = 1, T_STYLE = 2, T_SCRIPT = 4, T_FONT = 8, T_MEDIA = 16, T_RAW = 32, T_WS = 64,
	T_PING = 128, T_OTHER = 256, T_FRAME = 512, T_DOC = 1024, T_POPUP = 2048,
	T_DEFAULT = T_IMAGE | T_STYLE | T_SCRIPT | T_FONT | T_MEDIA | T_RAW | T_WS | T_PING | T_OTHER | T_FRAME,
	T_ALL = T_DEFAULT | T_DOC | T_POPUP,
};

struct opts {
	unsigned pos, neg;	/* type bits named / negated */
	int third;		/* 1 third-party only, -1 first-party only */
	int important, match_case;
	int elemhide, generichide, specifichide, document, badfilter;
	GPtrArray *inc, *exc;	/* domains */
	int pos_seen;		/* domain= named some pages (some of them may be inexpressible: entities) */
	int neg_dropped;	/* an exception in domain= that cannot be expressed */
	int bad;		/* an option we cannot express */
};

static const struct { const char *name; unsigned bit; } type_names[] = {
	{ "script", T_SCRIPT }, { "image", T_IMAGE }, { "stylesheet", T_STYLE }, { "css", T_STYLE },
	{ "font", T_FONT }, { "media", T_MEDIA }, { "object", T_OTHER }, { "xmlhttprequest", T_RAW },
	{ "xhr", T_RAW }, { "websocket", T_WS }, { "ping", T_PING }, { "other", T_OTHER },
	{ "subdocument", T_FRAME }, { "frame", T_FRAME }, { "popup", T_POPUP },
};

static void opts_domains(struct opts *o, const char *val)
{
	char **v = g_strsplit(val, "|", 0);
	for (int i = 0; v[i]; i++) {
		char *d = v[i];
		int neg = *d == '~';
		if (neg)
			d++;
		for (char *p = d; *p; p++)
			*p = (char)g_ascii_tolower(*p);
		if (!neg)
			o->pos_seen = 1;
		if (!valid_domain(d)) {	/* entities (google.*), regexes, non-ASCII: left out */
			if (neg)
				o->neg_dropped = 1;
			continue;
		}
		g_ptr_array_add(neg ? o->exc : o->inc, g_strdup(d));
	}
	g_strfreev(v);
}

/* returns 0 when some option makes the rule one we cannot express */
static int parse_opts(const char *s, struct opts *o)
{
	char **v = g_strsplit(s, ",", 0);
	int ok = 1;
	for (int i = 0; v[i] && ok; i++) {
		char *t = trim(v[i]);
		int neg = *t == '~';
		const char *n = neg ? t + 1 : t;
		const char *eq = strchr(n, '=');
		char *name = eq ? g_strndup(n, (gsize)(eq - n)) : g_strdup(n);
		const char *val = eq ? eq + 1 : "";
		unsigned bit = 0;
		for (guint k = 0; k < G_N_ELEMENTS(type_names); k++)
			if (!strcmp(name, type_names[k].name))
				bit = type_names[k].bit;
		if (bit) {
			if (neg)
				o->neg |= bit;
			else
				o->pos |= bit;
		} else if (!strcmp(name, "third-party") || !strcmp(name, "3p")) {
			o->third = neg ? -1 : 1;
		} else if (!strcmp(name, "first-party") || !strcmp(name, "1p")) {
			o->third = neg ? 1 : -1;
		} else if (!strcmp(name, "domain") || !strcmp(name, "from")) {
			opts_domains(o, val);
			if (!neg && !o->inc->len && !o->exc->len)
				ok = 0;
		} else if (!strcmp(name, "important")) {
			o->important = 1;
		} else if (!strcmp(name, "match-case")) {
			o->match_case = 1;
		} else if (!strcmp(name, "all")) {
			o->pos |= T_ALL;
		} else if (!strcmp(name, "document") || !strcmp(name, "doc")) {
			if (neg)
				o->neg |= T_DOC;
			else
				o->document = 1, o->pos |= T_DOC;
		} else if (!strcmp(name, "elemhide") || !strcmp(name, "ehide")) {
			o->elemhide = 1;
		} else if (!strcmp(name, "generichide") || !strcmp(name, "ghide")) {
			o->generichide = 1;
		} else if (!strcmp(name, "specifichide") || !strcmp(name, "shide")) {
			o->specifichide = 1;
		} else if (!strcmp(name, "badfilter")) {
			o->badfilter = 1;
		} else {
			ok = 0;	/* redirect, csp, removeparam, replace, denyallow, to, header, ... */
		}
		g_free(name);
	}
	g_strfreev(v);
	return ok;
}

/* "/regex/" -> a regular expression WebKit takes, or NULL */
static char *regex_to_wk(const char *r)
{
	GString *o = g_string_new(NULL);
	size_t n = strlen(r);
	int in_class = 0;
	for (size_t i = 0; i < n; i++) {
		char c = r[i];
		if ((unsigned char)c < 0x21 || (unsigned char)c > 0x7e)
			goto fail;
		if (c == '\\') {
			if (++i >= n)
				goto fail;
			char e = r[i];
			if (e == 'd') {
				g_string_append(o, in_class ? "0-9" : "[0-9]");
			} else if (e == 'w') {
				g_string_append(o, in_class ? "a-zA-Z0-9_" : "[a-zA-Z0-9_]");
			} else if (strchr(".?+*$^[](){}|\\", e)) {
				g_string_append_printf(o, "\\%c", e);
			} else if (strchr("/-:=&_%~@!#;,'<>\"", e)) {
				g_string_append_c(o, e);
			} else {
				goto fail;
			}
		} else if (in_class) {
			if (c == ']')
				in_class = 0;
			else if (c == '[' || c == '(' || c == ')')
				goto fail;
			g_string_append_c(o, c);
		} else if (c == '[') {
			in_class = 1;
			g_string_append_c(o, c);
		} else if (c == '|' || c == '(' || c == ')' || c == '{' || c == '}') {
			goto fail;
		} else if (c == '^' && i != 0) {
			goto fail;
		} else if (c == '$' && i != n - 1) {
			goto fail;
		} else {
			g_string_append_c(o, c);
		}
	}
	if (in_class || o->len == 0 || o->len > 400)
		goto fail;
	return g_string_free(o, FALSE);
fail:
	g_string_free(o, TRUE);
	return NULL;
}

/* an ABP pattern to a WebKit url-filter, or NULL */
static char *pattern_to_wk(const char *p, int match_case)
{
	int dom = 0, start = 0, end = 0;
	if (p[0] == '|' && p[1] == '|') {
		dom = 1;
		p += 2;
	} else if (p[0] == '|') {
		start = 1;
		p++;
	}
	size_t n = strlen(p);
	if (n && p[n - 1] == '|') {
		end = 1;
		n--;
	}
	GString *o = g_string_new(NULL);
	if (dom)
		g_string_append(o, "^[^:]+:(//)?([^/]+\\.)?");
	else if (start)
		g_string_append_c(o, '^');
	size_t base = o->len;
	int path_seen = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)p[i];
		if (c < 0x21 || c > 0x7e)
			goto fail;
		if (c == '*') {
			if (o->len < 2 || strcmp(o->str + o->len - 2, ".*"))
				g_string_append(o, ".*");
		} else if (c == '^') {
			if (i == n - 1 && dom && !path_seen && !end)
				g_string_append(o, "[/:?]");	/* a host ends here */
			else
				g_string_append(o, "[^a-zA-Z0-9_.%-]");
		} else {
			if (c == '/')
				path_seen = 1;
			if (strchr(".+?$()[]{}|\\", c))
				g_string_append_c(o, '\\');
			g_string_append_c(o, match_case ? (char)c : (char)g_ascii_tolower((int)c));
		}
	}
	if (end)
		g_string_append_c(o, '$');
	else if (o->len >= base + 2 && !strcmp(o->str + o->len - 2, ".*"))
		g_string_truncate(o, o->len - 2);
	if (!dom && !start && o->len >= 2 && !strncmp(o->str, ".*", 2))
		g_string_erase(o, 0, 2);
	if (o->len == 0)
		g_string_append(o, ".*");
	if (o->len > 400)
		goto fail;
	return g_string_free(o, FALSE);
fail:
	g_string_free(o, TRUE);
	return NULL;
}

static void types_json(GString *o, unsigned bits)
{
	static const struct { unsigned bit; const char *names; } m[] = {
		{ T_IMAGE, "\"image\",\"svg-document\"" }, { T_STYLE, "\"style-sheet\"" }, { T_SCRIPT, "\"script\"" },
		{ T_FONT, "\"font\"" }, { T_MEDIA, "\"media\"" }, { T_RAW, "\"raw\",\"fetch\"" },
		{ T_WS, "\"websocket\"" }, { T_PING, "\"ping\"" }, { T_OTHER, "\"other\"" },
		{ T_DOC, "\"document\"" }, { T_POPUP, "\"popup\"" },
	};
	g_string_append(o, ",\"resource-type\":[");
	int first = 1;
	for (guint i = 0; i < G_N_ELEMENTS(m); i++)
		if (bits & m[i].bit) {
			g_string_append_printf(o, "%s%s", first ? "" : ",", m[i].names);
			first = 0;
		}
	g_string_append_c(o, ']');
}

/* one rule object */
static void rule_obj(GString *o, const char *url_filter, unsigned types, int frame_ctx, const struct opts *op,
		     const char *action)
{
	g_string_append(o, "{\"trigger\":{\"url-filter\":");
	json_str(o, url_filter);
	if (op->match_case)
		g_string_append(o, ",\"url-filter-is-case-sensitive\":true");
	if (types)
		types_json(o, types);
	if (frame_ctx)
		g_string_append(o, ",\"load-context\":[\"child-frame\"]");
	if (op->third)
		g_string_append(o, op->third > 0 ? ",\"load-type\":[\"third-party\"]" : ",\"load-type\":[\"first-party\"]");
	GPtrArray *l = op->inc->len ? op->inc : op->exc->len ? op->exc : NULL;
	if (l) {
		g_string_append(o, op->inc->len ? ",\"if-domain\":[" : ",\"unless-domain\":[");
		for (guint i = 0; i < l->len; i++) {
			char *d = g_strconcat("*", g_ptr_array_index(l, i), NULL);
			if (i)
				g_string_append_c(o, ',');
			json_str(o, d);
			g_free(d);
		}
		g_string_append_c(o, ']');
	}
	g_string_append_printf(o, "},\"action\":{\"type\":\"%s\"}}", action);
}

/* host out of a "||host^" style pattern (nothing but the host), or NULL */
static char *pattern_host(const char *p)
{
	if (!(p[0] == '|' && p[1] == '|'))
		return NULL;
	p += 2;
	size_t n = strcspn(p, "^/:*?|");
	if (!n)
		return NULL;
	const char *rest = p + n;
	if (*rest && strcmp(rest, "^") && strcmp(rest, "/") && strcmp(rest, "^|") && strcmp(rest, "|"))
		return NULL;
	char *h = g_ascii_strdown(p, (gssize)n);
	if (!valid_domain(h)) {
		g_free(h);
		return NULL;
	}
	return h;
}

static void add_rule(abp *a, int kind, int n, char *json, const char *orig)
{
	if (g_hash_table_contains(a->seen, json)) {
		g_free(json);
		return;
	}
	g_hash_table_add(a->seen, g_strdup(json));
	struct rule *r = g_new0(struct rule, 1);
	r->json = json;
	r->orig = g_strdup(orig);
	r->kind = kind;
	r->n = n;
	g_ptr_array_add(a->rules, r);
}

static void network_rule(abp *a, const char *orig, char *line)
{
	int exception = 0;
	if (line[0] == '@' && line[1] == '@') {
		exception = 1;
		line += 2;
	}
	struct opts op = { 0 };
	op.inc = g_ptr_array_new_with_free_func(g_free);
	op.exc = g_ptr_array_new_with_free_func(g_free);
	char *dollar = strrchr(line, '$');
	if (dollar && (strchr(dollar, '/') || !dollar[1]))	/* not options: part of the pattern */
		dollar = NULL;
	int skip = 0;
	if (dollar) {
		*dollar = 0;
		if (!parse_opts(dollar + 1, &op))
			skip = 1;
	}
	char *pat = line;
	if (op.badfilter) {
		/* the filter it cancels is this line without that option */
		char *t = g_strdup(orig);
		char *d = strrchr(t, '$');
		if (d) {
			GString *keep = g_string_new(NULL);
			char **v = g_strsplit(d + 1, ",", 0);
			for (int i = 0; v[i]; i++)
				if (strcmp(trim(v[i]), "badfilter"))
					g_string_append_printf(keep, "%s%s", keep->len ? "," : "", v[i]);
			g_strfreev(v);
			*d = 0;
			g_hash_table_add(a->bad, keep->len ? g_strconcat(t, "$", keep->str, NULL) : g_strdup(t));
			g_string_free(keep, TRUE);
		}
		g_free(t);
		a->st.net_badfilter++;
		goto done;
	}
	if (skip) {
		a->st.net_skipped++;
		goto done;
	}
	/* switches of the lists: @@||host^$document, $elemhide, ... */
	if (exception && (op.document || op.elemhide || op.generichide || op.specifichide)) {
		GPtrArray *hosts = g_ptr_array_new_with_free_func(g_free);
		char *h = pattern_host(pat);
		if (h)
			g_ptr_array_add(hosts, h);
		else if (!*pat || !strcmp(pat, "*"))
			for (guint i = 0; i < op.inc->len; i++)
				g_ptr_array_add(hosts, g_strdup(g_ptr_array_index(op.inc, i)));
		if (!hosts->len)
			a->st.net_skipped++;
		for (guint i = 0; i < hosts->len; i++) {
			const char *hk = g_ptr_array_index(hosts, i);
			if (op.document)
				site_add(a, hk, "@nofilter");
			if (op.elemhide)
				site_add(a, hk, "@elemhide");
			if (op.generichide)
				site_add(a, hk, "@generichide");
			if (op.specifichide)
				site_add(a, hk, "@specifichide");
			a->st.allow_hosts++;
		}
		g_ptr_array_free(hosts, TRUE);
		goto done;
	}
	if (!exception && (op.elemhide || op.generichide || op.specifichide)) {
		a->st.net_skipped++;
		goto done;
	}
	/* the pattern */
	size_t pl = strlen(pat);
	char *rx = NULL;
	if (pl > 2 && pat[0] == '/' && pat[pl - 1] == '/') {
		char *inner = g_strndup(pat + 1, pl - 2);
		rx = regex_to_wk(inner);
		g_free(inner);
	} else {
		rx = pattern_to_wk(pat, op.match_case);
	}
	if (!rx) {
		a->st.net_skipped++;
		goto done;
	}
	if ((op.pos_seen && !op.inc->len) || (!op.pos_seen && op.neg_dropped)) {
		/* domain= whose pages cannot be expressed (entities like kat.*): skipping the rule is safe,
		 * dropping the condition would make it apply everywhere */
		a->st.net_skipped++;
		g_free(rx);
		goto done;
	}
	unsigned types = op.pos ? op.pos : T_DEFAULT;
	types &= ~op.neg;
	int nobj = 0;
	GString *o = g_string_new(NULL);
	const char *action = exception ? "ignore-previous-rules" : "block";
	unsigned plain = types & ~T_FRAME;
	if (op.pos == 0 && (plain & T_DOC))
		plain &= ~T_DOC;
	/* a top-level load is never third-party: for $third-party rules the frame case needs no
	 * rule of its own (WebKit's "document" type covers both, and the top frame cannot match) */
	if ((types & T_FRAME) && op.third > 0) {
		plain |= T_DOC;
		types &= ~T_FRAME;
	}
	/* a rule for a whole host: its frame case is folded into the one rule as well (going to
	 * the host itself in a tab is blocked too: the error page offers to open it anyway) */
	if (types & T_FRAME) {
		char *ph = pattern_host(pat);
		if (ph) {
			plain |= T_DOC;
			types &= ~T_FRAME;
			g_free(ph);
		}
	}
	if (plain) {
		rule_obj(o, rx, plain, 0, &op, action);
		nobj++;
	}
	if (types & T_FRAME) {
		if (nobj)
			g_string_append(o, ",\n");
		rule_obj(o, rx, T_DOC, 1, &op, action);
		nobj++;
	}
	g_free(rx);
	if (!nobj) {
		g_string_free(o, TRUE);
		a->st.net_skipped++;
		goto done;
	}
	int kind = exception ? 1 : op.important ? 2 : 0;
	add_rule(a, kind, nobj, g_string_free(o, FALSE), orig);
done:
	g_ptr_array_free(op.inc, TRUE);
	g_ptr_array_free(op.exc, TRUE);
}

/* ---- cosmetic rules ------------------------------------------------------------------ */

/* position of the separator of a cosmetic rule, or NULL. *excep: #@#, *kind: '#', '?', '$', '%' */
static const char *find_cosmetic(const char *s, int *excep, char *kind, int *slen)
{
	for (const char *p = s; *p; p++) {
		if (*p == '#') {
			const char *q = p + 1;
			int ex = 0;
			if (*q == '@') {
				ex = 1;
				q++;
			}
			char k = '#';
			if (*q == '?' || *q == '$' || *q == '%') {
				k = *q;
				q++;
			}
			if (*q != '#')
				continue;
			*excep = ex;
			*kind = k;
			*slen = (int)(q + 1 - p);
			return p;
		}
		if (!isalnum((unsigned char)*p) && !strchr(".,~*_:-", *p))
			return NULL;	/* not a domain list in front: a network rule */
	}
	return NULL;
}

static int balanced(const char *s)
{
	int par = 0, br = 0;
	char q = 0;
	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			continue;
		}
		if (q) {
			if (*s == q)
				q = 0;
			continue;
		}
		if (*s == '"' || *s == '\'')
			q = *s;
		else if (*s == '(')
			par++;
		else if (*s == ')')
			par--;
		else if (*s == '[')
			br++;
		else if (*s == ']')
			br--;
		if (par < 0 || br < 0)
			return 0;
	}
	return !par && !br && !q;
}

static int procedural(const char *s)
{
	static const char *const bad[] = {
		":has-text(", ":contains(", ":-abp-", ":xpath(", ":matches-css", ":matches-media", ":matches-path",
		":matches-attr", ":matches-prop", ":min-text-length", ":others(", ":upward(", ":watch-attr",
		":remove", ":nth-ancestor", ":matches-", ":not-", "^", "{", "}", ";", "<", ">>>",
	};
	for (guint i = 0; i < G_N_ELEMENTS(bad); i++)
		if (strstr(s, bad[i])) {
			if (!strcmp(bad[i], "^") && !s[0])
				continue;
			if (!strcmp(bad[i], "^")) {	/* ^= in attribute selectors is fine */
				const char *p = s;
				int ok = 1;
				while ((p = strchr(p, '^'))) {
					if (p[1] != '=')
						ok = 0;
					p++;
				}
				if (ok)
					continue;
			}
			return 1;
		}
	return 0;
}

static void scriptlet(abp *a, const char *key, int exception, const char *body)
{
	/* body: "+js(name, arg, arg)" */
	size_t n = strlen(body);
	if (n < 5 || body[n - 1] != ')')
		return;
	char *in = g_strndup(body + 4, n - 5);
	if (exception) {
		if (!*trim(in))
			site_add(a, key, "-js");
		g_free(in);
		return;
	}
	GString *out = g_string_new("+js");
	GString *cur = g_string_new(NULL);
	for (const char *p = in;; p++) {
		if (*p == '\\' && p[1] == ',') {
			g_string_append_c(cur, ',');
			p++;
		} else if (*p == ',' || !*p) {
			char *t = trim(cur->str);
			if (*t == '\t')
				*t = ' ';
			g_string_append_printf(out, "\t%s", t);
			g_string_truncate(cur, 0);
			if (!*p)
				break;
		} else if (*p == '\t' || *p == '\n') {
			g_string_append_c(cur, ' ');
		} else {
			g_string_append_c(cur, *p);
		}
	}
	if (strlen(out->str) > 3) {
		site_add(a, key, out->str);
		if (!strcmp(key, "*"))
			a->st.scriptlets_generic++;
		else
			a->st.scriptlets++;
	}
	g_string_free(cur, TRUE);
	g_string_free(out, TRUE);
	g_free(in);
}

static void cosmetic_rule(abp *a, char *line, const char *sep, int excep, char kind, int slen)
{
	char *doms = g_strndup(line, (gsize)(sep - line));
	char *body = trim((char *)sep + slen);
	GPtrArray *inc = g_ptr_array_new_with_free_func(g_free), *exc = g_ptr_array_new_with_free_func(g_free);
	char **v = g_strsplit(doms, ",", 0);
	for (int i = 0; v[i]; i++) {
		char *d = trim(v[i]);
		if (!*d || !strcmp(d, "*"))
			continue;
		int neg = *d == '~';
		if (neg)
			d++;
		char *l = g_ascii_strdown(d, -1);
		int ent = g_str_has_suffix(l, ".*");
		if (!ent && !valid_domain(l)) {
			g_free(l);
			continue;
		}
		if (ent) {
			char *base = g_strndup(l, strlen(l) - 2);
			int okd = valid_domain(base);
			g_free(base);
			if (!okd) {
				g_free(l);
				continue;
			}
		}
		g_ptr_array_add(neg ? exc : inc, l);
	}
	g_strfreev(v);
	GString *val = NULL;
	int is_js = !strncmp(body, "+js(", 4);
	if (kind == '%' || (kind == '$' && excep)) {
		a->st.cos_procedural++;
		goto done;
	}
	if (is_js) {
		if (inc->len == 0 && exc->len == 0 && excep) {
			site_add(a, "*", "-js");
		} else if (inc->len == 0) {
			scriptlet(a, "*", excep, body);
		} else {
			for (guint i = 0; i < inc->len; i++)
				scriptlet(a, g_ptr_array_index(inc, i), excep, body);
		}
		goto done;
	}
	val = g_string_new(NULL);
	if (kind == '$') {	/* ABP style injection "selector { declarations }" */
		if (!strchr(body, '{') || !strchr(body, '}') || !balanced(body) || strstr(body, "url(") ||
		    !g_str_has_suffix(body, "}")) {
			a->st.cos_procedural++;
			goto done;
		}
		g_string_append_printf(val, "$%s", body);
	} else {
		char *st = strstr(body, ":style(");
		if (st && g_str_has_suffix(body, ")")) {
			char *sel = g_strndup(body, (gsize)(st - body));
			char *decl = g_strndup(st + 7, strlen(st + 7) - 1);
			if (!balanced(sel) || procedural(sel) || strchr(decl, '{') || strchr(decl, '}') || strstr(decl, "url(") ||
			    !*trim(sel) || excep) {
				a->st.cos_procedural++;
				g_free(sel);
				g_free(decl);
				goto done;
			}
			g_string_append_printf(val, "$%s{%s%s}", trim(sel), trim(decl),
					       strstr(decl, "!important") ? "" : " !important");
			g_free(sel);
			g_free(decl);
		} else {
			if (!*body || !balanced(body) || procedural(body) || !ascii_only(body) ||
			    (excep && strchr(body, '\t')) || strchr(body, '\t')) {
				if (*body)
					a->st.cos_procedural++;
				goto done;
			}
			g_string_append(val, excep ? "!" : "");
			g_string_append(val, body);
		}
	}
	if (inc->len == 0) {
		if (excep) {
			site_add(a, "*", val->str);
			a->st.cos_exception++;
		} else if (val->str[0] == '$') {
			site_add(a, "*", val->str);
			a->st.cos_site++;
		} else {
			if (!g_hash_table_contains(a->gen_seen, val->str)) {
				char *c = g_strdup(val->str);
				g_hash_table_add(a->gen_seen, c);
				g_ptr_array_add(a->gen, c);
				a->st.cos_generic++;
			}
			for (guint i = 0; i < exc->len; i++) {
				char *x = g_strconcat("!", val->str, NULL);
				site_add(a, g_ptr_array_index(exc, i), x);
				g_free(x);
			}
		}
	} else {
		for (guint i = 0; i < inc->len; i++)
			site_add(a, g_ptr_array_index(inc, i), val->str);
		if (!excep)
			for (guint i = 0; i < exc->len; i++) {
				char *x = g_strconcat("!", val->str, NULL);
				if (val->str[0] != '$')
					site_add(a, g_ptr_array_index(exc, i), x);
				g_free(x);
			}
		if (excep)
			a->st.cos_exception += inc->len;
		else
			a->st.cos_site += inc->len;
	}
done:
	if (val)
		g_string_free(val, TRUE);
	g_ptr_array_free(inc, TRUE);
	g_ptr_array_free(exc, TRUE);
	g_free(doms);
}

/* ---- entry points -------------------------------------------------------------------- */

void abp_add_list(abp *a, const char *text, gsize len)
{
	const char *end = text + len;
	while (text < end) {
		const char *nl = memchr(text, '\n', (size_t)(end - text));
		size_t ll = nl ? (size_t)(nl - text) : (size_t)(end - text);
		char *line = g_strndup(text, ll);
		text += ll + (nl ? 1 : 0);
		char *t = trim(line);
		a->st.lines++;
		if (!*t || *t == '!' || (*t == '[' && t[1] == 'A') || (*t == '#' && t[1] == ' ') || !strncmp(t, "#!", 2)) {
			g_free(line);
			continue;
		}
		int ex = 0, slen = 0;
		char kind = 0;
		const char *sep = find_cosmetic(t, &ex, &kind, &slen);
		if (sep)
			cosmetic_rule(a, t, sep, ex, kind, slen);
		else if (!strncmp(t, "||", 2) || !strncmp(t, "@@", 2) || !strchr(t, ' '))
			{
			char *copy = g_strdup(t);
			network_rule(a, t, copy);
			g_free(copy);
		}
		g_free(line);
	}
}

void abp_finish(abp *a)
{
	for (guint i = 0; i < a->rules->len; i++) {
		struct rule *r = g_ptr_array_index(a->rules, i);
		if (g_hash_table_contains(a->bad, r->orig)) {
			r->kind = -1;
			continue;
		}
		if (r->kind == 0) {
			g_ptr_array_add(a->blocks, r);
			a->st.net_block += (unsigned)r->n;
		} else if (r->kind == 1) {
			g_ptr_array_add(a->excepts, r);
			a->st.net_except += (unsigned)r->n;
		} else if (r->kind == 2) {
			g_ptr_array_add(a->imps, r);
			a->st.net_important += (unsigned)r->n;
		}
	}
}

static unsigned count_objs(GPtrArray *v, guint from, guint to)
{
	unsigned n = 0;
	for (guint i = from; i < to && i < v->len; i++)
		n += (unsigned)((struct rule *)g_ptr_array_index(v, i))->n;
	return n;
}

/* number of rule objects of the exceptions that go into every chunk */
static unsigned exc_count(const abp *a) { return count_objs(a->excepts, 0, a->excepts->len); }

int abp_chunks(const abp *a, int max_rules)
{
	unsigned e = exc_count(a);
	int per = max_rules - (int)e - (int)a->st.net_important;
	if (per < max_rules / 4)
		per = max_rules / 4;
	unsigned total = a->st.net_block + a->st.net_important;
	int n = (int)((total + (unsigned)per - 1) / (unsigned)per);
	return n < 1 ? 1 : n;
}

char *abp_chunk_json(const abp *a, int max_rules, int i, unsigned *nrules)
{
	int n = abp_chunks(a, max_rules);
	GString *o = g_string_new("[");
	unsigned cnt = 0;
	int first = 1;
	guint nb = a->blocks->len, ni = a->imps->len;
	guint b0 = (guint)((guint64)nb * (guint)i / (guint)n), b1 = (guint)((guint64)nb * (guint)(i + 1) / (guint)n);
	guint i0 = (guint)((guint64)ni * (guint)i / (guint)n), i1 = (guint)((guint64)ni * (guint)(i + 1) / (guint)n);
	GPtrArray *seq[3] = { a->blocks, a->excepts, a->imps };
	guint from[3] = { b0, 0, i0 }, to[3] = { b1, a->excepts->len, i1 };
	unsigned room = (unsigned)max_rules;
	for (int s = 0; s < 3; s++)
		for (guint k = from[s]; k < to[s]; k++) {
			struct rule *r = g_ptr_array_index(seq[s], k);
			if (cnt + (unsigned)r->n > room)
				break;
			g_string_append_printf(o, "%s\n%s", first ? "" : ",", r->json);
			first = 0;
			cnt += (unsigned)r->n;
		}
	if (first) {	/* WebKit does not take an empty list */
		g_string_append(o, "\n{\"trigger\":{\"url-filter\":\"^https?://freebws\\\\.invalid/\"},\"action\":{\"type\":\"block\"}}");
		cnt = 1;
	}
	g_string_append(o, "\n]\n");
	if (nrules)
		*nrules = cnt;
	return g_string_free(o, FALSE);
}

char *abp_generic(const abp *a)
{
	GString *o = g_string_new(NULL);
	for (guint i = 0; i < a->gen->len; i++)
		g_string_append_printf(o, "%s\n", (char *)g_ptr_array_index(a->gen, i));
	return g_string_free(o, FALSE);
}

static int cmp_str(gconstpointer x, gconstpointer y) { return strcmp(*(char *const *)x, *(char *const *)y); }

char *abp_site_table(const abp *a)
{
	GPtrArray *keys = g_ptr_array_new();
	GHashTableIter it;
	gpointer k, v;
	g_hash_table_iter_init(&it, a->site);
	while (g_hash_table_iter_next(&it, &k, &v))
		g_ptr_array_add(keys, k);
	g_ptr_array_sort(keys, cmp_str);
	GString *o = g_string_new(NULL);
	for (guint i = 0; i < keys->len; i++) {
		const char *key = g_ptr_array_index(keys, i);
		GPtrArray *vals = g_hash_table_lookup(a->site, key);
		for (guint j = 0; j < vals->len; j++)
			g_string_append_printf(o, "%s\t%s\n", key, (char *)g_ptr_array_index(vals, j));
	}
	g_ptr_array_free(keys, TRUE);
	return g_string_free(o, FALSE);
}
