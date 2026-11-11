/* Arithmetic expansion: $(( )) with C integer operators, variables and
 * assignments, evaluated with long integers.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct ar {
	const char *s, *p;
	bool noeval; /* inside the untaken side of && || ?: */
	int depth;
};

static long assign_expr(struct ar *a);

__attribute__((noreturn)) static void ar_fail(struct ar *a, const char *what)
{
	sh_error("arithmetic: %s in '%s'", what, a->s);
}

static void skip(struct ar *a)
{
	while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n')
		a->p++;
}

static bool eat(struct ar *a, const char *op)
{
	skip(a);
	size_t n = strlen(op);
	if (strncmp(a->p, op, n))
		return false;
	/* do not take "<" out of "<<" or "<=", "&" out of "&&", ... */
	char next = a->p[n];
	if (n == 1 && strchr("<>&|", op[0]) && next == op[0])
		return false;
	if (n == 1 && strchr("<>!=", op[0]) && next == '=')
		return false;
	if (n == 1 && strchr("+-*/%&|^", op[0]) && next == '=')
		return false;
	if (n == 2 && (!strcmp(op, "<<") || !strcmp(op, ">>")) && next == '=')
		return false;
	if (n == 1 && (op[0] == '+' || op[0] == '-') && next == op[0])
		return false;
	a->p += n;
	return true;
}

static long var_value(struct ar *a, const char *name)
{
	const char *v = var_get(name);
	if (!v || !*v) {
		if (!v && sh.o_nounset && !a->noeval)
			sh_error("%s: unbound variable", name);
		return 0;
	}
	char *end;
	long n = strtol(v, &end, 0);
	while (*end == ' ' || *end == '\t')
		end++;
	if (!*end)
		return n;
	if (a->depth > 32)
		ar_fail(a, "expression nests too deep");
	struct ar sub = {.s = v, .p = v, .noeval = a->noeval, .depth = a->depth + 1};
	long r = assign_expr(&sub);
	skip(&sub);
	if (*sub.p)
		ar_fail(&sub, "bad value");
	return r;
}

static void set_var(struct ar *a, const char *name, long v)
{
	if (!a->noeval)
		var_set_int(name, v);
}

static long primary(struct ar *a)
{
	skip(a);
	if (*a->p == '(') {
		a->p++;
		long v = assign_expr(a);
		skip(a);
		if (*a->p != ')')
			ar_fail(a, "missing ')'");
		a->p++;
		return v;
	}
	if (isdigit((unsigned char)*a->p)) {
		char *end;
		long v;
		if (a->p[0] == '0' && (a->p[1] == 'x' || a->p[1] == 'X'))
			v = strtol(a->p, &end, 16);
		else if (a->p[0] == '0')
			v = strtol(a->p, &end, 8);
		else {
			v = strtol(a->p, &end, 10);
			if (*end == '#') { /* base#digits */
				int base = (int)v;
				if (base < 2 || base > 36)
					ar_fail(a, "bad base");
				v = strtol(end + 1, &end, base);
			}
		}
		if (isalnum((unsigned char)*end))
			ar_fail(a, "bad number");
		a->p = end;
		return v;
	}
	if (isalpha((unsigned char)*a->p) || *a->p == '_') {
		const char *st = a->p;
		while (isalnum((unsigned char)*a->p) || *a->p == '_')
			a->p++;
		char *name = xstrndup(st, (size_t)(a->p - st));
		skip(a);
		long v;
		/* assignments */
		static const char *const ops[] = {"<<=", ">>=", "+=", "-=", "*=", "/=", "%=",
		                                  "&=", "^=", "|=", NULL};
		for (int i = 0; ops[i]; i++)
			if (!strncmp(a->p, ops[i], strlen(ops[i]))) {
				a->p += strlen(ops[i]);
				long r = assign_expr(a), cur = var_value(a, name);
				switch (ops[i][0]) {
				case '<': v = cur << r; break;
				case '>': v = cur >> r; break;
				case '+': v = cur + r; break;
				case '-': v = cur - r; break;
				case '*': v = cur * r; break;
				case '/':
				case '%':
					if (!r && !a->noeval)
						ar_fail(a, "division by zero");
					v = !r ? 0 : ops[i][0] == '/' ? cur / r : cur % r;
					break;
				case '&': v = cur & r; break;
				case '^': v = cur ^ r; break;
				default: v = cur | r; break;
				}
				set_var(a, name, v);
				free(name);
				return v;
			}
		if (a->p[0] == '=' && a->p[1] != '=') {
			a->p++;
			v = assign_expr(a);
			set_var(a, name, v);
		} else if (!strncmp(a->p, "++", 2) || !strncmp(a->p, "--", 2)) {
			v = var_value(a, name);
			set_var(a, name, a->p[0] == '+' ? v + 1 : v - 1);
			a->p += 2;
		} else
			v = var_value(a, name);
		free(name);
		return v;
	}
	if (!*a->p)
		ar_fail(a, "missing operand");
	ar_fail(a, "unexpected character");
}

