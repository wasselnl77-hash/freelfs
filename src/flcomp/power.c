/* Power-saving mode: power_saver=1 in desktop.conf (the panel's battery flyout writes it).
 * The compositor's share is a frame cap of 30 per second (render.c asks power_frame_gap_ms)
 * and a backlight at 70 % (media.c). The CPU governor is the panel's part (flpower).
 * SPDX-License-Identifier: MIT */
#include "comp.h"

int power_frame_gap_ms(void)
{
	return CF.power_saver && !perf_active() ? 33 : 0;	/* performance mode (perf.c) wins */
}

void power_apply(bool first)
{
	backlight_saver(CF.power_saver);
	if (!first)
		damage_all();	/* pending frames wait for the new pace */
}
