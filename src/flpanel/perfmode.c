/* perfmode.c -- the "Performance mode" switch of the battery flyout.
 * The setting is desktop.conf performance= and flcomp does the work (src/flcomp/perf.c): auto (the
 * default) switches it on while a fullscreen window is on top, on keeps it on, off never. The
 * switch here sets on, or auto when turned off; "off" can only be written by hand for now, and the
 * row then shows an off switch that turns it on.
 * SPDX-License-Identifier: MIT */
#include <flui.h>
#include <string.h>

#include "flpanel.h"
#include "perfmode.h"

static const char *mode(void)
{
	const char *v = fl_conf_get("performance", "auto");
	return v ? v : "auto";
}

int perfmode_forced(void)
{
	const char *v = mode();
	return !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "always");
}

void perfmode_set(int on)
{
	fl_conf_set("performance", on ? "on" : "auto");
}

void perfmode_row(fl_ui *ui, struct panel *p, int y, int width)
{
	fl_canvas *cv = &ui->cv;
	int on = perfmode_forced();
	const char *v = mode();
	const char *sub = on ? "Always on: no frame limit"
	                  : !strcmp(v, "off") || !strcmp(v, "0") ? "Off"
	                                                         : "Automatic: on while an app is fullscreen";
	fl_fill_rect(cv, fl_r(0, y, width, 1), fl_th.line);
	fl_text(cv, ui->f_reg, 16, y + 14, fl_th.text, "Performance mode", -1);
	fl_text(cv, p->f_small, 16, y + 33, fl_th.dim, sub, -1);
	if (fl_switch(ui, 3, fl_r(width - 16 - 40, y + 10, 40, 24), &on))
		perfmode_set(on);
}
