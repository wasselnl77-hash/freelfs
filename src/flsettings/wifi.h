/* wifi.h -- the Wi-Fi page of flsettings (iwd over D-Bus)
 * SPDX-License-Identifier: MIT
 */
#ifndef WIFI_H
#define WIFI_H
#include <flui.h>

struct wifi;

/* Connects to the system bus ($DBUS_SYSTEM_BUS_ADDRESS overrides) and, when
 * net.connman.iwd is there, registers the password agent. Never fails: with
 * no bus or no iwd the page says so. */
struct wifi *wifi_new(fl_app *app, fl_font *head, fl_font *small);
void wifi_set_win(struct wifi *w, fl_win *win);
void wifi_free(struct wifi *w);
/* The page became visible or hidden: only a visible page polls and scans. */
void wifi_show(struct wifi *w, int on);
/* Draws the page into c and handles its input. */
void wifi_draw(struct wifi *w, fl_ui *ui, fl_rect c);
/* 1 while a password prompt is open (Escape belongs to it). */
int wifi_prompting(const struct wifi *w);

#endif
