/* perfmode.h -- the performance mode row of the battery flyout. SPDX-License-Identifier: MIT */
#ifndef PERFMODE_H
#define PERFMODE_H
#include <flui.h>
struct panel;
/* desktop.conf performance=auto|on|off, as flcomp reads it (src/flcomp/perf.c) */
int perfmode_forced(void);	/* 1: performance=on */
void perfmode_set(int on);	/* on: performance=on; off: performance=auto */
/* draws the row whose top edge is y (a 48 px high band) and handles its switch */
void perfmode_row(fl_ui *ui, struct panel *p, int y, int width);
#endif
