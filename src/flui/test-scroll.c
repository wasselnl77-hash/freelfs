/* test-scroll.c -- synthetic pointer events through the immediate-mode widgets (no compositor)
 * SPDX-License-Identifier: MIT
 *
 * Links ui.c and gfx.c with stubs for the text and clipboard calls (the scrollbar and the list
 * draw nothing that needs them). Each frame is one run_pass pass: the press, the held button and
 * the release come in as run_pass would deliver them. Exit status: the number of failed checks.
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 240
#define H 320
static uint32_t px[W * H];
static int fails;

#define CHECK(c, ...)                                                                                \
	do {                                                                                         \
		if (c) {                                                                             \
			printf("ok    ");                                                            \
		} else {                                                                             \
			printf("FAIL  ");                                                            \
			fails++;                                                                     \
		}                                                                                    \
		printf(__VA_ARGS__);                                                                 \
		printf("\n");                                                                        \
	} while (0)

/* ---- stubs: text and clipboard are not drawn here; the theme is all zero (colours only) --- */

fl_theme fl_th;
fl_look fl_lk;

int fl_text_width(fl_font *f, const char *s, int len) { (void)f; (void)s; return len; }
int fl_font_height(const fl_font *f) { (void)f; return 16; }
int fl_text(fl_canvas *cv, fl_font *f, int x, int y, fl_color c, const char *s, int len)
{ (void)cv; (void)f; (void)x; (void)y; (void)c; (void)s; return len; }
void fl_text_in(fl_canvas *cv, fl_font *f, fl_rect r, int align, int pad, fl_color c, const char *s)
{ (void)cv; (void)f; (void)r; (void)align; (void)pad; (void)c; (void)s; }
int fl_text_fit(fl_font *f, const char *s, int w, char *out, size_t cap)
{ (void)f; (void)s; (void)w; (void)out; (void)cap; return 0; }
void fl_clipboard_set(fl_app *a, const char *text, size_t len) { (void)a; (void)text; (void)len; }
char *fl_clipboard_get(fl_app *a, size_t *len) { (void)a; (void)len; return NULL; }
fl_app *fl_win_app(fl_win *w) { (void)w; return NULL; }

/* ---- the frame: one pass as run_pass runs it ---------------------------------------------- */

static void set_pointer(fl_ui *ui, int x, int y, int held, int press, int release)
{
	ui->mx = x;
	ui->my = y;
	ui->mouse_in = 1;
	ui->btn_held = held;
	ui->press = press;
	ui->release = release;
}

typedef void (*draw_fn)(fl_ui *ui, void *ud);

static void frame(fl_ui *ui, draw_fn draw, void *ud)
{
	fl_ui_pass_begin(ui, 1);
	ui->cv = (fl_canvas){px, W, H, W, {0, 0, W, H}};
	ui->cursor = FL_CURSOR_ARROW;
	ui->_repass = 0;
	draw(ui, ud);
	fl_ui_pass_end(ui);
}

/* ---- scene 1: a bare bar (content 1000 px, view 300 px: thumb 90 px, span 210, max 700) -- */

#define ID_BAR 7u
static const fl_rect bar_r = {190, 0, 4, 300};
static int bar_off;
static int bar_changed;

static void draw_bar(fl_ui *ui, void *ud)
{
	(void)ud;
	bar_changed = fl_scrollbar(ui, ID_BAR, bar_r, &bar_off, 1000, 300);
}

