/* flsetup -- first boot: choose the keyboard
 * SPDX-License-Identifier: MIT
 *
 * Runs alone under `flcomp --firstboot`. Four layouts, a preview of the
 * keys, a field to try them in, Continue. Writes LAYOUT=xx to
 * /etc/keyboard.conf ($FL_KEYBOARD_CONF for tests) and exits 0.
 */
#include <errno.h>
#include <flui.h>
#include <stdio.h>
#include <string.h>

enum { ID_TILES = 100, ID_TRY = 110, ID_CONT = 111 };

struct setup {
	fl_app *app;
	fl_font *title, *mono, *small;
	int sel;
	char trial[128];
	char err[160];
	int saved;
};

static void use_layout(struct setup *s, int i)
{
	s->sel = i;
	fl_app_set_layout(s->app, fl_kbd_layouts[i].xkb);
}

static void draw(fl_ui *ui, void *ud)
{
	struct setup *s = ud;
	fl_canvas *cv = &ui->cv;
	int W = cv->w, H = cv->h;
	fl_fill_rect(cv, fl_r(0, 0, W, H), fl_th.bg);

	const int CW = 760, CH = 528, PX = 40;
	fl_rect card = fl_r((W - CW) / 2, (H - CH) / 2, CW, CH);
	fl_fill_rect(cv, card, fl_th.panel);
	fl_fill_rect(cv, fl_r(card.x, card.y, CW, 4), fl_th.accent);

	int x = card.x + PX, w = CW - 2 * PX, y = card.y + 40;
	fl_text(cv, s->title, x, y, fl_th.strong, "FreeLFS", -1);
	y += 36 + 4;
	fl_text(cv, ui->f_reg, x, y, fl_th.dim, "Which keyboard do you have?", -1);
	y += 18 + 24;

	int pick = fl_kbd_tiles(ui, ID_TILES, fl_r(x, y, w, 64), 8, s->sel);
	if (pick >= 0 && pick != s->sel) {
		use_layout(s, pick);
		ui->focus = ID_TRY; /* so you can type at once */
	}
	y += 64 + 24;

	fl_kbd_preview(ui, x, y, s->sel);
	y += 3 * 42 + 2 * 5 + 24;

	fl_text(cv, s->small, x, y, fl_th.dim, "Try it: type something here", -1);
	y += 16 + 6;
	int enter = ui->key == 0xff0d; /* Return, before the field takes it */
	fl_textfield(ui, ID_TRY, fl_r(x, y, w, 40), s->trial, sizeof s->trial, NULL, s->mono);
	y += 40 + 24;

	fl_rect cont = fl_r(x + w - 108, y, 108, 40);
	if (s->err[0])
		fl_text_in(cv, ui->f_reg, fl_r(x, y, w - 124, 40), FL_LEFT, 0, fl_th.err, s->err);
	if (fl_button(ui, ID_CONT, cont, "Continue", FL_BTN_ACCENT) || enter) {
		if (fl_kbd_conf_write(fl_kbd_layouts[s->sel].id) == 0) {
			s->saved = 1;
			fl_app_quit(s->app);
		} else {
			snprintf(s->err, sizeof s->err, "Could not save %s: %s", fl_kbd_conf_path(), strerror(errno));
		}
	}

	fl_text_in(cv, s->small, fl_r(W - 216, H - 12 - 16, 200, 16), FL_RIGHT, 0, fl_th.dim, "FreeLFS 0.1");
}

static void closed(fl_win *w, void *ud)
{
	fl_app_quit(((struct setup *)ud)->app);
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		fprintf(stderr, "usage: flsetup\nChoose the keyboard layout and write %s\n", fl_kbd_conf_path());
		return strcmp(argv[1], "-h") && strcmp(argv[1], "--help") ? 2 : 0;
	}
	struct setup s = {0};
	s.app = fl_app_new(NULL, NULL);
	if (!s.app) {
		fprintf(stderr, "flsetup: cannot connect to a Wayland compositor\n");
		return 1;
	}
	s.title = fl_font_ui(600, 28);
	s.mono = fl_font_mono(400, 14);
	s.small = fl_font_ui(400, 12);
	int cur = fl_kbd_conf_read();
	use_layout(&s, cur >= 0 ? cur : 0);

	int ow, oh;
	fl_app_output_size(s.app, &ow, &oh);
	fl_win_opts o = {
		.title = "FreeLFS setup", .app_id = "flsetup", .w = ow, .h = oh, .fullscreen = 1,
		.font_px = 14, .draw = draw, .close = closed, .ud = &s,
	};
	fl_win *w = fl_win_new(s.app, &o);
	if (!w)
		return 1;
	fl_win_ui(w)->focus = ID_TRY;
	fl_app_run(s.app);
	fl_app_free(s.app);
	return s.saved ? 0 : 1;
}
