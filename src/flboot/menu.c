/* The boot menu: a clean dark screen with the entries, a countdown, and the
 * arrow keys and Enter. Falls back to the text console without graphics.
 * SPDX-License-Identifier: MIT */
#include "flboot.h"

/* colours, 0xRRGGBB */
#define C_BACK 0x1d2125
#define C_PANEL 0x272c33
#define C_TEXT 0xd8dce0
#define C_DIM 0x8a9199
#define C_TITLE 0xf0f2f4
#define C_ACCENT 0x3f9b63
#define C_SELTEXT 0xffffff

static bool graphics;
static int scale;

static void itoa_s(int v, char *buf)
{
	char tmp[12];
	int n = 0;
	if (v < 0)
		v = 0;
	do
		tmp[n++] = (char)('0' + v % 10);
	while ((v /= 10) && n < 11);
	while (n)
		*buf++ = tmp[--n];
	*buf = '\0';
}

static void countdown_text(int left, char *buf)
{
	if (left < 0) {
		memcpy(buf, "Up/Down: choose   Enter: start", 31);
		return;
	}
	memcpy(buf, "Starting in ", 12);
	itoa_s(left, buf + 12);
	size_t n = strlen(buf);
	memcpy(buf + n, " s   (any key to stop)", 23);
}

/* One screen: a heading, a dim line, an optional paragraph (NULL terminated),
 * rows to choose from, and a hint. */
struct page {
	const char *head, *sub, *foot;
	const char *const *para;
	const char *rows[MAX_ENTRIES + 2];
	int n;
};

static int para_lines(const struct page *p)
{
	int n = 0;
	while (p->para && p->para[n])
		n++;
	return n;
}

/* ROW cut to fit MAXC characters, in BUF */
static const char *fit(const char *row, int maxc, char *buf, size_t cap)
{
	size_t n = strlen(row);
	if ((int)n <= maxc || maxc < 8 || (size_t)maxc + 1 > cap)
		return row;
	memcpy(buf, row, (size_t)maxc - 3);
	memcpy(buf + maxc - 3, "...", 4);
	return buf;
}

static void draw_gfx(const struct page *p, int sel)
{
	int line = font_height * scale, rowh = line + 14 * scale, np = para_lines(p);
	int pw = 560 * scale;
	if (pw > gfx_width() - 40)
		pw = gfx_width() - 40;
	int top = 28 * scale + line * 3 + 28 * scale;
	int ph = top + (np ? np * (line + 6 * scale) + 18 * scale : 0) + p->n * rowh + line + 52 * scale;
	int px = (gfx_width() - pw) / 2, py = (gfx_height() - ph) / 2;
	if (py < 0)
		py = 0;
	int maxc = (pw - 72 * scale) / (font_width * scale);
	char buf[128];

	gfx_clear(C_BACK);
	gfx_rect(px, py, pw, ph, C_PANEL);
	gfx_rect(px, py, pw, 4 * scale, C_ACCENT); /* accent strip */
	gfx_text(px + 32 * scale, py + 28 * scale, p->head, C_TITLE, scale * 2);
	gfx_text(px + 32 * scale, py + 28 * scale + line * 2 + 8 * scale, fit(p->sub, maxc + 4, buf, sizeof buf), C_DIM,
	         scale);

	int y = py + top;
	for (int i = 0; i < np; i++, y += line + 6 * scale)
		gfx_text(px + 32 * scale, y, fit(p->para[i], maxc + 4, buf, sizeof buf), C_TEXT, scale);
	if (np)
		y += 18 * scale;
	for (int i = 0; i < p->n; i++, y += rowh) {
		bool on = i == sel;
		if (on)
			gfx_rect(px + 20 * scale, y, pw - 40 * scale, rowh, C_ACCENT);
		gfx_text(px + 36 * scale, y + 7 * scale, fit(p->rows[i], maxc, buf, sizeof buf),
		         on ? C_SELTEXT : C_TEXT, scale);
	}
	gfx_text(px + 32 * scale, py + ph - line - 26 * scale, p->foot, C_DIM, scale);
	const char *ver = "flboot 0.2";
	gfx_text(gfx_width() - gfx_text_width(ver, scale) - 16 * scale,
	         gfx_height() - line - 12 * scale, ver, C_DIM, scale);
}

static void draw_text(const struct page *p, int sel)
{
	ST->ConOut->ClearScreen(ST->ConOut);
	print(L"  ");
	print_ascii(p->head);
	print(L" -- ");
	print_ascii(p->sub);
	print(L"\r\n\r\n");
	for (int i = 0; i < para_lines(p); i++) {
		print(L"  ");
		print_ascii(p->para[i]);
		print(L"\r\n");
	}
	if (p->para)
		print(L"\r\n");
	for (int i = 0; i < p->n; i++) {
		print(i == sel ? L"  > " : L"    ");
		print_ascii(p->rows[i]);
		print(L"\r\n");
	}
	print(L"\r\n  ");
	print_ascii(p->foot);
	print(L"\r\n");
}

