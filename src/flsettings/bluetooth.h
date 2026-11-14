/* bluetooth.h -- the Bluetooth page of flsettings (bluetoothctl)
 * SPDX-License-Identifier: MIT
 */
#ifndef BLUETOOTH_H
#define BLUETOOTH_H
#include <flui.h>

struct bluetooth;

struct bluetooth *bt_new(fl_app *app, fl_font *head, fl_font *small);
void bt_free(struct bluetooth *b);
void bt_set_win(struct bluetooth *b, fl_win *win);
/* The page became visible (the adapter and devices are read again) or hidden (unpaired scan results go). */
void bt_show(struct bluetooth *b, int visible);
/* Draws into c (offset by the caller's scrolling); returns the height it needs. */
int bt_draw(struct bluetooth *b, fl_ui *ui, fl_rect c);

#endif
