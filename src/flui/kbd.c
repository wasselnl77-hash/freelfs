/* kbd.c -- keyboard layout helpers shared by flsetup and flsettings, dead keys
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const fl_kbd_layout fl_kbd_layouts[4] = {
	{"de", "de", "QWERTZ", "German", {"QWERTZUIOP\xc3\x9c+", "ASDFGHJKL\xc3\x96\xc3\x84#", "<YXCVBNM,.-"}, 6},
	{"us", "us", "QWERTY", "US", {"QWERTYUIOP[]", "ASDFGHJKL;'", "ZXCVBNM,./"}, 6},
	{"uk", "gb", "QWERTY", "UK", {"QWERTYUIOP[]", "ASDFGHJKL;'#", "\\ZXCVBNM,./"}, 6},
	{"fr", "fr", "AZERTY", "French", {"AZERTYUIOP^$", "QSDFGHJKLM\xc3\xb9*", "<WXCVBN,;:!"}, 6},
};

int fl_kbd_find(const char *id)
{
	for (int i = 0; i < 4; i++)
		if (!strcmp(fl_kbd_layouts[i].id, id))
			return i;
	return -1;
}

const char *fl_kbd_conf_path(void)
{
	const char *e = getenv("FL_KEYBOARD_CONF");
	return e && *e ? e : "/etc/keyboard.conf";
}

int fl_kbd_conf_read(void)
{
	FILE *f = fopen(fl_kbd_conf_path(), "r");
	if (!f)
		return -1;
	char line[128];
	int found = -1;
	while (fgets(line, sizeof line, f)) {
		if (strncmp(line, "LAYOUT=", 7))
			continue;
		char *v = line + 7;
		v[strcspn(v, "\r\n \t")] = 0;
		if (*v == '"')
			v++;
		v[strcspn(v, "\"")] = 0;
		found = fl_kbd_find(v);
	}
	fclose(f);
	return found;
}

int fl_kbd_conf_write(const char *id)
{
	const char *path = fl_kbd_conf_path();
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
	FILE *f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f, "LAYOUT=%s\n", id);
	int err = fclose(f);
	if (err || rename(tmp, path) < 0) {
		int e = errno;
		unlink(tmp);
		errno = e;
		return -1;
	}
	return 0;
}

/* ---- widgets --------------------------------------------------------------------------------- */

static fl_font *name_font, *sub_font, *key_font;

static void fonts(void)
{
	if (!name_font) {
		name_font = fl_font_mono(500, 14);
		sub_font = fl_font_ui(400, 12);
		key_font = name_font;
	}
}

int fl_kbd_tiles(fl_ui *ui, uint32_t id_base, fl_rect r, int gap, int sel)
{
	fonts();
	int cw = (r.w - 3 * gap) / 4, ret = -1;
	for (int i = 0; i < 4; i++) {
		fl_rect t = {r.x + i * (cw + gap), r.y, cw, r.h};
		int res = fl_widget(ui, id_base + (uint32_t)i, t, FL_FOCUSABLE);
		int on = i == sel;
		fl_fill_rect(&ui->cv, t, on ? fl_th.select : (res & FL_HOVER) ? fl_th.raised : 0);
		fl_stroke_rect(&ui->cv, t, 1, on || (res & FL_FOCUS) ? fl_th.accent : fl_th.line);
		const fl_kbd_layout *l = &fl_kbd_layouts[i];
		int nh = fl_font_height(name_font), sh = fl_font_height(sub_font), top = t.y + (t.h - nh - sh - 2) / 2;
		fl_text(&ui->cv, name_font, t.x + 12, top, on ? fl_th.strong : fl_th.text, l->name, -1);
		fl_text(&ui->cv, sub_font, t.x + 12, top + nh + 2, on ? fl_th.text : fl_th.dim, l->where, -1);
		if ((res & FL_CLICK) || ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space))) {
			if (res & FL_FOCUS && !(res & FL_CLICK))
				ui->key = 0;
			ret = i;
		}
		if ((res & FL_FOCUS) && ui->key == XKB_KEY_Right && i < 3) {
			ui->key = 0;
			ui->focus = id_base + (uint32_t)i + 1;
			ret = i + 1;
		} else if ((res & FL_FOCUS) && ui->key == XKB_KEY_Left && i > 0) {
			ui->key = 0;
			ui->focus = id_base + (uint32_t)i - 1;
			ret = i - 1;
		}
	}
	return ret;
}