static void draw(const struct page *p, int sel)
{
	if (graphics)
		draw_gfx(p, sel);
	else
		draw_text(p, sel);
}

/* moves SEL for Up/Down/j/k; true if the key was one of them */
static bool move(EFI_INPUT_KEY key, int *sel, int n)
{
	if (key.ScanCode == SCAN_UP || key.UnicodeChar == 'k')
		*sel = *sel > 0 ? *sel - 1 : n - 1;
	else if (key.ScanCode == SCAN_DOWN || key.UnicodeChar == 'j')
		*sel = *sel < n - 1 ? *sel + 1 : 0;
	else
		return false;
	return true;
}

/* A page without a countdown: the chosen row, or -1 for Esc (and the last row if BACK). */
static int page_run(const struct page *p, int sel, bool back)
{
	for (;;) {
		draw(p, sel);
		UINTN which;
		BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &which);
		EFI_INPUT_KEY key;
		if (EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &key)))
			continue;
		if (key.ScanCode == SCAN_ESC)
			return -1;
		if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n')
			return back && sel == p->n - 1 ? -1 : sel;
		move(key, &sel, p->n);
	}
}

static int find_default(const struct config *c, const int *map, int n)
{
	if (!c->def)
		return 0;
	for (int r = 0; r < n; r++) {
		if (map[r] < 0)
			continue;
		const struct entry *e = &c->entries[map[r]];
		if (!memcmp(e->title, c->def, strlen(c->def) + 1) ||
		    (e->version && !memcmp(e->version, c->def, strlen(c->def) + 1)))
			return r;
	}
	return 0;
}

/* "Start an earlier state": the snapshots, then a notice. Returns the entry or -1. */
static int earlier_state(const struct config *c)
{
	static const char *const notice[] = {
		"This starts the system as it was when that snapshot was taken.",
		"It is read-only: nothing you change there is kept.",
		"Your own files in your home folder stay as they are.",
		"To keep that state, choose \"Make this the current state\"",
		"once it has started (Settings, Snapshots, or the notice).",
		"Restarting goes back to the normal system.",
		NULL,
	};
	struct page list = {"Start an earlier state", "Choose a snapshot of the system", "Up/Down: choose   Enter: next   Esc: back", NULL, {0}, 0};
	int map[MAX_ENTRIES];
	for (int i = 0; i < c->n; i++)
		if (c->entries[i].snapshot) {
			map[list.n] = i;
			list.rows[list.n++] = c->entries[i].title;
		}
	list.rows[list.n++] = "Back";
	int sel = 0;
	for (;;) {
		sel = page_run(&list, sel, true);
		if (sel < 0)
			return -1;
		struct page ask = {"Start an earlier state", c->entries[map[sel]].title, "Up/Down: choose   Enter: start   Esc: back",
		                   notice, {"Start this earlier state", "Back"}, 2};
		if (page_run(&ask, 0, true) == 0)
			return map[sel];
	}
}

int menu_run(struct config *c)
{
	struct page main = {"FreeLFS", "Choose what to start", "", NULL, {0}, 0};
	int map[MAX_ENTRIES + 1], nsnap = 0;
	for (int i = 0; i < c->n; i++) {
		if (c->entries[i].snapshot) {
			nsnap++;
			continue;
		}
		map[main.n] = i;
		main.rows[main.n++] = c->entries[i].title;
	}
	if (nsnap) {
		map[main.n] = -1;
		main.rows[main.n++] = "Start an earlier state";
	}
	int sel = find_default(c, map, main.n), left = c->timeout;
	if (left == 0 && map[sel] >= 0)
		return map[sel];
	graphics = gfx_init();
	scale = !graphics ? 1 : gfx_height() >= 1400 ? 3 : gfx_height() >= 900 ? 2 : 1;
	ST->ConOut->EnableCursor(ST->ConOut, false);

	EFI_EVENT timer, events[2];
	BS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &timer);
	BS->SetTimer(timer, TimerPeriodic, 10000000); /* 1 s, in 100 ns units */
	events[0] = ST->ConIn->WaitForKey;
	events[1] = timer;
	ST->ConIn->Reset(ST->ConIn, false);

	int result = -1;
	char buf[64];
	while (result < 0) {
		countdown_text(left, buf);
		main.foot = buf;
		draw(&main, sel);
		UINTN which;
		BS->WaitForEvent(left >= 0 ? 2 : 1, events, &which);
		if (which == 1) {
			if (left > 0 && --left == 0 && map[sel] >= 0)
				result = map[sel];
			continue;
		}
		EFI_INPUT_KEY key;
		if (EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &key)))
			continue;
		left = -1; /* any key stops the countdown */
		if (move(key, &sel, main.n))
			continue;
		if (key.UnicodeChar == '\r' || key.UnicodeChar == '\n')
			result = map[sel] >= 0 ? map[sel] : earlier_state(c);
	}
	BS->SetTimer(timer, TimerCancel, 0);
	BS->CloseEvent(timer);
	if (graphics)
		gfx_clear(0x000000);
	return result;
}
