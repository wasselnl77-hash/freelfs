/* fltheme.h -- Themes, the one place to adjust the look
 * SPDX-License-Identifier: MIT
 */
#ifndef FLTHEME_H
#define FLTHEME_H

#include <flui.h>

/* hints.c: GTK settings.ini (colour scheme, font) for Flatpak and other GTK apps; 0 or -1 */
int hints_write(void);

/* presets.c: the keys a theme is made of, the built-in presets and the user's own */
#define PRESET_MAX_KEYS 12
struct preset {
	const char *name, *blurb;
	int n;
	const char *kv[PRESET_MAX_KEYS][2]; /* key, value; value NULL: back to the default */
};
extern const char *const theme_keys[]; /* every key a theme sets, NULL terminated */
const char *theme_default(const char *key);
extern const struct preset builtin_presets[];
extern const int n_builtin;
int preset_matches(const struct preset *p); /* the live look is what the preset says */
int preset_apply(const struct preset *p);   /* one write to desktop.conf, GTK hints too */
int theme_reset(void);

/* user presets: ~/.config/freelfs/themes/NAME.theme, key=value lines */
#define USER_MAX 24
struct user_preset {
	char name[40];
};
const char *themes_dir(void);
int user_list(struct user_preset *out, int max);
int user_save(const char *name);   /* the keys of the live look; 0, or -1 */
int user_load(const char *name);   /* 0, or -1 (missing, not a theme file) */
int user_delete(const char *name);
int name_ok(const char *name);     /* letters, digits, space, - and _ , 1..32 */

#endif
