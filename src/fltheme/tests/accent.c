/* accent.c -- the custom accent field (Colours) and user presets: a deleted accent stays deleted.
 * Host test, no display. desktop.conf is a variable here (the stubs stand for libflui's conf), and
 * one "frame" is what fltheme's page_colours does with the field. Run with an absolute, empty
 * scratch directory: ./tests/accent "$(mktemp -d)". Exit status 1 on a failure.
 * SPDX-License-Identifier: MIT */
#include "../presets.c"

static char *conf_accent; /* accent= in desktop.conf, NULL when unset */
static char conf_path[600];
static int fails;

const char *fl_conf_path(void) { return conf_path; }

const char *fl_conf_get(const char *key, const char *def)
{
	if (!strcmp(key, "accent") && conf_accent)
		return conf_accent;
	return def;
}

/* one write: a NULL value removes the key, as fl_conf_set_many does */
int fl_conf_set_many(const char *const *pairs, int n)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(pairs[2 * i], "accent")) {
			free(conf_accent);
			conf_accent = pairs[2 * i + 1] ? strdup(pairs[2 * i + 1]) : NULL;
		}
	return 0;
}

int hints_write(void) { return 0; }

static void check(int ok, const char *what)
{
	printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		fails++;
}

/* the state Colours keeps between frames */
struct field {
	char seen[64], hex[16];
};

/* the same test fltheme's page_colours makes for "a swatch is in use" */
static int is_swatch(void)
{
	static const char *const ids[] = {"green", "teal", "blue", "amber", "orange", "rose"};
	const char *cur = conf_accent ? conf_accent : "green";
	for (size_t i = 0; i < sizeof ids / sizeof *ids; i++)
		if (!strcmp(cur, ids[i]))
			return 1;
	return 0;
}

static void frame(struct field *f)
{
	accent_field_sync(conf_accent, is_swatch(), f->seen, sizeof f->seen, f->hex, sizeof f->hex);
}

static void set_accent(const char *v)
{
	const char *p[2] = {"accent", v};
	fl_conf_set_many(p, 1);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: accent ABSOLUTE_EMPTY_DIR\n");
		return 2;
	}
	snprintf(conf_path, sizeof conf_path, "%s/desktop.conf", argv[1]);

	/* a custom accent opens in the field */
	set_accent("8a5fd0");
	struct field f = {{0}, {0}};
	frame(&f);
	check(!strcmp(f.hex, "8a5fd0"), "custom accent shows in the field when Colours opens");

	/* the person clears the field: it stays cleared on the next frames (the bug: it refilled) */
	f.hex[0] = 0;
	frame(&f);
	frame(&f);
	check(f.hex[0] == 0, "a cleared field stays cleared");

	/* Reset: the key is removed, the default green is in use, and it does not come back */
	set_accent(NULL);
	frame(&f);
	frame(&f);
	check(conf_accent == NULL, "Reset removes accent= from desktop.conf");
	check(f.hex[0] == 0, "the field is empty after Reset");

	/* reopen Colours: nothing custom is shown, the removed accent is not restored */
	struct field g = {{0}, {0}};
	frame(&g);
	check(g.hex[0] == 0 && conf_accent == NULL, "after reopening, the removed accent does not come back");

	/* a custom accent that was not removed survives a reopen */
	set_accent("123abc");
	frame(&g);
	check(!strcmp(g.hex, "123abc"), "a custom accent that is still set shows again after reopening");

	/* choosing a swatch empties the field, and typed text is kept until the accent changes */
	set_accent("teal");
	frame(&g);
	check(g.hex[0] == 0, "a swatch in use leaves the field empty");
	snprintf(g.hex, sizeof g.hex, "ff00aa");
	frame(&g);
	check(!strcmp(g.hex, "ff00aa"), "typed text is kept while the accent does not change");

	/* user presets: a preset keeps its own copy of an accent, and deleting it really removes it */
	struct user_preset up[USER_MAX];
	set_accent("8a5fd0");
	check(user_save("Violet") == 0, "save a preset with the custom accent");
	check(user_list(up, USER_MAX) == 1 && !strcmp(up[0].name, "Violet"), "the preset is listed");
	check(user_delete("Violet") == 0, "delete the preset");
	check(user_list(up, USER_MAX) == 0, "a deleted preset is not listed again");
	check(user_delete("Violet") == -1, "deleting it twice reports a failure (no silent success)");

	free(conf_accent);
	printf("%s: %d failure(s)\n", fails ? "FAILED" : "passed", fails);
	return fails ? 1 : 0;
}