static long unary(struct ar *a)
{
	skip(a);
	if (!strncmp(a->p, "++", 2) || !strncmp(a->p, "--", 2)) {
		char op = a->p[0];
		a->p += 2;
		skip(a);
		const char *st = a->p;
		while (isalnum((unsigned char)*a->p) || *a->p == '_')
			a->p++;
		if (a->p == st)
			ar_fail(a, "++/-- needs a variable");
		char *name = xstrndup(st, (size_t)(a->p - st));
		long v = var_value(a, name) + (op == '+' ? 1 : -1);
		set_var(a, name, v);
		free(name);
		return v;
	}
	if (eat(a, "+"))
		return unary(a);
	if (eat(a, "-"))
		return -unary(a);
	if (eat(a, "!"))
		return !unary(a);
	if (eat(a, "~"))
		return ~unary(a);
	return primary(a);
}

static long mul(struct ar *a)
{
	long v = unary(a);
	for (;;) {
		if (eat(a, "*"))
			v *= unary(a);
		else if (eat(a, "/") || eat(a, "%")) {
			char op = a->p[-1];
			long r = unary(a);
			if (!r) {
				if (!a->noeval)
					ar_fail(a, "division by zero");
				v = 0;
			} else
				v = op == '/' ? v / r : v % r;
		} else
			return v;
	}
}

static long add(struct ar *a)
{
	long v = mul(a);
	for (;;) {
		if (eat(a, "+"))
			v += mul(a);
		else if (eat(a, "-"))
			v -= mul(a);
		else
			return v;
	}
}

static long shift(struct ar *a)
{
	long v = add(a);
	for (;;) {
		if (eat(a, "<<"))
			v <<= add(a);
		else if (eat(a, ">>"))
			v >>= add(a);
		else
			return v;
	}
}

static long rel(struct ar *a)
{
	long v = shift(a);
	for (;;) {
		if (eat(a, "<="))
			v = v <= shift(a);
		else if (eat(a, ">="))
			v = v >= shift(a);
		else if (eat(a, "<"))
			v = v < shift(a);
		else if (eat(a, ">"))
			v = v > shift(a);
		else
			return v;
	}
}

static long eq(struct ar *a)
{
	long v = rel(a);
	for (;;) {
		if (eat(a, "=="))
			v = v == rel(a);
		else if (eat(a, "!="))
			v = v != rel(a);
		else
			return v;
	}
}

static long band(struct ar *a)
{
	long v = eq(a);
	while (eat(a, "&"))
		v &= eq(a);
	return v;
}

static long bxor(struct ar *a)
{
	long v = band(a);
	while (eat(a, "^"))
		v ^= band(a);
	return v;
}

static long bor(struct ar *a)
{
	long v = bxor(a);
	while (eat(a, "|"))
		v |= bxor(a);
	return v;
}

static long land(struct ar *a)
{
	long v = bor(a);
	while (eat(a, "&&")) {
		bool save = a->noeval;
		if (!v)
			a->noeval = true;
		long r = bor(a);
		a->noeval = save;
		v = v && r;
	}
	return v;
}

static long lor(struct ar *a)
{
	long v = land(a);
	while (eat(a, "||")) {
		bool save = a->noeval;
		if (v)
			a->noeval = true;
		long r = land(a);
		a->noeval = save;
		v = v || r;
	}
	return v;
}

static long cond(struct ar *a)
{
	long v = lor(a);
	if (!eat(a, "?"))
		return v;
	bool save = a->noeval;
	if (!v)
		a->noeval = true;
	long t = assign_expr(a);
	a->noeval = save;
	skip(a);
	if (*a->p != ':')
		ar_fail(a, "missing ':'");
	a->p++;
	if (v)
		a->noeval = true;
	long f = cond(a);
	a->noeval = save;
	return v ? t : f;
}

static long assign_expr(struct ar *a)
{
	return cond(a);
}

long arith_eval(const char *expr)
{
	struct ar a = {.s = expr, .p = expr};
	skip(&a);
	if (!*a.p)
		return 0;
	long v = assign_expr(&a);
	while (eat(&a, ",")) /* the comma operator */
		v = assign_expr(&a);
	skip(&a);
	if (*a.p)
		ar_fail(&a, "unexpected text");
	return v;
}
