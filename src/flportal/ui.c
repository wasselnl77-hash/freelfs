/* flportal -- helpers the dialogs share
 * SPDX-License-Identifier: MIT
 */
#include "flportal.h"

#include <string.h>

void ui_mask(fl_ui *ui, struct inmask *m)
{
	m->mouse_in = ui->mouse_in;
	m->press = ui->press;
	m->release = ui->release;
	m->wheel = ui->wheel;
	m->btn_held = ui->btn_held;
	m->key = ui->key;
	memcpy(m->text, ui->text, sizeof m->text);
	ui->mouse_in = ui->press = ui->release = ui->wheel = ui->btn_held = 0;
	ui->key = 0;
	ui->text[0] = 0;
}

void ui_unmask(fl_ui *ui, const struct inmask *m)
{
	ui->mouse_in = m->mouse_in;
	ui->press = m->press;
	ui->release = m->release;
	ui->wheel = m->wheel;
	ui->btn_held = m->btn_held;
	ui->key = m->key;
	memcpy(ui->text, m->text, sizeof ui->text);
}

void ui_modal_frame(fl_ui *ui, int w, int h, fl_rect *box)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, fl_r(0, 0, cv->w, cv->h), fl_th.dark ? 0x99000000 : 0x66000000);
	*box = fl_r((cv->w - w) / 2, (cv->h - h) / 2 - 20, w, h);
	fl_fill_rect(cv, *box, fl_th.panel);
	fl_stroke_rect(cv, *box, 1, fl_th.line);
	fl_fill_rect(cv, fl_r(box->x, box->y, w, 3), fl_th.accent);
}

int ui_fit_prefix(fl_font *f, const char *s, int w)
{
	int len = (int)strlen(s), best = 0;
	for (int i = 1; i <= len; i++) {
		if (i < len && ((unsigned char)s[i] & 0xc0) == 0x80)
			continue;
		if (fl_text_width(f, s, i) > w)
			break;
		best = i;
	}
	return best ? best : len > 0 ? 1 : 0;
}

int ui_wrap(fl_ui *ui, fl_font *f, fl_rect r, fl_color c, const char *s, int max_lines)
{
	int lh = fl_font_height(f) + 3, y = r.y, lines = 0;
	const char *p = s;
	while (*p && lines < max_lines) {
		while (*p == ' ')
			p++;
		const char *nl = strchr(p, '\n');
		size_t seg = nl ? (size_t)(nl - p) : strlen(p);
		/* longest prefix of the paragraph that fits, cut at a space when it can be */
		char tmp[512];
		size_t n = seg < sizeof tmp - 1 ? seg : sizeof tmp - 1;
		memcpy(tmp, p, n);
		tmp[n] = 0;
		int fit = ui_fit_prefix(f, tmp, r.w);
		if ((size_t)fit < n) {
			int sp = fit;
			while (sp > 0 && tmp[sp] != ' ')
				sp--;
			if (sp > 0)
				fit = sp;
		}
		if (ui)
			fl_text(&ui->cv, f, r.x, y, c, tmp, fit);
		y += lh;
		lines++;
		p += fit;
		if ((size_t)fit >= seg && nl)
			p = nl + 1;
	}
	return y - r.y;
}

void ui_tile(fl_ui *ui, fl_rect r, const char *name)
{
	fl_fill_rect(&ui->cv, r, fl_th.raised);
	char l[8] = "?";
	if (name && *name) {
		size_t n = 1;
		while (((unsigned char)name[n] & 0xc0) == 0x80 && n < 4)
			n++;
		memcpy(l, name, n);
		l[n] = 0;
	}
	fl_text_in(&ui->cv, ui->f_semi, r, FL_CENTER, 0, fl_th.strong, l);
}