static void scene_bar(void)
{
	fl_ui ui;
	memset(&ui, 0, sizeof ui);
	bar_off = 0;
	printf("scene: bar alone\n");

	set_pointer(&ui, 100, 200, 0, 0, 0);
	frame(&ui, draw_bar, NULL);
	CHECK(bar_off == 0, "idle: offset stays at 0");

	set_pointer(&ui, 191, 45, FL_BTN_L, FL_BTN_L, 0); /* press on the thumb (y 0..89) */
	frame(&ui, draw_bar, NULL);
	CHECK(bar_off == 0 && ui.active == ID_BAR, "press on thumb holds the bar, offset unchanged");

	set_pointer(&ui, 191, 145, FL_BTN_L, 0, 0); /* drag 100 px down: 100 * 700 / 210 = 333 */
	frame(&ui, draw_bar, NULL);
	CHECK(bar_off == 333, "drag 100 px: offset %d (want 333)", bar_off);

	set_pointer(&ui, 20, 245, FL_BTN_L, 0, 0); /* off the bar, to the left: still dragging */
	frame(&ui, draw_bar, NULL);
	CHECK(bar_off == 666, "drag outside the bar keeps going: offset %d (want 666)", bar_off);

	set_pointer(&ui, 20, 245, 0, 0, FL_BTN_L); /* release outside */
	frame(&ui, draw_bar, NULL);
	CHECK(bar_off == 666 && ui.active == 0 && ui._sb_on == 0, "release ends the drag at 666");

	/* track press: one page (300 px) toward the pointer, not repeated while held */
	bar_off = 0;
	set_pointer(&ui, 191, 250, FL_BTN_L, FL_BTN_L, 0);
	frame(&ui, draw_bar, NULL);
	CHECK(bar_off == 300, "track press below the thumb pages down: offset %d (want 300)", bar_off);
	set_pointer(&ui, 191, 250, FL_BTN_L, 0, 0);
	frame(&ui, draw_bar, NULL);
	frame(&ui, draw_bar, NULL);
	CHECK(bar_off == 300, "held on the track: no second page (offset %d)", bar_off);
	set_pointer(&ui, 191, 250, 0, 0, FL_BTN_L);
	frame(&ui, draw_bar, NULL);

	/* track press above the thumb: one page up. The thumb is now at 300 * 210 / 700 = 90 */
	set_pointer(&ui, 191, 60, FL_BTN_L, FL_BTN_L, 0);
	frame(&ui, draw_bar, NULL);
	CHECK(bar_off == 0, "track press above the thumb pages up: offset %d (want 0)", bar_off);
	set_pointer(&ui, 191, 60, 0, 0, FL_BTN_L);
	frame(&ui, draw_bar, NULL);

	/* the hit area is a little wider than the 4 px bar */
	bar_off = 0;
	set_pointer(&ui, 188, 45, FL_BTN_L, FL_BTN_L, 0);
	frame(&ui, draw_bar, NULL);
	CHECK(ui.active == ID_BAR, "a press 2 px left of the bar still takes it");
	set_pointer(&ui, 188, 45, 0, 0, FL_BTN_L);
	frame(&ui, draw_bar, NULL);
}

/* ---- scene 2: a list with its bar inside the list's area (the real case in every app) -------- */

#define ID_LIST 3u
#define ID_LBAR (3u + 0x10000u)
static const fl_rect list_r = {0, 0, 200, 300};
static fl_list_state lst;
static int list_ret = -1;

static void draw_row(fl_ui *ui, fl_rect r, int index, int hover, int selected, void *ud)
{
	(void)ui; (void)r; (void)index; (void)hover; (void)selected; (void)ud;
}

static void draw_list(fl_ui *ui, void *ud)
{
	(void)ud;
	list_ret = fl_list(ui, ID_LIST, list_r, 50, 20, &lst, draw_row, NULL);
}

static void scene_list(void)
{
	fl_ui ui;
	memset(&ui, 0, sizeof ui);
	memset(&lst, 0, sizeof lst);
	printf("scene: list with its bar inside\n");

	set_pointer(&ui, 100, 200, 0, 0, 0); /* a frame with the pointer away: the bar is known */
	frame(&ui, draw_list, NULL);

	int sel_before = lst.sel; /* the idle frame's hover (row 10) */
	set_pointer(&ui, 196, 45, FL_BTN_L, FL_BTN_L, 0); /* on the list's bar, on its thumb */
	frame(&ui, draw_list, NULL);
	CHECK(list_ret == -1, "press on the bar selects no row (ret %d)", list_ret);
	CHECK(lst.sel == sel_before, "the highlight does not jump to the row under the bar (sel %d)", lst.sel);
	CHECK(ui.active == ID_LBAR, "the bar, not the list, holds the press");

	set_pointer(&ui, 196, 145, FL_BTN_L, 0, 0);
	frame(&ui, draw_list, NULL);
	CHECK(lst.scroll == 333, "dragging the list's bar scrolls the list: scroll %d (want 333)", lst.scroll);

	set_pointer(&ui, 196, 145, 0, 0, FL_BTN_L);
	frame(&ui, draw_list, NULL);
	CHECK(list_ret == -1 && lst.sel == sel_before, "release on the bar activates nothing (ret %d, sel %d)",
	      list_ret, lst.sel);

	/* a plain click on a row still works: row 2 (y 40..59 at scroll 0 is row 2 from y 40) */
	lst.scroll = 0;
	set_pointer(&ui, 50, 45, FL_BTN_L, FL_BTN_L, 0);
	frame(&ui, draw_list, NULL);
	set_pointer(&ui, 50, 45, 0, 0, FL_BTN_L);
	frame(&ui, draw_list, NULL);
	CHECK(list_ret == 2, "a click on row 2 returns 2 (ret %d)", list_ret);
}

int main(void)
{
	scene_bar();
	scene_list();
	printf("%s: %d failed check(s)\n", fails ? "FAILED" : "all passed", fails);
	return fails;
}