void fl_kbd_preview(fl_ui *ui, int x, int y, int layout)
{
	fonts();
	const fl_kbd_layout *l = &fl_kbd_layouts[layout];
	static const int indent[3] = {0, 14, 28};
	for (int row = 0; row < 3; row++) {
		const char *s = l->rows[row];
		int n = 0, kx = x + indent[row], ky = y + row * (42 + 5);
		while (*s) {
			const char *e = s + 1;
			while (((unsigned char)*e & 0xc0) == 0x80)
				e++;
			fl_rect k = {kx + n * (46 + 5), ky, 46, 42};
			int on = row == 0 && n < l->hi;
			fl_fill_rect(&ui->cv, k, on ? fl_th.accent : fl_th.raised);
			char lab[8];
			snprintf(lab, sizeof lab, "%.*s", (int)(e - s), s);
			fl_text_in(&ui->cv, key_font, k, FL_CENTER, 0, on ? fl_th.on_accent : fl_th.text, lab);
			n++;
			s = e;
		}
	}
}

/* ---- dead keys ---------------------------------------------------------------------------------- */

struct dead_map {
	uint32_t sym;
	uint32_t accent;
	const char *base;       /* ASCII letters */
	const uint16_t res[24]; /* the composed code points, same order */
};

static const struct dead_map dead_maps[] = {
	{XKB_KEY_dead_acute, 0xb4, "aeiouyAEIOUYcCnNsSzZ",
	 {0xe1, 0xe9, 0xed, 0xf3, 0xfa, 0xfd, 0xc1, 0xc9, 0xcd, 0xd3, 0xda, 0xdd, 0x107, 0x106, 0x144,
	  0x143, 0x15b, 0x15a, 0x17a, 0x179}},
	{XKB_KEY_dead_grave, 0x60, "aeiouAEIOU", {0xe0, 0xe8, 0xec, 0xf2, 0xf9, 0xc0, 0xc8, 0xcc, 0xd2, 0xd9}},
	{XKB_KEY_dead_circumflex, 0x5e, "aeiouAEIOU", {0xe2, 0xea, 0xee, 0xf4, 0xfb, 0xc2, 0xca, 0xce, 0xd4, 0xdb}},
	{XKB_KEY_dead_diaeresis, 0xa8, "aeiouyAEIOU", {0xe4, 0xeb, 0xef, 0xf6, 0xfc, 0xff, 0xc4, 0xcb, 0xcf, 0xd6, 0xdc}},
	{XKB_KEY_dead_tilde, 0x7e, "anoANO", {0xe3, 0xf1, 0xf5, 0xc3, 0xd1, 0xd5}},
};

static int put_cp(char *out, size_t cap, uint32_t cp)
{
	if (cp < 0x80 && cap > 1) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800 && cap > 2) {
		out[0] = (char)(0xc0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cap > 3) {
		out[0] = (char)(0xe0 | cp >> 12);
		out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	return 0;
}

int fl_compose_feed(fl_compose *c, uint32_t sym, const char *utf8, char *out, size_t cap)
{
	const struct dead_map *dm = NULL, *pend = NULL;
	for (size_t i = 0; i < sizeof dead_maps / sizeof *dead_maps; i++) {
		if (dead_maps[i].sym == sym)
			dm = &dead_maps[i];
		if (c->dead && dead_maps[i].sym == c->dead)
			pend = &dead_maps[i];
	}
	if (dm) { /* a dead key */
		int n = 0;
		if (pend) { /* two in a row: the accent itself */
			n = put_cp(out, cap, pend->accent);
			if (pend == dm) {
				c->dead = 0;
				return n;
			}
		}
		c->dead = sym;
		return n;
	}
	if (sym >= XKB_KEY_dead_grave && sym <= XKB_KEY_dead_greek)
		return 0; /* other dead keys: not composed, swallowed */
	if (!utf8[0])
		return 0; /* shift, arrows...: leave a pending dead key alone */
	if (pend) {
		c->dead = 0;
		if ((unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f) {
			if (utf8[1] == 0) {
				if (utf8[0] == ' ')
					return put_cp(out, cap, pend->accent);
				const char *p = strchr(pend->base, utf8[0]);
				if (p)
					return put_cp(out, cap, pend->res[p - pend->base]);
			}
			int n = put_cp(out, cap, pend->accent);
			size_t l = strlen(utf8);
			if (n + l < cap) {
				memcpy(out + n, utf8, l);
				n += (int)l;
			}
			return n;
		}
	}
	size_t l = strlen(utf8);
	if (l >= cap)
		l = cap - 1;
	memcpy(out, utf8, l);
	return (int)l;
}
