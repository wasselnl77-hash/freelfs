/* Shell variables, function-local scopes and the positional parameters.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NBUCKETS 256

static struct var *table[NBUCKETS];
static long changes, pchanges;
struct params params;
char *arg0;

struct saved_var {
	struct saved_var *next;
	char *name, *value;
	unsigned flags;
	bool existed;
};
struct scope {
	struct scope *prev;
	struct saved_var *saved;
};
static struct scope *scopes;

static unsigned hash(const char *s)
{
	unsigned h = 5381;
	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h % NBUCKETS;
}

struct var *var_lookup(const char *name)
{
	for (struct var *v = table[hash(name)]; v; v = v->next)
		if (!strcmp(v->name, name))
			return v;
	return NULL;
}

const char *var_get(const char *name)
{
	struct var *v = var_lookup(name);
	return v ? v->value : NULL;
}

long var_changes(void)
{
	return changes;
}

long path_changes(void)
{
	return pchanges;
}

static struct var *var_create(const char *name)
{
	struct var *v = var_lookup(name);
	if (v)
		return v;
	unsigned h = hash(name);
	v = xcalloc(1, sizeof(*v));
	v->name = xstrdup(name);
	v->next = table[h];
	table[h] = v;
	return v;
}

void var_set(const char *name, const char *value, unsigned flags)
{
	struct var *v = var_create(name);
	if ((v->flags & V_READONLY) && value)
		sh_error("%s: is read-only", name);
	if (value) {
		char *nv = xstrdup(value);
		free(v->value);
		v->value = nv;
	}
	v->flags |= flags;
	if (sh.o_allexport)
		v->flags |= V_EXPORT;
	changes++;
	if (!strcmp(name, "PATH")) {
		pchanges++;
		hash_clear();
	}
}

void var_set_int(const char *name, long n)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", n);
	var_set(name, buf, 0);
}

int var_unset(const char *name)
{
	unsigned h = hash(name);
	for (struct var **pp = &table[h]; *pp; pp = &(*pp)->next) {
		struct var *v = *pp;
		if (strcmp(v->name, name))
			continue;
		if (v->flags & V_READONLY) {
			sh_warn("unset: %s: is read-only", name);
			return 1;
		}
		*pp = v->next;
		free(v->name);
		free(v->value);
		free(v);
		changes++;
		if (!strcmp(name, "PATH")) {
			pchanges++;
			hash_clear();
		}
		return 0;
	}
	return 0;
}

void var_export(const char *name)
{
	var_create(name)->flags |= V_EXPORT;
	changes++;
}

void var_assign(const char *assign, unsigned flags)
{
	const char *eq = strchr(assign, '=');
	if (!eq) {
		var_set(assign, NULL, flags);
		return;
	}
	char *name = xstrndup(assign, (size_t)(eq - assign));
	var_set(name, eq + 1, flags);
	free(name);
}

char **var_environ(void)
{
	struct strlist env = {0};
	for (int i = 0; i < NBUCKETS; i++)
		for (struct var *v = table[i]; v; v = v->next)
			if ((v->flags & V_EXPORT) && v->value)
				sl_push(&env, xasprintf("%s=%s", v->name, v->value));
	return sl_nullterm(&env);
}

static int cmp_var(const void *a, const void *b)
{
	return strcmp((*(struct var *const *)a)->name, (*(struct var *const *)b)->name);
}

void var_list(int (*filter)(struct var *), void (*show)(struct var *))
{
	size_t n = 0, cap = 64;
	struct var **all = xmalloc(cap * sizeof(*all));
	for (int i = 0; i < NBUCKETS; i++)
		for (struct var *v = table[i]; v; v = v->next)
			if (!filter || filter(v)) {
				if (n == cap)
					all = xrealloc(all, (cap *= 2) * sizeof(*all));
				all[n++] = v;
			}
	qsort(all, n, sizeof(*all), cmp_var);
	for (size_t i = 0; i < n; i++)
		show(all[i]);
	free(all);
}

void var_names(struct strlist *out)
{
	for (int i = 0; i < NBUCKETS; i++)
		for (struct var *v = table[i]; v; v = v->next)
			if (v->value)
				sl_add(out, v->name);
}

void vars_push_scope(void)
{
	struct scope *s = xcalloc(1, sizeof(*s));
	s->prev = scopes;
	scopes = s;
}

void vars_pop_scope(void)
{
	struct scope *s = scopes;
	if (!s)
		return;
	scopes = s->prev;
	for (struct saved_var *sv = s->saved, *next; sv; sv = next) {
		next = sv->next;
		struct var *v = var_lookup(sv->name);
		if (v)
			v->flags &= ~(unsigned)V_READONLY;
		if (sv->existed) {
			v = var_create(sv->name);
			free(v->value);
			v->value = sv->value;
			v->flags = sv->flags;
		} else {
			var_unset(sv->name);
			free(sv->value);
		}
		if (!strcmp(sv->name, "PATH")) {
			pchanges++;
			hash_clear();
		}
		changes++;
		free(sv->name);
		free(sv);
	}
	free(s);
}

bool var_scope_active(void)
{
	return scopes != NULL;
}

void var_make_local(const char *name)
{
	if (!scopes)
		sh_error("local: only usable in a function");
	for (struct saved_var *sv = scopes->saved; sv; sv = sv->next)
		if (!strcmp(sv->name, name))
			return;
	struct saved_var *sv = xcalloc(1, sizeof(*sv));
	sv->name = xstrdup(name);
	struct var *v = var_lookup(name);
	if (v) {
		sv->existed = true;
		sv->value = v->value ? xstrdup(v->value) : NULL;
		sv->flags = v->flags;
	}
	sv->next = scopes->saved;
	scopes->saved = sv;
}

void params_set(char **argv, int argc)
{
	struct params np;
	np.argc = argc;
	np.argv = xcalloc((size_t)argc + 1, sizeof(char *));
	for (int i = 0; i < argc; i++)
		np.argv[i] = xstrdup(argv[i]);
	params_free(&params);
	params = np;
}

void params_free(struct params *p)
{
	for (int i = 0; i < p->argc; i++)
		free(p->argv[i]);
	free(p->argv);
	p->argv = NULL;
	p->argc = 0;
}

void vars_init(char **envp)
{
	for (char **e = envp; e && *e; e++) {
		const char *eq = strchr(*e, '=');
		if (!eq || !is_name(*e, (size_t)(eq - *e)))
			continue;
		var_assign(*e, V_EXPORT);
	}
	if (!var_get("IFS"))
		var_set("IFS", " \t\n", 0);
	if (!var_get("PS2"))
		var_set("PS2", "> ", 0);
	if (!var_get("PS4"))
		var_set("PS4", "+ ", 0);
	if (!var_get("PATH"))
		var_set("PATH", "/usr/local/bin:/usr/bin:/bin", 0);
	var_set("OPTIND", "1", 0);
	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", (long)getppid());
	var_set("PPID", buf, 0);
	var_set("FLSH_VERSION", FLSH_VERSION, 0);
	/* the user's shell, not necessarily the login shell */
	char path[4096];
	ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (n > 0) {
		path[n] = '\0';
		var_set("FLSH", path, 0);
	}
}
