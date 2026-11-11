/* flinstall -- the FreeLFS installer window ("FreeLFS install")
 * SPDX-License-Identifier: MIT
 *
 * Steps: language, keyboard, Wi-Fi, disk, account, time zone, summary, install.
 * The window never touches a disk. It starts flinstall-helper (setuid root for the
 * group seat, see helper.c) and talks to it in lines over a pipe pair: it asks for
 * the disks, sends the choices and the partition edits, asks the helper to check them
 * and finally to install, and shows the progress lines it gets back.
 *
 * Tests (no compositor, FLUI_PPM=FILE): FLINSTALL_HELPER="flinstall-helper --dry-run ..."
 * replaces the helper (with FLINSTALL_FAKE=DIR it lists the disks of a fixture);
 * FLINSTALL_STEP=0..7 starts on that step; FLINSTALL_TEST=a,b,c presets state, see
 * apply_test().
 */
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "wifi.h"

#define HELPER_DEFAULT "/usr/libexec/flinstall-helper"
#define RESTART_DEFAULT "/usr/bin/flpower restart"

enum { ST_LANG, ST_KBD, ST_WIFI, ST_DISK, ST_USER, ST_TZ, ST_SUMMARY, ST_INSTALL, ST_COUNT };
static const char *const step_name[ST_COUNT] = {"Language", "Keyboard", "Wi-Fi", "Disk", "Account", "Time zone", "Summary", "Install"};

enum {
	ID_LANG = 100, ID_KBD = 140, ID_TRY = 150, ID_BACK = 160, ID_NEXT = 161, ID_STEP = 170,
	ID_DISK = 200, ID_MODE = 230, ID_EDIT = 240, ID_RESCAN = 241,
	ID_NAME = 300, ID_FULL = 301, ID_PW = 302, ID_PW2 = 303,
	ID_SEARCH = 400, ID_MAP = 401, ID_POP = 410,
	ID_CONFIRM = 500, ID_RESTART = 501,
	ID_ROW = 600, ID_ED = 800, ID_EDSIZE = 900, ID_EDNAME = 901
};

/* ---- languages ------------------------------------------------------------------------------ */

struct lang {
	const char *id, *native, *english;
	int kbd;           /* index in fl_kbd_layouts: de us uk fr */
	const char *zone;  /* suggested time zone */
};

static const struct lang LANGS[] = {
	{"en_US.UTF-8", "English", "United States", 1, "America/New_York"},
	{"en_GB.UTF-8", "English", "United Kingdom", 2, "Europe/London"},
	{"de_DE.UTF-8", "Deutsch", "German", 0, "Europe/Berlin"},
	{"fr_FR.UTF-8", "Fran\xc3\xa7" "ais", "French", 3, "Europe/Paris"},
	{"es_ES.UTF-8", "Espa\xc3\xb1ol", "Spanish", 1, "Europe/Madrid"},
	{"it_IT.UTF-8", "Italiano", "Italian", 1, "Europe/Rome"},
	{"pt_BR.UTF-8", "Portugu\xc3\xaas (Brasil)", "Portuguese", 1, "America/Sao_Paulo"},
	{"nl_NL.UTF-8", "Nederlands", "Dutch", 1, "Europe/Amsterdam"},
	{"pl_PL.UTF-8", "Polski", "Polish", 1, "Europe/Warsaw"},
	{"cs_CZ.UTF-8", "\xc4\x8c" "e\xc5\xa1tina", "Czech", 0, "Europe/Prague"},
	{"sv_SE.UTF-8", "Svenska", "Swedish", 1, "Europe/Stockholm"},
	{"tr_TR.UTF-8", "T\xc3\xbcrk\xc3\xa7" "e", "Turkish", 1, "Europe/Istanbul"},
};
#define NLANG ((int)(sizeof LANGS / sizeof LANGS[0]))

/* ---- the disks the helper reported ------------------------------------------------------------ */

struct dpart {
	char dev[64], kind[8], fs[24], label[40], uuid[40];
	long long start, size;
	int busy;
};
struct dfree {
	long long start, size;
};
struct dsk {
	char dev[256], model[64], label[8];
	long long size;
	int removable, live, busy, ro;
	struct dpart p[32];
	int np;
	struct dfree f[32];
	int nf;
};

/* ---- the partition editor's working copy -------------------------------------------------------- */

enum { R_NONE, R_ROOT, R_BOOT, R_SWAP, R_DATA };
struct epart {
	char key[64];     /* existing device, or "newN" */
	int is_new;
	long long start, size;
	char kind[8];     /* new: esp root swap data; existing: esp linux swap other */
	char fsnow[24], label[40];
	int role, fmt;
	char fs[12];      /* what to format as: vfat btrfs ext4 swap */
	char dname[32];   /* data: /drives/NAME */
};

struct row { /* what the table shows: a partition or a gap */
	int part;           /* index in S.ep, or -1 */
	long long start, size;
};

struct S {
	fl_app *app;
	fl_win *win;
	fl_font *title, *head, *small, *mono;
	int step, maxstep;
	char err[300]; /* shown in the bottom bar */
	char test_edops[200]; /* tests: partition edits to make when the disks are here */
	/* language, keyboard */
	int lang, kbd, kbd_touched, tz_touched;
	char trial[128];
	/* Wi-Fi */
	struct wifi *wifi;
	/* helper */
	pid_t hpid;
	int hin, hout;
	char rbuf[16384];
	size_t rlen;
	int dry, helper_dead, force_live;
	char helper_msg[300];
	int expect_ok;
	char set_err[300];
	/* disks */
	struct dsk disks[8];
	int ndisks, disks_state; /* 0 asking, 1 ok, 2 failed */
	int sel_disk, mode;      /* mode 0 whole, 1 free, 2 manual */
	int editing;
	/* editor */
	struct epart ep[40];
	int nep, newtable, ndeleted, nnew;
	char deleted[32][64];
	int ed_sel;               /* row index */
	char ed_size[16];
	int ed_kind;
	char ed_msg[200];
	int ed_for;               /* disk the working copy is made from, -1 */
	/* account */
	char user[40], full[80], pw[FI_PW_MAX + 4], pw2[FI_PW_MAX + 4];
	int user_edited;
	struct fi_reserved res;
	/* time zone */
	fl_worldmap *map;
	fl_tzsearch search;
	char query[64];
	int zone;
	/* summary */
	int check_state; /* 0 not asked, 1 asked, 2 ok, 3 refused */
	int check_deferred;
	char check_err[300];
	char plan[16][200];
	int nplan;
	int confirm;
	/* install */
	int inst; /* 0 not started, 1 running, 2 done, 3 failed */
	int cur_step, total_steps, pct;
	char step_text[8][48];
	char log[10][180];
	int nlog;
	char fail_msg[300];
	char restart_err[160];
};

static struct S S;

static void redraw(void)
{
	if (S.win)
		fl_win_redraw(S.win);
}

/* ---- the helper ----------------------------------------------------------------------------------- */

static void hsend(const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b - 1, fmt, ap);
	va_end(ap);
	if (n < 0 || S.hin < 0)
		return;
	if ((size_t)n > sizeof b - 2)
		n = sizeof b - 2;
	b[n++] = '\n';
	ssize_t w = write(S.hin, b, (size_t)n);
	(void)w;
	explicit_bzero(b, sizeof b);
}

static void addlog(const char *s)
{
	if (S.nlog == 10) {
		memmove(S.log[0], S.log[1], 9 * sizeof S.log[0]);
		S.nlog--;
	}
	snprintf(S.log[S.nlog++], sizeof S.log[0], "%s", s);
}

static char *field(char **p)
{
	char *s = *p;
	if (!s)
		return "";
	char *t = strchr(s, '\t');
	if (t) {
		*t = 0;
		*p = t + 1;
	} else
		*p = NULL;
	return s;
}

static void ask_check(void);
static void test_edit(const char *ops);

static void on_line(char *line)
{
	char *p = line;
	char *cmd = field(&p);
	if (!strcmp(cmd, "HELLO")) {
		char *mode = field(&p);
		S.dry = !strcmp(mode, "dry-run") && !S.force_live;
	} else if (!strcmp(cmd, "DISK") && S.ndisks < 8) {
		struct dsk *d = &S.disks[S.ndisks++];
		memset(d, 0, sizeof *d);
		snprintf(d->dev, sizeof d->dev, "%s", field(&p));
		d->size = atoll(field(&p));
		d->removable = atoi(field(&p));
		char *flags = field(&p);
		d->live = strstr(flags, "live") != NULL;
		d->busy = strstr(flags, "busy") != NULL;
		d->ro = strstr(flags, "ro,") != NULL || !strcmp(flags + (strlen(flags) > 2 ? strlen(flags) - 2 : 0), "ro");
		if (strstr(flags, "gpt"))
			strcpy(d->label, "gpt");
		else if (strstr(flags, "dos"))
			strcpy(d->label, "dos");
		snprintf(d->model, sizeof d->model, "%s", field(&p));
	} else if (!strcmp(cmd, "PART") && S.ndisks) {
		struct dsk *d = &S.disks[S.ndisks - 1];
		if (d->np < 32) {
			struct dpart *q = &d->p[d->np++];
			memset(q, 0, sizeof *q);
			snprintf(q->dev, sizeof q->dev, "%s", field(&p));
			q->start = atoll(field(&p));
			q->size = atoll(field(&p));
			snprintf(q->kind, sizeof q->kind, "%s", field(&p));
			snprintf(q->fs, sizeof q->fs, "%s", field(&p));
			snprintf(q->label, sizeof q->label, "%s", field(&p));
			snprintf(q->uuid, sizeof q->uuid, "%s", field(&p));
			q->busy = atoi(field(&p));
			if (!strcmp(q->fs, "-"))
				q->fs[0] = 0;
			if (!strcmp(q->label, "-"))
				q->label[0] = 0;
		}
	} else if (!strcmp(cmd, "FREE") && S.ndisks) {
		struct dsk *d = &S.disks[S.ndisks - 1];
		if (d->nf < 32) {
			d->f[d->nf].start = atoll(field(&p));
			d->f[d->nf].size = atoll(field(&p));
			d->nf++;
		}
	} else if (!strcmp(cmd, "END")) {
		S.disks_state = 1;
		if (S.sel_disk >= S.ndisks)
			S.sel_disk = 0;
		while (S.sel_disk < S.ndisks - 1 && (S.disks[S.sel_disk].live || S.disks[S.sel_disk].busy || S.disks[S.sel_disk].ro))
			S.sel_disk++; /* the first disk you may use */
		if (S.test_edops[0])
			test_edit(S.test_edops);
		if (S.check_deferred) {
			S.check_deferred = 0;
			ask_check();
		}
	} else if (!strcmp(cmd, "PLAN")) {
		if (S.nplan < 16) {
			char *act = field(&p), *dev = field(&p), *size = field(&p), *fs = field(&p), *mp = field(&p);
			char *text = p;
			char *dn = strrchr(dev, '/');
			dn = dn && strncmp(act, "newtable", 8) ? dn + 1 : dev;
			if (!strcmp(act, "newtable"))
				snprintf(S.plan[S.nplan], sizeof S.plan[0], "Erase the partition table of %s", dev);
			else if (!strcmp(act, "delete"))
				snprintf(S.plan[S.nplan], sizeof S.plan[0], "Delete partition %s", dn);
			else if (!strcmp(act, "keep"))
				snprintf(S.plan[S.nplan], sizeof S.plan[0], "Keep %s as it is (%s%s%s)%s%s", dn, size, strcmp(fs, "-") ? ", " : "",
				         strcmp(fs, "-") ? fs : "", strcmp(mp, "-") ? ", used at " : "", strcmp(mp, "-") ? mp : "");
			else {
				const char *what = !strcmp(mp, "/") ? "system" : !strcmp(mp, "/boot") ? "boot" : !strcmp(mp, "swap") ? "swap" : "data";
				snprintf(S.plan[S.nplan], sizeof S.plan[0], "%s %s partition, %s%s%s%s%s",
				         !strcmp(act, "format") ? "Format the" : "Make a new", what, size, strcmp(fs, "swap") ? " " : "",
				         strcmp(fs, "swap") ? fs : "",
				         strcmp(mp, "-") && strcmp(mp, "swap") ? ", used at " : "", strcmp(mp, "-") && strcmp(mp, "swap") ? mp : "");
			}
			(void)text;
			S.nplan++;
		}
	} else if (!strcmp(cmd, "OK")) {
		if (S.expect_ok > 0)
			S.expect_ok--;
		else if (S.check_state == 1)
			S.check_state = 2;
	} else if (!strcmp(cmd, "ERR")) {
		char *m = p ? p : "";
		if (S.expect_ok > 0) {
			S.expect_ok--;
			snprintf(S.set_err, sizeof S.set_err, "%s", m);
		} else if (S.check_state == 1) {
			S.check_state = 3;
			snprintf(S.check_err, sizeof S.check_err, "%s", m);
		} else if (S.inst == 1)
			addlog(m);
	} else if (!strcmp(cmd, "STEP")) {
		S.cur_step = atoi(field(&p));
		S.total_steps = atoi(field(&p));
		if (S.cur_step >= 1 && S.cur_step <= 8)
			snprintf(S.step_text[S.cur_step - 1], sizeof S.step_text[0], "%s", p ? p : "");
		S.pct = 0;
	} else if (!strcmp(cmd, "PROGRESS")) {
		S.pct = atoi(field(&p));
	} else if (!strcmp(cmd, "CMD") || !strcmp(cmd, "LOG")) {
		addlog(p ? p : "");
	} else if (!strcmp(cmd, "DONE")) {
		S.inst = 2;
		S.cur_step = S.total_steps;
		S.pct = 100;
	} else if (!strcmp(cmd, "FAIL")) {
		S.inst = 3;
		snprintf(S.fail_msg, sizeof S.fail_msg, "%s", p ? p : "");
	} else if (line[0]) {
		/* not protocol: something the helper (or its loader) said on stderr */
		snprintf(S.helper_msg, sizeof S.helper_msg, "%s", line);
	}
}

static void helper_event(void *ud, int fd, short rev)
{
	char tmp[4096];
	ssize_t n = read(fd, tmp, sizeof tmp);
	if (n <= 0) {
		fl_fd_remove(S.app, fd);
		S.helper_dead = 1;
		if (S.disks_state == 0) {
			S.disks_state = 2;
		}
		if (S.inst == 1) {
			S.inst = 3;
			snprintf(S.fail_msg, sizeof S.fail_msg, "The installer helper stopped unexpectedly.");
		}
		if (S.check_state == 1) {
			S.check_state = 3;
			snprintf(S.check_err, sizeof S.check_err, "The installer helper is not running.");
		}
		redraw();
		return;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (tmp[i] == '\n') {
			S.rbuf[S.rlen] = 0;
			on_line(S.rbuf);
			S.rlen = 0;
		} else if (S.rlen < sizeof S.rbuf - 1 && tmp[i] != '\r')
			S.rbuf[S.rlen++] = tmp[i];
	}
	redraw();
}

static int helper_start(void)
{
	const char *cmd = getenv("FLINSTALL_HELPER");
	char buf[1024];
	snprintf(buf, sizeof buf, "%s", cmd && *cmd ? cmd : HELPER_DEFAULT);
	char *argv[24];
	int n = 0;
	for (char *t = strtok(buf, " "); t && n < 23; t = strtok(NULL, " "))
		argv[n++] = t;
	argv[n] = NULL;
	int in[2], out[2];
	if (pipe(in) < 0 || pipe(out) < 0)
		return -1;
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		dup2(out[1], 2);
		close(in[0]);
		close(in[1]);
		close(out[0]);
		close(out[1]);
		execvp(argv[0], argv);
		fprintf(stderr, "cannot start %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	S.hpid = pid;
	S.hin = in[1];
	S.hout = out[0];
	signal(SIGPIPE, SIG_IGN);
	fl_fd_add(S.app, S.hout, POLLIN, helper_event, NULL);
	return 0;
}

static void ask_disks(void)
{
	S.ndisks = 0;
	S.disks_state = 0;
	S.ed_for = -1;
	hsend("disks");
}

/* ---- sizes -------------------------------------------------------------------------------------------- */

static void fmt_size(long long b, char *o, size_t cap)
{
	if (b >= 1000 * FI_GIB)
		snprintf(o, cap, "%.1f TiB", (double)b / (1024 * FI_GIB));
	else if (b >= 10 * FI_GIB)
		snprintf(o, cap, "%.0f GiB", (double)b / FI_GIB);
	else if (b >= FI_GIB)
		snprintf(o, cap, "%.1f GiB", (double)b / FI_GIB);
	else
		snprintf(o, cap, "%lld MiB", b / FI_MIB);
}

static const struct dsk *cur_disk(void)
{
	return S.sel_disk >= 0 && S.sel_disk < S.ndisks ? &S.disks[S.sel_disk] : NULL;
}

static long long largest_free(const struct dsk *d)
{
	long long best = 0;
	if (!d->label[0])
		return d->size - 2 * FI_MIB;
	if (strcmp(d->label, "gpt"))
		return 0;
	for (int i = 0; i < d->nf; i++)
		if (d->f[i].size > best)
			best = d->f[i].size;
	return best;
}

static int disk_usable(const struct dsk *d)
{
	return !d->live && !d->busy && !d->ro;
}

/* ---- the partition editor: model ---------------------------------------------------------------------- */

static void ed_load(void)
{
	const struct dsk *d = cur_disk();
	S.nep = S.newtable = S.ndeleted = S.nnew = 0;
	S.ed_msg[0] = 0;
	S.ed_for = S.sel_disk;
	if (!d)
		return;
	for (int i = 0; i < d->np && S.nep < 40; i++) {
		struct epart *e = &S.ep[S.nep++];
		memset(e, 0, sizeof *e);
		snprintf(e->key, sizeof e->key, "%s", d->p[i].dev);
		e->start = d->p[i].start;
		e->size = d->p[i].size;
		snprintf(e->kind, sizeof e->kind, "%s", d->p[i].kind);
		snprintf(e->fsnow, sizeof e->fsnow, "%s", d->p[i].fs);
		snprintf(e->label, sizeof e->label, "%s", d->p[i].label);
	}
}

static long long disk_end(const struct dsk *d)
{
	return (d->size - 34 * 512) / FI_MIB * FI_MIB;
}

static int cmp_row(const void *a, const void *b)
{
	const struct row *x = a, *y = b;
	return x->start < y->start ? -1 : x->start > y->start;
}

/* partitions and gaps in disk order */
static int ed_rows(struct row *rows, int max)
{
	const struct dsk *d = cur_disk();
	int n = 0;
	if (!d)
		return 0;
	for (int i = 0; i < S.nep && n < max; i++)
		rows[n++] = (struct row){.part = i, .start = S.ep[i].start, .size = S.ep[i].size};
	qsort(rows, (size_t)n, sizeof *rows, cmp_row);
	struct row out[80];
	int m = 0;
	long long pos = FI_MIB, end = disk_end(d);
	for (int i = 0; i <= n && m < 78; i++) {
		long long to = i < n ? rows[i].start / FI_MIB * FI_MIB : end;
		if (to - pos >= FI_MIB)
			out[m++] = (struct row){.part = -1, .start = pos, .size = to - pos};
		if (i < n) {
			out[m++] = rows[i];
			long long e = (rows[i].start + rows[i].size + FI_MIB - 1) / FI_MIB * FI_MIB;
			if (e > pos)
				pos = e;
		}
	}
	if (m > max)
		m = max;
	memcpy(rows, out, (size_t)m * sizeof *rows);
	return m;
}

static void ed_set_role(struct epart *e, int role)
{
	/* one partition per role of / and /boot */
	for (int i = 0; i < S.nep; i++)
		if (&S.ep[i] != e && S.ep[i].role == role && (role == R_ROOT || role == R_BOOT))
			S.ep[i].role = R_NONE;
	e->role = role;
	switch (role) {
	case R_ROOT:
		e->fmt = 1;
		strcpy(e->fs, "btrfs");
		break;
	case R_BOOT:
		e->fmt = e->is_new || strcmp(e->fsnow, "vfat");
		strcpy(e->fs, "vfat");
		break;
	case R_SWAP:
		e->fmt = e->is_new || strcmp(e->fsnow, "swap");
		strcpy(e->fs, "swap");
		break;
	case R_DATA:
		if (!e->fs[0] || !strcmp(e->fs, "vfat") || !strcmp(e->fs, "swap"))
			strcpy(e->fs, "ext4");
		if (!e->dname[0])
			snprintf(e->dname, sizeof e->dname, "data");
		e->fmt = e->is_new || !e->fsnow[0];
		break;
	default:
		e->fmt = 0;
	}
}

static int ed_create(const struct row *r, const char *kind, long long size)
{
	if (S.nep >= 40)
		return -1;
	size = size / FI_MIB * FI_MIB;
	if (size < FI_MIB)
		size = FI_MIB;
	if (size > r->size)
		size = r->size;
	if (r->size - size < FI_MIB)
		size = r->size; /* what is left would be a sliver */
	struct epart *e = &S.ep[S.nep++];
	memset(e, 0, sizeof *e);
	snprintf(e->key, sizeof e->key, "new%d", ++S.nnew);
	e->is_new = 1;
	e->start = r->start;
	e->size = size;
	snprintf(e->kind, sizeof e->kind, "%s", kind);
	if (!strcmp(kind, "esp"))
		ed_set_role(e, R_BOOT);
	else if (!strcmp(kind, "root"))
		ed_set_role(e, R_ROOT);
	else if (!strcmp(kind, "swap"))
		ed_set_role(e, R_SWAP);
	else
		ed_set_role(e, R_DATA);
	return S.nep - 1;
}

static void ed_delete(int i)
{
	struct epart *e = &S.ep[i];
	if (e->is_new) {
		/* the helper numbers the new partitions in the order it is told: renumber */
		int gone = atoi(e->key + 3);
		memmove(&S.ep[i], &S.ep[i + 1], (size_t)(S.nep - i - 1) * sizeof S.ep[0]);
		S.nep--;
		S.nnew--;
		for (int k = 0; k < S.nep; k++)
			if (S.ep[k].is_new && atoi(S.ep[k].key + 3) > gone)
				snprintf(S.ep[k].key, sizeof S.ep[k].key, "new%d", atoi(S.ep[k].key + 3) - 1);
		return;
	}
	if (S.ndeleted < 32) {
		char key[64];
		snprintf(key, sizeof key, "%s", e->key);
		snprintf(S.deleted[S.ndeleted++], 64, "%s", key);
	}
	memmove(&S.ep[i], &S.ep[i + 1], (size_t)(S.nep - i - 1) * sizeof S.ep[0]);
	S.nep--;
}

static void ed_newtable(void)
{
	S.nep = S.nnew = 0;
	S.ndeleted = 0;
	S.newtable = 1;
	S.ed_sel = 0;
}

/* What is missing, as a sentence; NULL when the layout can be sent to the helper. */
static const char *ed_problem(void)
{
	static char b[200];
	const struct dsk *d = cur_disk();
	int root = -1, boot = -1;
	if (!d)
		return "No disk.";
	for (int i = 0; i < S.nep; i++) {
		if (S.ep[i].role == R_ROOT)
			root = i;
		if (S.ep[i].role == R_BOOT)
			boot = i;
	}
	if (!S.newtable && strcmp(d->label, "gpt"))
		return "This disk needs a GPT partition table: choose \"New partition table\".";
	if (root < 0)
		return "Choose a partition for the system (use it as \"/\").";
	if (S.ep[root].size < FI_MIN_ROOT_BYTES) {
		char sz[32];
		fmt_size(FI_MIN_ROOT_BYTES, sz, sizeof sz);
		snprintf(b, sizeof b, "The system partition needs at least %s.", sz);
		return b;
	}
	if (boot < 0)
		return "Choose a partition for the boot files (use it as \"/boot\").";
	if (S.ep[boot].size < FI_MIN_BOOT_BYTES)
		return "The boot partition needs at least 200 MiB.";
	if (!S.ep[boot].fmt && strcmp(S.ep[boot].fsnow, "vfat"))
		return "The boot partition must be FAT32: format it.";
	return NULL;
}

static void ed_send(void)
{
	hsend("part reset");
	S.expect_ok++;
	if (S.newtable) {
		hsend("part newtable");
		S.expect_ok++;
	}
	for (int i = 0; i < S.ndeleted; i++) {
		hsend("part delete %s", S.deleted[i]);
		S.expect_ok++;
	}
	/* creates in the order of their ids */
	for (int id = 1; id <= S.nnew; id++)
		for (int i = 0; i < S.nep; i++)
			if (S.ep[i].is_new && atoi(S.ep[i].key + 3) == id) {
				struct epart *e = &S.ep[i];
				hsend("part create %lld %lld %s", e->start, e->size,
				      !strcmp(e->kind, "esp") ? "esp" : !strcmp(e->kind, "swap") ? "swap" : e->role == R_ROOT ? "root" : "data");
				S.expect_ok++;
			}
	for (int i = 0; i < S.nep; i++) {
		struct epart *e = &S.ep[i];
		if (e->role == R_NONE)
			continue;
		char mp[64];
		if (e->role == R_ROOT)
			strcpy(mp, "/");
		else if (e->role == R_BOOT)
			strcpy(mp, "/boot");
		else if (e->role == R_SWAP)
			strcpy(mp, "swap");
		else
			snprintf(mp, sizeof mp, "/drives/%s", e->dname);
		if (e->fmt) {
			const char *lab = e->role == R_ROOT ? "freelfs" : e->role == R_BOOT ? "ESP" : e->role == R_DATA ? e->dname : "swap";
			hsend("part format %s %s %s", e->key, e->fs, lab);
			S.expect_ok++;
		}
		hsend("part mount %s %s", e->key, mp);
		S.expect_ok++;
	}
}

/* ---- sending the choices -------------------------------------------------------------------------------- */

static const char *tz_name(void)
{
	const fl_tzone *z = S.zone >= 0 ? fl_tz_get(S.zone) : NULL;
	return z ? z->name : "UTC";
}

static void send_config(void)
{
	const struct dsk *d = cur_disk();
	S.set_err[0] = 0;
	static const char *const modes[] = {"whole", "free", "manual"};
	hsend("set mode %s", modes[S.mode]);
	hsend("set disk %s", d ? d->dev : "");
	hsend("set user %s", S.user);
	hsend("set fullname %s", S.full[0] ? S.full : S.user);
	hsend("set password %s", S.pw);
	hsend("set lang %s", LANGS[S.lang].id);
	hsend("set keyboard %s", fl_kbd_layouts[S.kbd].id);
	hsend("set tz %s", tz_name());
	S.expect_ok += 8;
	if (S.mode == 2)
		ed_send();
}

static void ask_check(void)
{
	if (S.disks_state == 0) { /* the disk list is still on its way: ask when it is here */
		S.check_deferred = 1;
		S.check_state = 1;
		S.nplan = 0;
		return;
	}
	S.nplan = 0;
	S.check_state = 1;
	S.check_err[0] = 0;
	S.confirm = 0;
	send_config();
	hsend("check");
}

static void start_install(void)
{
	S.inst = 1;
	S.cur_step = 0;
	S.total_steps = 5;
	S.pct = 0;
	S.nlog = 0;
	memset(S.step_text, 0, sizeof S.step_text);
	hsend("install");
	/* the password is in the helper now: wipe our copy */
	explicit_bzero(S.pw, sizeof S.pw);
	explicit_bzero(S.pw2, sizeof S.pw2);
}

/* ---- movement ---------------------------------------------------------------------------------------------- */

static void goto_step(int st)
{
	S.step = st;
	if (st > S.maxstep)
		S.maxstep = st;
	S.err[0] = 0;
	wifi_show(S.wifi, st == ST_WIFI);
	if (st == ST_DISK && S.disks_state == 2 && !S.helper_dead)
		ask_disks();
	if (st == ST_SUMMARY)
		ask_check();
	if (st == ST_TZ && S.zone < 0)
		S.zone = fl_tz_find(LANGS[S.lang].zone) >= 0 ? fl_tz_find(LANGS[S.lang].zone) : fl_tz_find("UTC");
	redraw();
}

static void pick_lang(int i)
{
	S.lang = i;
	if (!S.kbd_touched) {
		S.kbd = LANGS[i].kbd;
		fl_app_set_layout(S.app, fl_kbd_layouts[S.kbd].xkb);
	}
	if (!S.tz_touched) {
		int z = fl_tz_find(LANGS[i].zone);
		if (z >= 0)
			S.zone = z;
	}
}

/* ---- small widgets ------------------------------------------------------------------------------------------- */

static void text_at(fl_ui *ui, fl_font *f, int x, int y, fl_color c, const char *s)
{
	fl_text(&ui->cv, f, x, y, c, s, -1);
}

/* word-wrapped dim text; returns the height used */
static int paragraph(fl_ui *ui, fl_font *f, int x, int y, int w, fl_color c, const char *s)
{
	int lh = fl_font_height(f) + 3, yy = y;
	char line[512];
	const char *p = s;
	while (*p) {
		size_t n = 0, last = 0;
		while (p[n] && p[n] != '\n') {
			if (p[n] == ' ')
				last = n;
			if (fl_text_width(f, p, (int)n + 1) > w) {
				if (last)
					n = last;
				break;
			}
			n++;
		}
		if (!p[n] || p[n] == '\n') {
			/* the rest fits */
		}
		if (n >= sizeof line)
			n = sizeof line - 1;
		memcpy(line, p, n);
		line[n] = 0;
		fl_text(&ui->cv, f, x, yy, c, line, -1);
		yy += lh;
		p += n;
		while (*p == ' ')
			p++;
		if (*p == '\n')
			p++;
	}
	return yy - y;
}

static int choice_row(fl_ui *ui, uint32_t id, fl_rect r, const char *title, const char *sub, int on, int enabled, int radio_style)
{
	int res = enabled ? fl_widget(ui, id, r, FL_FOCUSABLE) : 0;
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, on ? fl_th.select : (res & FL_HOVER) ? fl_th.raised : fl_th.panel);
	fl_stroke_rect(cv, r, 1, on || (res & FL_FOCUS) ? fl_th.accent : fl_th.line);
	int cx = r.x + 22, cy = r.y + r.h / 2;
	if (radio_style) {
		fl_stroke_rect(cv, fl_r(cx - 8, cy - 8, 16, 16), 1, enabled ? (on ? fl_th.accent : fl_th.dim) : fl_th.line);
		if (on)
			fl_fill_rect(cv, fl_r(cx - 4, cy - 4, 8, 8), fl_th.accent);
	}
	int tx = r.x + (radio_style ? 46 : 16);
	fl_color tc = !enabled ? fl_th.dim : on ? fl_th.strong : fl_th.text;
	if (sub && sub[0]) {
		int th = fl_font_height(ui->f_med), sh = fl_font_height(S.small);
		int top = r.y + (r.h - th - sh - 2) / 2;
		fl_text(cv, ui->f_med, tx, top, tc, title, -1);
		fl_text(cv, S.small, tx, top + th + 2, fl_th.dim, sub, -1);
	} else
		fl_text_in(cv, ui->f_med, fl_r(tx, r.y, r.w - (tx - r.x) - 8, r.h), FL_LEFT, 0, tc, title);
	return enabled && ((res & FL_CLICK) || ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)));
}

/* a small button-like toggle for the editor panel */
static int chip(fl_ui *ui, uint32_t id, fl_rect r, const char *label, int on, int enabled)
{
	int res = enabled ? fl_widget(ui, id, r, FL_FOCUSABLE) : 0;
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, on ? fl_th.select : (res & FL_HOVER) ? fl_th.raised : 0);
	fl_stroke_rect(cv, r, 1, on || (res & FL_FOCUS) ? fl_th.accent : fl_th.line);
	fl_text_in(cv, S.small, r, FL_CENTER, 0, !enabled ? fl_th.line : on ? fl_th.strong : fl_th.text, label);
	return enabled && ((res & FL_CLICK) || ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)));
}

/* A password field: squares, no clipboard. */
static int pw_field(fl_ui *ui, uint32_t id, fl_rect r, char *buf, size_t cap)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int focused = res & FL_FOCUS, enter = 0;
	size_t len = strlen(buf);
	if (focused) {
		if (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter) {
			enter = 1;
			ui->key = 0;
		} else if (ui->key == XKB_KEY_BackSpace) {
			while (len > 0 && ((unsigned char)buf[len - 1] & 0xc0) == 0x80)
				buf[--len] = 0;
			if (len > 0)
				buf[--len] = 0;
			ui->key = 0;
		} else if ((ui->mods & FL_MOD_CTRL) && (ui->key == 'u' || ui->key == 'U')) {
			explicit_bzero(buf, cap);
			len = 0;
			ui->key = 0;
		}
		size_t n = strlen(ui->text);
		if (n && len + n < FI_PW_MAX && !(ui->mods & FL_MOD_CTRL)) {
			memcpy(buf + len, ui->text, n + 1);
			len += n;
		}
		explicit_bzero(ui->text, sizeof ui->text);
	}
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.bg);
	fl_stroke_rect(cv, r, 1, focused ? fl_th.accent : fl_th.line);
	int x = r.x + 14, cy = r.y + r.h / 2, dots = 0;
	for (size_t i = 0; i < len; i++)
		if (((unsigned char)buf[i] & 0xc0) != 0x80)
			dots++;
	int maxd = (r.w - 28) / 14, first = dots > maxd ? dots - maxd : 0;
	for (int i = first; i < dots; i++)
		fl_fill_rect(cv, fl_r(x + (i - first) * 14, cy - 3, 7, 7), fl_th.text);
	if (focused)
		fl_fill_rect(cv, fl_r(dots ? x + (dots - first) * 14 : x, cy - 9, 1, 18), fl_th.text);
	return enter;
}

static void label_above(fl_ui *ui, int x, int y, const char *s)
{
	text_at(ui, S.small, x, y, fl_th.dim, s);
}

/* ---- the steps ------------------------------------------------------------------------------------------------ */

static void step_language(fl_ui *ui, fl_rect b)
{
	int gap = 10, cols = 3, tw = (b.w - gap * (cols - 1)) / cols, th = 58;
	for (int i = 0; i < NLANG; i++) {
		fl_rect t = fl_r(b.x + (i % cols) * (tw + gap), b.y + (i / cols) * (th + gap), tw, th);
		char sub[80];
		snprintf(sub, sizeof sub, "%s", LANGS[i].english);
		if (choice_row(ui, ID_LANG + (uint32_t)i, t, LANGS[i].native, sub, i == S.lang, 1, 0))
			pick_lang(i);
	}
	int y = b.y + ((NLANG + cols - 1) / cols) * (th + gap) + 14;
	paragraph(ui, S.small, b.x, y, b.w, fl_th.dim,
	          "FreeLFS itself is in English for now. This sets the language for dates and numbers, "
	          "and for programs that come with a translation.");
}

static void step_keyboard(fl_ui *ui, fl_rect b)
{
	fl_canvas *cv = &ui->cv;
	int x = b.x, w = b.w, y = b.y;
	int pick = fl_kbd_tiles(ui, ID_KBD, fl_r(x, y, w, 64), 8, S.kbd);
	if (pick >= 0 && pick != S.kbd) {
		S.kbd = pick;
		S.kbd_touched = 1;
		fl_app_set_layout(S.app, fl_kbd_layouts[pick].xkb);
		ui->focus = ID_TRY;
	}
	y += 64 + 22;
	fl_kbd_preview(ui, x, y, S.kbd);
	y += 3 * 42 + 2 * 5 + 22;
	label_above(ui, x, y, "Try it: type something here");
	y += 20;
	fl_textfield(ui, ID_TRY, fl_r(x, y, w, 40), S.trial, sizeof S.trial, NULL, S.mono);
	(void)cv;
}

static void step_wifi(fl_ui *ui, fl_rect b)
{
	wifi_draw(S.wifi, ui, b);
}

static void disk_tag(fl_ui *ui, fl_rect r, const char *s, fl_color c)
{
	int w = fl_text_width(S.small, s, -1) + 16;
	fl_rect t = fl_r(r.x + r.w - w - 14, r.y + (r.h - 22) / 2, w, 22);
	fl_stroke_rect(&ui->cv, t, 1, c);
	fl_text_in(&ui->cv, S.small, t, FL_CENTER, 0, c, s);
}

static void ed_view(fl_ui *ui, fl_rect b);

static void step_disk(fl_ui *ui, fl_rect b)
{
	fl_canvas *cv = &ui->cv;
	if (S.disks_state == 0) {
		text_at(ui, ui->f_reg, b.x, b.y + 8, fl_th.dim, "Looking for disks...");
		return;
	}
	if (S.disks_state == 2 || !S.ndisks) {
		int y = b.y + 8;
		text_at(ui, ui->f_med, b.x, y, S.disks_state == 2 ? fl_th.err : fl_th.text,
		        S.disks_state == 2 ? "The installer could not look at the disks." : "No disk was found.");
		y += 26;
		if (S.helper_msg[0])
			y += paragraph(ui, S.small, b.x, y, b.w, fl_th.dim, S.helper_msg) + 8;
		if (fl_button(ui, ID_RESCAN, fl_r(b.x, y + 8, 100, 36), "Try again", FL_BTN_OUTLINE) && !S.helper_dead)
			ask_disks();
		return;
	}
	if (S.editing) {
		if (S.ed_for != S.sel_disk)
			ed_load();
		ed_view(ui, b);
		return;
	}
	int y = b.y, rh = 60;
	int shown = S.ndisks > 4 ? 4 : S.ndisks;
	for (int i = 0; i < shown; i++) {
		struct dsk *d = &S.disks[i];
		fl_rect r = fl_r(b.x, y, b.w, rh);
		int enabled = disk_usable(d);
		char title[160], sz[32], sub[200] = "";
		fmt_size(d->size, sz, sizeof sz);
		snprintf(title, sizeof title, "%s   %s", d->model, sz);
		if (d->np) {
			size_t n = 0;
			n += (size_t)snprintf(sub + n, sizeof sub - n, "%d partition%s: ", d->np, d->np == 1 ? "" : "s");
			for (int k = 0; k < d->np && n < sizeof sub - 40; k++)
				n += (size_t)snprintf(sub + n, sizeof sub - n, "%s%s", k ? ", " : "", d->p[k].label[0] ? d->p[k].label : (d->p[k].fs[0] ? d->p[k].fs : "unknown"));
		} else
			snprintf(sub, sizeof sub, "%s", d->label[0] ? "No partitions" : "Empty, no partition table");
		int click = choice_row(ui, ID_DISK + (uint32_t)i, r, title, sub, i == S.sel_disk, enabled, 1);
		if (click && i != S.sel_disk) {
			S.sel_disk = i;
			S.ed_for = -1;
		}
		fl_icon_draw(cv, FL_ICON_DRIVE, r.x + r.w - 40, r.y + (r.h - 20) / 2 - 0, 20, fl_th.dim);
		if (d->live)
			disk_tag(ui, fl_r(r.x, r.y, r.w - 40, r.h), "Installation medium", fl_th.dim);
		else if (d->busy)
			disk_tag(ui, fl_r(r.x, r.y, r.w - 40, r.h), "In use", fl_th.dim);
		else if (d->ro)
			disk_tag(ui, fl_r(r.x, r.y, r.w - 40, r.h), "Read-only", fl_th.dim);
		else if (d->removable)
			disk_tag(ui, fl_r(r.x, r.y, r.w - 40, r.h), "Removable", fl_th.dim);
		y += rh + 8;
	}
	y += 8;
	const struct dsk *d = cur_disk();
	if (!d || !disk_usable(d)) {
		text_at(ui, ui->f_reg, b.x, y, fl_th.err, "None of these disks can be used.");
		return;
	}
	char free_sz[32], need[32], t[200];
	long long lf = largest_free(d), need_b = FI_ESP_BYTES + FI_MIN_ROOT_BYTES;
	fmt_size(lf, free_sz, sizeof free_sz);
	fmt_size(need_b, need, sizeof need);
	int free_ok = lf >= need_b && (!d->label[0] || !strcmp(d->label, "gpt"));
	int mh = 54;
	snprintf(t, sizeof t, "Erases everything on this disk. FreeLFS gets all of it.");
	if (choice_row(ui, ID_MODE, fl_r(b.x, y, b.w, mh), "Use the whole disk", t, S.mode == 0, 1, 1))
		S.mode = 0;
	y += mh + 8;
	if (!d->label[0])
		snprintf(t, sizeof t, "The disk is empty: this is the same as using the whole disk.");
	else if (strcmp(d->label, "gpt"))
		snprintf(t, sizeof t, "Not possible: this disk has an old (MBR) partition table.");
	else if (!free_ok)
		snprintf(t, sizeof t, "Not possible: the largest free space is %s; FreeLFS needs %s.", free_sz, need);
	else
		snprintf(t, sizeof t, "Nothing is erased. FreeLFS uses free space only (%s free in the largest piece).", free_sz);
	if (choice_row(ui, ID_MODE + 1, fl_r(b.x, y, b.w, mh), "Install next to existing partitions", t, S.mode == 1, free_ok || !d->label[0], 1))
		S.mode = 1;
	y += mh + 8;
	if (choice_row(ui, ID_MODE + 2, fl_r(b.x, y, b.w, mh), "Manual", "Make, delete and format partitions, and choose where each is used.", S.mode == 2, 1, 1))
		S.mode = 2;
	y += mh + 14;
	if (S.mode == 0 || (S.mode == 1 && !d->label[0])) {
		char list[400] = "";
		size_t n = 0;
		for (int k = 0; k < d->np && n < sizeof list - 60; k++) {
			char sz[32];
			fmt_size(d->p[k].size, sz, sizeof sz);
			n += (size_t)snprintf(list + n, sizeof list - n, "%s%s (%s)", k ? ", " : "", d->p[k].label[0] ? d->p[k].label : d->p[k].dev, sz);
		}
		char msg[600];
		if (d->np)
			snprintf(msg, sizeof msg, "Everything on %s will be erased: %s.", d->model, list);
		else
			snprintf(msg, sizeof msg, "Nothing is on %s now.", d->model);
		paragraph(ui, S.small, b.x, y, b.w, d->np ? fl_th.err : fl_th.dim, msg);
	} else if (S.mode == 2) {
		if (fl_button(ui, ID_EDIT, fl_r(b.x, y, 180, 38), "Edit partitions...", FL_BTN_OUTLINE)) {
			if (S.ed_for != S.sel_disk) {
				ed_load();
				S.ed_sel = 0;
			}
			S.editing = 1;
		}
		const char *pr = S.ed_for == S.sel_disk ? ed_problem() : "Open the editor and choose the system and boot partitions.";
		paragraph(ui, S.small, b.x + 196, y + 4, b.w - 196, pr ? fl_th.dim : fl_th.accent, pr ? pr : "The layout is ready.");
	} else {
		paragraph(ui, S.small, b.x, y, b.w, fl_th.dim, "Existing partitions are not touched.");
	}
}

/* ---- the partition editor ------------------------------------------------------------------------------------- */

static const fl_color *seg_colour(const struct epart *e, fl_color *tmp)
{
	if (!e)
		*tmp = fl_th.raised;
	else if (e->role == R_ROOT)
		*tmp = fl_th.accent;
	else if (e->role == R_BOOT)
		*tmp = fl_mix(fl_th.accent, fl_th.text, 90);
	else if (e->role == R_SWAP)
		*tmp = fl_mix(fl_th.accent, fl_th.bg, 150);
	else if (e->role == R_DATA)
		*tmp = fl_mix(fl_th.accent, fl_th.dim, 140);
	else
		*tmp = fl_th.dim;
	return tmp;
}

static const char *role_name(const struct epart *e, char *buf, size_t cap)
{
	switch (e->role) {
	case R_ROOT: return "System  /";
	case R_BOOT: return "Boot  /boot";
	case R_SWAP: return "Swap";
	case R_DATA: snprintf(buf, cap, "Data  /drives/%s", e->dname); return buf;
	}
	return "Not used";
}

static void ed_view(fl_ui *ui, fl_rect b)
{
	fl_canvas *cv = &ui->cv;
	const struct dsk *d = cur_disk();
	struct row rows[80];
	int nrows = ed_rows(rows, 80);
	if (S.ed_sel >= nrows)
		S.ed_sel = nrows - 1;
	if (S.ed_sel < 0)
		S.ed_sel = 0;
	int panel_w = 270, gap = 20, lw = b.w - panel_w - gap;
	int y = b.y;
	/* the disk as a bar */
	fl_rect bar = fl_r(b.x, y, lw, 22);
	fl_fill_rect(cv, bar, fl_th.bg);
	long long total = disk_end(d) - FI_MIB;
	if (total < 1)
		total = 1;
	for (int i = 0; i < nrows; i++) {
		int x0 = bar.x + (int)((rows[i].start - FI_MIB) * bar.w / total);
		int x1 = bar.x + (int)((rows[i].start + rows[i].size - FI_MIB) * bar.w / total);
		fl_color c;
		seg_colour(rows[i].part >= 0 ? &S.ep[rows[i].part] : NULL, &c);
		fl_fill_rect(cv, fl_r(x0, bar.y, x1 - x0 > 2 ? x1 - x0 - 1 : 1, bar.h), c);
		if (i == S.ed_sel)
			fl_stroke_rect(cv, fl_r(x0 - 1, bar.y - 1, x1 - x0 + 1, bar.h + 2), 2, fl_th.strong);
	}
	y += 22 + 14;
	/* the table */
	int rh = 34, head = 24;
	fl_rect tr = fl_r(b.x, y, lw, b.h - (y - b.y) - 52);
	fl_fill_rect(cv, tr, fl_th.panel);
	fl_stroke_rect(cv, tr, 1, fl_th.line);
	fl_clip(cv, tr);
	int cx0 = tr.x + 12, cx1 = tr.x + 150, cx2 = tr.x + 232, cx3 = tr.x + 330;
	text_at(ui, S.small, cx0, tr.y + 5, fl_th.dim, "Partition");
	text_at(ui, S.small, cx1, tr.y + 5, fl_th.dim, "Size");
	text_at(ui, S.small, cx2, tr.y + 5, fl_th.dim, "Holds");
	text_at(ui, S.small, cx3, tr.y + 5, fl_th.dim, "Use");
	int vis = (tr.h - head) / rh;
	int first = S.ed_sel >= vis ? S.ed_sel - vis + 1 : 0;
	for (int i = first; i < nrows && i < first + vis; i++) {
		fl_rect r = fl_r(tr.x, tr.y + head + (i - first) * rh, tr.w, rh);
		int res = fl_widget(ui, ID_ROW + (uint32_t)i, r, 0);
		if (res & FL_CLICK) {
			S.ed_sel = i;
			S.ed_msg[0] = 0;
		}
		if (i == S.ed_sel)
			fl_fill_rect(cv, r, fl_th.select);
		else if (res & FL_HOVER)
			fl_fill_rect(cv, r, fl_th.raised);
		char sz[32], nm[100], hold[64], usebuf[64];
		fmt_size(rows[i].size, sz, sizeof sz);
		fl_color tc = fl_th.text;
		if (rows[i].part >= 0) {
			struct epart *e = &S.ep[rows[i].part];
			const char *dn = strrchr(e->key, '/');
			snprintf(nm, sizeof nm, "%s", e->is_new ? "New partition" : (dn ? dn + 1 : e->key));
			if (e->fmt)
				snprintf(hold, sizeof hold, "%s (new)", e->fs);
			else
				snprintf(hold, sizeof hold, "%s", e->fsnow[0] ? e->fsnow : "-");
			fl_text_in(cv, S.small, fl_r(cx3, r.y, r.x + r.w - cx3 - 8, rh), FL_LEFT, 0, e->role ? fl_th.strong : fl_th.dim, role_name(e, usebuf, sizeof usebuf));
		} else {
			snprintf(nm, sizeof nm, "Free space");
			hold[0] = 0;
			tc = fl_th.dim;
		}
		fl_text_in(cv, ui->f_reg, fl_r(cx0, r.y, cx1 - cx0, rh), FL_LEFT, 0, tc, nm);
		fl_text_in(cv, ui->f_reg, fl_r(cx1, r.y, cx2 - cx1, rh), FL_LEFT, 0, tc, sz);
		fl_text_in(cv, ui->f_reg, fl_r(cx2, r.y, cx3 - cx2, rh), FL_LEFT, 0, fl_th.dim, hold);
	}
	if (S.ndeleted || S.newtable) {
		char del[300] = "";
		size_t n = 0;
		if (S.newtable)
			n += (size_t)snprintf(del + n, sizeof del - n, "All old partitions will be removed.");
		else {
			n += (size_t)snprintf(del + n, sizeof del - n, "Will be deleted: ");
			for (int i = 0; i < S.ndeleted && n < sizeof del - 40; i++) {
				const char *dn = strrchr(S.deleted[i], '/');
				n += (size_t)snprintf(del + n, sizeof del - n, "%s%s", i ? ", " : "", dn ? dn + 1 : S.deleted[i]);
			}
		}
		fl_text_in(cv, S.small, fl_r(tr.x, tr.y + tr.h - 28, tr.w, 28), FL_LEFT, 12, fl_th.err, del);
	}
	cv->clip = fl_win_content(S.win);
	/* the panel on the right */
	fl_rect pr = fl_r(b.x + lw + gap, b.y, panel_w, b.h - 52);
	fl_fill_rect(cv, pr, fl_th.panel);
	fl_stroke_rect(cv, pr, 1, fl_th.line);
	int px = pr.x + 16, pw = pr.w - 32, py = pr.y + 16;
	if (S.ed_sel < nrows) {
		struct row *r = &rows[S.ed_sel];
		char sz[32];
		fmt_size(r->size, sz, sizeof sz);
		if (r->part < 0) {
			text_at(ui, ui->f_semi, px, py, fl_th.strong, "Free space");
			py += 22;
			text_at(ui, S.small, px, py, fl_th.dim, sz);
			py += 28;
			label_above(ui, px, py, "Make a partition for");
			py += 20;
			static const char *const kinds[] = {"System", "Boot", "Swap", "Data"};
			int cw = (pw - 3 * 6) / 4;
			for (int k = 0; k < 4; k++)
				if (chip(ui, ID_ED + (uint32_t)k, fl_r(px + k * (cw + 6), py, cw, 28), kinds[k], S.ed_kind == k, 1))
					S.ed_kind = k;
			py += 28 + 14;
			label_above(ui, px, py, "Size in GiB (empty: all)");
			py += 20;
			fl_textfield(ui, ID_EDSIZE, fl_r(px, py, pw, 34), S.ed_size, sizeof S.ed_size, NULL, S.mono);
			py += 34 + 14;
			if (fl_button(ui, ID_ED + 10, fl_r(px, py, pw, 36), "Make partition", FL_BTN_ACCENT)) {
				static const char *const kk[] = {"root", "esp", "swap", "data"};
				long long want = r->size;
				if (S.ed_size[0]) {
					double g = atof(S.ed_size);
					want = g > 0 ? (long long)(g * FI_GIB) : 0;
				}
				if (want < FI_MIB)
					snprintf(S.ed_msg, sizeof S.ed_msg, "Enter a size of at least 1 MiB.");
				else {
					struct row copy = *r;
					int n = ed_create(&copy, kk[S.ed_kind], want);
					S.ed_msg[0] = 0;
					S.ed_size[0] = 0;
					(void)n;
				}
			}
			py += 36 + 10;
			if (!strcmp(d->label, "gpt") || S.newtable)
				;
			else
				py += paragraph(ui, S.small, px, py, pw, fl_th.dim, "This disk has no GPT partition table. Make a new one below first.");
		} else {
			struct epart *e = &S.ep[r->part];
			int busy = 0;
			for (int k = 0; k < d->np; k++)
				if (!strcmp(d->p[k].dev, e->key))
					busy = d->p[k].busy;
			text_at(ui, ui->f_semi, px, py, fl_th.strong, e->is_new ? "New partition" : e->key);
			py += 22;
			char info[120];
			snprintf(info, sizeof info, "%s%s%s", sz, e->fsnow[0] && !e->is_new ? ", holds " : "", e->is_new ? "" : e->fsnow);
			text_at(ui, S.small, px, py, fl_th.dim, info);
			py += 26;
			label_above(ui, px, py, "Use as");
			py += 20;
			static const char *const roles[] = {"Not used", "System /", "Boot /boot", "Swap", "Data"};
			int cw = (pw - 6) / 2;
			for (int k = 0; k < 5; k++) {
				fl_rect cr = fl_r(px + (k % 2) * (cw + 6), py + (k / 2) * 34, k == 4 ? cw : cw, 28);
				if (chip(ui, ID_ED + 20 + (uint32_t)k, cr, roles[k], e->role == k, !busy))
					ed_set_role(e, k);
			}
			py += 3 * 34 + 6;
			if (e->role == R_DATA) {
				label_above(ui, px, py, "Folder name in /drives");
				py += 20;
				fl_textfield(ui, ID_EDNAME, fl_r(px, py, pw, 34), e->dname, sizeof e->dname, NULL, S.mono);
				for (char *c = e->dname; *c; c++)
					if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'))
						*c = '_';
				py += 34 + 12;
				static const char *const fss[] = {"ext4", "btrfs"};
				int w2 = (pw - 6) / 2;
				for (int k = 0; k < 2; k++)
					if (chip(ui, ID_ED + 30 + (uint32_t)k, fl_r(px + k * (w2 + 6), py, w2, 28), fss[k], e->fmt && !strcmp(e->fs, fss[k]), 1)) {
						strcpy(e->fs, fss[k]);
						e->fmt = 1;
					}
				py += 28 + 8;
			}
			if (e->role != R_NONE && e->role != R_ROOT && !e->is_new) {
				int on = e->fmt, forced = e->role == R_ROOT || (e->role == R_BOOT && strcmp(e->fsnow, "vfat")) ||
				                           (e->role == R_SWAP && strcmp(e->fsnow, "swap")) || (e->role == R_DATA && !e->fsnow[0]);
				if (chip(ui, ID_ED + 35, fl_r(px, py, pw, 28), on ? "Format: contents will be erased" : "Keep what is on it", on, !forced))
					e->fmt = !e->fmt;
				py += 28 + 8;
			} else if (e->role == R_ROOT)
				py += paragraph(ui, S.small, px, py, pw, fl_th.dim, "Formatted as btrfs with the FreeLFS layout.") + 6;
			else if (e->role == R_NONE && !e->is_new)
				py += paragraph(ui, S.small, px, py, pw, fl_th.dim, "Left alone.") + 6;
			if (fl_button(ui, ID_ED + 40, fl_r(px, pr.y + pr.h - 52, pw, 36), "Delete partition", FL_BTN_OUTLINE) && !busy) {
				ed_delete(r->part);
				S.ed_sel = 0;
			}
			if (busy)
				text_at(ui, S.small, px, pr.y + pr.h - 72, fl_th.err, "In use: cannot be changed.");
		}
	}
	/* below */
	int by = b.y + b.h - 40;
	if (fl_button(ui, ID_ED + 50, fl_r(b.x, by, 190, 36), "New partition table", FL_BTN_OUTLINE)) {
		ed_newtable();
		snprintf(S.ed_msg, sizeof S.ed_msg, "All partitions of this disk are removed.");
	}
	if (fl_button(ui, ID_ED + 51, fl_r(b.x + 198, by, 80, 36), "Reset", FL_BTN_PLAIN)) {
		ed_load();
		S.ed_sel = 0;
	}
	if (fl_button(ui, ID_ED + 52, fl_r(b.x + b.w - 100, by, 100, 36), "Done", FL_BTN_ACCENT))
		S.editing = 0;
	const char *pb = ed_problem();
	const char *msg = S.ed_msg[0] ? S.ed_msg : pb;
	if (msg)
		fl_text_in(cv, S.small, fl_r(b.x + 290, by, b.w - 400, 36), FL_LEFT, 0, S.ed_msg[0] ? fl_th.err : fl_th.dim, msg);
	else
		fl_text_in(cv, S.small, fl_r(b.x + 290, by, b.w - 400, 36), FL_LEFT, 0, fl_th.accent, "The layout is ready.");
}

/* ---- account ------------------------------------------------------------------------------------------------- */

static const char *user_problem(void)
{
	static char b[200];
	return fi_user_error(S.user, &S.res, b, sizeof b);
}

static void step_user(fl_ui *ui, fl_rect b)
{
	int x = b.x, w = b.w > 520 ? 520 : b.w, y = b.y;
	label_above(ui, x, y, "Your name");
	fl_textfield(ui, ID_FULL, fl_r(x, y + 20, w, 38), S.full, sizeof S.full, "Your full name (optional)", ui->f_reg);
	y += 20 + 38 + 18;
	label_above(ui, x, y, "User name");
	char before[40];
	snprintf(before, sizeof before, "%s", S.user);
	int r = fl_textfield(ui, ID_NAME, fl_r(x, y + 20, w, 38), S.user, sizeof S.user, "for example anna", S.mono);
	if (r & FL_EDIT_CHANGED)
		S.user_edited = 1;
	y += 20 + 38;
	const char *up = S.user[0] ? user_problem() : NULL;
	if (up)
		text_at(ui, S.small, x, y + 6, fl_th.err, up);
	else if (S.user[0])
		text_at(ui, S.small, x, y + 6, fl_th.dim, "Your folder will be /usr/<name>.");
	else
		text_at(ui, S.small, x, y + 6, fl_th.dim, "Lowercase letters, digits, - and _. This is also your home folder's name.");
	y += 18 + 18;
	label_above(ui, x, y, "Password");
	int e1 = pw_field(ui, ID_PW, fl_r(x, y + 20, w, 38), S.pw, sizeof S.pw);
	y += 20 + 38 + 18;
	label_above(ui, x, y, "Password again");
	int e2 = pw_field(ui, ID_PW2, fl_r(x, y + 20, w, 38), S.pw2, sizeof S.pw2);
	y += 20 + 38;
	const char *pe = S.pw[0] ? fi_password_error(S.pw) : NULL;
	if (pe)
		text_at(ui, S.small, x, y + 6, fl_th.err, pe);
	else if (S.pw2[0] && strcmp(S.pw, S.pw2))
		text_at(ui, S.small, x, y + 6, fl_th.err, "The two passwords are not the same.");
	else if (S.pw[0] && !strcmp(S.pw, S.pw2))
		text_at(ui, S.small, x, y + 6, fl_th.accent, "The passwords match.");
	y += 18 + 20;
	paragraph(ui, S.small, x, y, w, fl_th.dim, FI_ROOT_NOTE);
	if (!S.user_edited && ui->focus != ID_NAME) {
		/* suggest a user name from the full name until the person types one */
		char sug[40] = "";
		size_t n = 0;
		for (const char *c = S.full; *c && n < 31; c++) {
			if (*c == ' ')
				break;
			if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'))
				sug[n++] = *c;
			else if (*c >= 'A' && *c <= 'Z')
				sug[n++] = (char)(*c + 32);
		}
		sug[n] = 0;
		if (strcmp(sug, S.user))
			snprintf(S.user, sizeof S.user, "%s", sug);
	}
	(void)e1;
	(void)e2;
	(void)before;
}

static int user_ok(void)
{
	return S.user[0] && !user_problem() && !fi_password_error(S.pw) && !strcmp(S.pw, S.pw2);
}

/* ---- time zone ------------------------------------------------------------------------------------------------- */

static void step_tz(fl_ui *ui, fl_rect b)
{
	fl_canvas *cv = &ui->cv;
	int x = b.x, w = b.w, y = b.y;
	int picked = fl_tzsearch_field(ui, ID_SEARCH, fl_r(x, y, w, 36), &S.search, S.query, sizeof S.query);
	y += 36 + 10;
	int mw = w > 760 ? 760 : w, mh = fl_worldmap_height(mw);
	fl_rect mr = fl_r(x + (w - mw) / 2, y, mw, mh);
	int mx = ui->mx, my = ui->my;
	if (fl_tzsearch_covers(&S.search, ui->mx, ui->my))
		ui->mx = ui->my = -10000;
	int hover = -1;
	fl_worldmap_set_24h(S.map, 1);
	int clicked = fl_worldmap_draw(ui, ID_MAP, mr, S.map, S.zone, &hover);
	ui->mx = mx;
	ui->my = my;
	int pop = fl_tzsearch_popup(ui, ID_POP, &S.search, S.query);
	if (pop >= 0)
		picked = pop;
	if (picked < 0)
		picked = clicked;
	if (picked >= 0) {
		S.zone = picked;
		S.tz_touched = 1;
	}
	y += mh + 14;
	const fl_tzone *z = S.zone >= 0 ? fl_tz_get(S.zone) : NULL;
	if (z) {
		struct tm tm;
		int off = 0;
		char abbr[16] = "", t[16], o[24], line[200];
		fl_tz_local(S.zone, fl_tz_time(), &tm, &off, abbr, sizeof abbr);
		fl_tz_fmt_time(&tm, 1, t, sizeof t);
		fl_tz_fmt_offset(off, o, sizeof o);
		text_at(ui, S.head, x, y, fl_th.strong, t);
		int tw = fl_text_width(S.head, t, -1);
		snprintf(line, sizeof line, "%s, %s   %s   %s", z->city, z->country, abbr, o);
		text_at(ui, ui->f_reg, x + tw + 16, y + 1, fl_th.text, line);
		text_at(ui, S.small, x, y + 22, fl_th.dim, z->name);
	} else
		text_at(ui, ui->f_reg, x, y, fl_th.dim, "Click the map or search for a city.");
	(void)cv;
}

/* ---- summary ----------------------------------------------------------------------------------------------------- */

static int summary_erases(void)
{
	const struct dsk *d = cur_disk();
	if (!d)
		return 0;
	if (S.mode == 0)
		return d->np > 0;
	if (S.mode == 2)
		return S.newtable || S.ndeleted || 1;
	return 0;
}

static void step_summary(fl_ui *ui, fl_rect b)
{
	fl_canvas *cv = &ui->cv;
	int x = b.x, w = b.w, y = b.y, lw = 150;
	const struct dsk *d = cur_disk();
	char sz[32];
	struct { const char *k; char v[260]; } rows[6];
	int n = 0;
	rows[n].k = "Language";
	snprintf(rows[n++].v, 260, "%s (%s)", LANGS[S.lang].native, LANGS[S.lang].english);
	rows[n].k = "Keyboard";
	snprintf(rows[n++].v, 260, "%s, %s", fl_kbd_layouts[S.kbd].name, fl_kbd_layouts[S.kbd].where);
	rows[n].k = "Disk";
	if (d) {
		fmt_size(d->size, sz, sizeof sz);
		snprintf(rows[n++].v, 260, "%s, %s: %s", d->model, sz, S.mode == 0 ? "the whole disk" : S.mode == 1 ? "next to existing partitions" : "partitions set by hand");
	} else
		snprintf(rows[n++].v, 260, "-");
	rows[n].k = "Account";
	snprintf(rows[n++].v, 260, "%s (%s)", S.user, S.full[0] ? S.full : S.user);
	rows[n].k = "Time zone";
	snprintf(rows[n++].v, 260, "%s", tz_name());
	for (int i = 0; i < n; i++) {
		text_at(ui, ui->f_reg, x, y, fl_th.dim, rows[i].k);
		fl_text_in(cv, ui->f_reg, fl_r(x + lw, y - 4, w - lw, 24), FL_LEFT, 0, fl_th.text, rows[i].v);
		y += 28;
	}
	y += 10;
	fl_fill_rect(cv, fl_r(x, y, w, 1), fl_th.line);
	y += 14;
	text_at(ui, ui->f_semi, x, y, fl_th.strong, "What will happen");
	y += 26;
	if (S.check_state == 1)
		text_at(ui, ui->f_reg, x, y, fl_th.dim, "Checking...");
	else if (S.check_state == 3) {
		y += paragraph(ui, ui->f_reg, x, y, w, fl_th.err, S.check_err);
		y += 6;
		paragraph(ui, S.small, x, y, w, fl_th.dim, "Go back and change it.");
	} else if (S.check_state == 2) {
		for (int i = 0; i < S.nplan; i++) {
			int erase = !strncmp(S.plan[i], "Erase", 5) || !strncmp(S.plan[i], "Delete", 6) || !strncmp(S.plan[i], "Format", 6);
			fl_fill_rect(cv, fl_r(x, y + 7, 5, 5), erase ? fl_th.err : fl_th.accent);
			fl_text_in(cv, ui->f_reg, fl_r(x + 16, y - 4, w - 16, 24), FL_LEFT, 0, erase ? fl_th.err : fl_th.text, S.plan[i]);
			y += 24;
		}
		text_at(ui, S.small, x + 16, y + 2, fl_th.dim, "Then copy FreeLFS, install the boot loader and set up the account.");
		y += 30;
		if (S.dry) {
			text_at(ui, S.small, x, y, fl_th.dim, "Dry run: the helper only lists what it would do. Nothing is written.");
			y += 22;
		}
		if (summary_erases() && !S.dry) {
			fl_rect cr = fl_r(x, y + 4, w, 36);
			int res = fl_widget(ui, ID_CONFIRM, cr, FL_FOCUSABLE);
			fl_stroke_rect(cv, fl_r(x, y + 12, 18, 18), 1, S.confirm ? fl_th.accent : fl_th.dim);
			if (S.confirm)
				fl_fill_rect(cv, fl_r(x + 4, y + 16, 10, 10), fl_th.accent);
			text_at(ui, ui->f_reg, x + 30, y + 12, fl_th.text, "I understand that the changes above cannot be undone.");
			if ((res & FL_CLICK) || ((res & FL_FOCUS) && ui->key == XKB_KEY_space))
				S.confirm = !S.confirm;
		}
	}
}

static int summary_ok(void)
{
	return S.check_state == 2 && (!summary_erases() || S.dry || S.confirm);
}

/* ---- install ------------------------------------------------------------------------------------------------------ */

static void restart_machine(void)
{
	const char *cmd = getenv("FLINSTALL_RESTART");
	char buf[256];
	snprintf(buf, sizeof buf, "%s", cmd && *cmd ? cmd : RESTART_DEFAULT);
	char *argv[8];
	int n = 0;
	for (char *t = strtok(buf, " "); t && n < 7; t = strtok(NULL, " "))
		argv[n++] = t;
	argv[n] = NULL;
	pid_t pid = fork();
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (pid < 0)
		snprintf(S.restart_err, sizeof S.restart_err, "Could not restart: %s", strerror(errno));
}

static void step_install(fl_ui *ui, fl_rect b)
{
	fl_canvas *cv = &ui->cv;
	int x = b.x, w = b.w, y = b.y;
	static const char *const names[] = {"Preparing the disk", "Formatting", "Copying the system", "Installing the boot loader", "Writing to the disk"};
	for (int i = 0; i < 5; i++) {
		int done = S.inst == 2 || S.cur_step > i + 1, now = S.cur_step == i + 1 && S.inst == 1, failed = S.cur_step == i + 1 && S.inst == 3;
		fl_rect dot = fl_r(x, y + 3, 14, 14);
		fl_stroke_rect(cv, dot, 1, failed ? fl_th.err : done || now ? fl_th.accent : fl_th.line);
		if (done)
			fl_fill_rect(cv, fl_inset(dot, 3, 3), fl_th.accent);
		else if (now)
			fl_fill_rect(cv, fl_inset(dot, 5, 5), fl_th.accent);
		else if (failed)
			fl_fill_rect(cv, fl_inset(dot, 4, 4), fl_th.err);
		text_at(ui, ui->f_reg, x + 28, y, failed ? fl_th.err : done || now ? fl_th.text : fl_th.dim, names[i]);
		if (now && i == 2) {
			char p[16];
			snprintf(p, sizeof p, "%d %%", S.pct);
			fl_text_in(cv, ui->f_reg, fl_r(x, y - 4, w, 24), FL_RIGHT, 0, fl_th.dim, p);
		}
		y += 28;
	}
	y += 10;
	fl_rect pb = fl_r(x, y, w, 10);
	fl_fill_rect(cv, pb, fl_th.bg);
	int total = S.total_steps ? S.total_steps : 5;
	int part = S.inst == 2 ? 100 : (S.cur_step > 0 ? ((S.cur_step - 1) * 100 + (S.cur_step == 3 ? S.pct : 0)) / total : 0);
	if (S.cur_step == 3 && S.inst == 1)
		part = ((S.cur_step - 1) * 100 + S.pct) / total;
	fl_fill_rect(cv, fl_r(pb.x, pb.y, pb.w * part / 100, pb.h), S.inst == 3 ? fl_th.err : fl_th.accent);
	y += 10 + 22;
	if (S.inst == 2) {
		text_at(ui, ui->f_semi, x, y, fl_th.strong, S.dry ? "Dry run finished. Nothing was written." : "FreeLFS is installed.");
		y += 28;
		if (!S.dry)
			y += paragraph(ui, ui->f_reg, x, y, w, fl_th.dim, "Remove the installation medium, then restart. Your account is ready to log in.");
		y += 12;
		if (!S.dry && fl_button(ui, ID_RESTART, fl_r(x, y, 160, 40), "Restart now", FL_BTN_ACCENT))
			restart_machine();
		if (S.restart_err[0])
			text_at(ui, S.small, x + 176, y + 12, fl_th.err, S.restart_err);
		y += 54;
	} else if (S.inst == 3) {
		text_at(ui, ui->f_semi, x, y, fl_th.err, "The installation did not finish.");
		y += 26;
		y += paragraph(ui, ui->f_reg, x, y, w, fl_th.text, S.fail_msg) + 6;
		y += paragraph(ui, S.small, x, y, w, fl_th.dim, "The disk may be partly changed. Start the installer again to retry.") + 10;
	} else {
		text_at(ui, ui->f_reg, x, y, fl_th.dim, "This takes a few minutes. Please do not turn the computer off.");
		y += 28;
	}
	/* the helper's last lines */
	int lh = fl_font_height(S.mono) + 2;
	int rows = (b.y + b.h - y) / lh;
	if (rows > 10)
		rows = 10;
	int from = S.nlog > rows ? S.nlog - rows : 0;
	fl_rect lr = fl_r(x, y, w, rows * lh + 12);
	if (rows > 1 && S.nlog) {
		fl_fill_rect(cv, lr, fl_th.bg);
		fl_clip(cv, lr);
		for (int i = from; i < S.nlog; i++) {
			char fit[200];
			fl_text_fit(S.mono, S.log[i], w - 20, fit, sizeof fit);
			text_at(ui, S.mono, x + 10, y + 6 + (i - from) * lh, fl_th.dim, fit);
		}
		cv->clip = fl_win_content(S.win);
	}
}

/* ---- the window ---------------------------------------------------------------------------------------------------- */

static int can_next(const char **why)
{
	*why = NULL;
	switch (S.step) {
	case ST_DISK: {
		const struct dsk *d = cur_disk();
		if (S.disks_state != 1 || !d || !disk_usable(d)) {
			*why = "Choose a disk.";
			return 0;
		}
		if (S.editing) {
			*why = "Press Done first.";
			return 0;
		}
		if (S.mode == 2 && (S.ed_for != S.sel_disk || ed_problem())) {
			*why = "Set up the partitions first.";
			return 0;
		}
		if (S.mode == 1 && d->label[0] && !(largest_free(d) >= FI_ESP_BYTES + FI_MIN_ROOT_BYTES && !strcmp(d->label, "gpt"))) {
			*why = "There is not enough free space on this disk.";
			return 0;
		}
		return 1;
	}
	case ST_USER:
		if (!user_ok()) {
			*why = "Fill in a user name and the same password twice.";
			return 0;
		}
		return 1;
	case ST_SUMMARY:
		return summary_ok();
	case ST_INSTALL:
		return 0;
	}
	return 1;
}

static void draw_sidebar(fl_ui *ui, fl_rect sb)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, sb, fl_th.panel);
	fl_fill_rect(cv, fl_r(sb.x + sb.w - 1, sb.y, 1, sb.h), fl_th.line);
	text_at(ui, S.head, sb.x + 24, sb.y + 26, fl_th.strong, "FreeLFS");
	text_at(ui, S.small, sb.x + 24, sb.y + 48, fl_th.dim, S.dry ? "install (dry run)" : "install");
	int y = sb.y + 92;
	for (int i = 0; i < ST_COUNT; i++) {
		fl_rect r = fl_r(sb.x, y, sb.w - 1, 36);
		int cur = i == S.step, done = i < S.step || (S.inst == 2 && i == ST_INSTALL);
		int can_jump = i < S.step && S.inst == 0 && S.step != ST_INSTALL;
		int res = can_jump ? fl_widget(ui, ID_STEP + (uint32_t)i, r, FL_FOCUSABLE) : 0;
		if (cur)
			fl_fill_rect(cv, r, fl_th.select);
		else if (res & FL_HOVER)
			fl_fill_rect(cv, r, fl_th.raised);
		if (cur)
			fl_fill_rect(cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
		fl_rect dot = fl_r(sb.x + 24, y + 11, 14, 14);
		fl_stroke_rect(cv, dot, 1, cur || done ? fl_th.accent : fl_th.line);
		if (done)
			fl_fill_rect(cv, fl_inset(dot, 3, 3), fl_th.accent);
		fl_text_in(cv, ui->f_reg, fl_r(sb.x + 50, y, sb.w - 56, 36), FL_LEFT, 0, cur ? fl_th.strong : done ? fl_th.text : fl_th.dim, step_name[i]);
		if (res & FL_CLICK)
			goto_step(i);
		y += 38;
	}
}

static const char *const step_sub[ST_COUNT] = {
	"Which language do you speak?", "Which keyboard do you have?", "Connect to a network (you can skip this)",
	"Where should FreeLFS go?", "Who will use this computer?", "Where are you?",
	"Check it before anything is written", "Installing",
};

static void draw(fl_ui *ui, void *ud)
{
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(S.win);
	fl_fill_rect(cv, c, fl_th.bg);
	int sw = 208;
	fl_rect sb = fl_r(c.x, c.y, sw, c.h);
	draw_sidebar(ui, sb);
	int mx = c.x + sw + 40, mw = c.w - sw - 80;
	fl_text(cv, S.title, mx, c.y + 28, fl_th.strong, S.step == ST_INSTALL && S.inst == 2 ? "Done" : step_name[S.step], -1);
	const char *sub = step_sub[S.step];
	if (S.step == ST_INSTALL)
		sub = S.inst == 2 ? "All set" : S.inst == 3 ? "Something went wrong" : "Installing";
	fl_text(cv, ui->f_reg, mx, c.y + 28 + 34, fl_th.dim, sub, -1);
	int top = c.y + 28 + 34 + 34, bar_h = 72;
	fl_rect body = fl_r(mx, top, mw, c.y + c.h - bar_h - top - 10);
	/* the bottom bar */
	fl_fill_rect(cv, fl_r(c.x + sw, c.y + c.h - bar_h, c.w - sw, 1), fl_th.line);
	const char *why;
	int next_ok = can_next(&why);
	fl_rect clip = cv->clip;
	switch (S.step) {
	case ST_LANG: step_language(ui, body); break;
	case ST_KBD: step_keyboard(ui, body); break;
	case ST_WIFI: step_wifi(ui, body); break;
	case ST_DISK: step_disk(ui, body); break;
	case ST_USER: step_user(ui, body); break;
	case ST_TZ: step_tz(ui, body); break;
	case ST_SUMMARY: step_summary(ui, body); break;
	case ST_INSTALL: step_install(ui, body); break;
	}
	cv->clip = clip;
	int by = c.y + c.h - bar_h + 16;
	if (S.step != ST_INSTALL) {
		if (S.step > 0 && fl_button(ui, ID_BACK, fl_r(mx, by, 110, 40), "Back", FL_BTN_OUTLINE)) {
			if (S.step == ST_DISK && S.editing)
				S.editing = 0;
			else
				goto_step(S.step - 1);
		}
		const char *label = S.step == ST_SUMMARY ? (summary_erases() ? "Erase and install" : "Install") : "Continue";
		int w = S.step == ST_SUMMARY ? 190 : 130;
		fl_rect nr = fl_r(mx + mw - w, by, w, 40);
		if (next_ok) {
			if (fl_button(ui, ID_NEXT, nr, label, FL_BTN_ACCENT)) {
				if (S.step == ST_SUMMARY) {
					start_install();
					goto_step(ST_INSTALL);
				} else
					goto_step(S.step + 1);
			}
		} else {
			fl_fill_rect(cv, nr, fl_th.raised);
			fl_text_in(cv, ui->f_semi, nr, FL_CENTER, 0, fl_th.dim, label);
		}
		if (!next_ok && why)
			fl_text_in(cv, S.small, fl_r(mx + 130, by, mw - w - 150, 40), FL_RIGHT, 0, fl_th.dim, why);
	}
}

static void closed(fl_win *w, void *ud)
{
	if (S.inst == 1)
		return; /* not while the disk is being written */
	fl_app_quit(S.app);
}

/* ---- tests -------------------------------------------------------------------------------------------------------- */

/* partition edits for screenshots: del<N> (partition N of the disk), mk:KIND:GIB on the first free space,
 * role<N>:R (row N gets role R), newtable. Separated by ';' */
static void test_edit(const char *ops)
{
	char buf[200];
	snprintf(buf, sizeof buf, "%s", ops);
	S.test_edops[0] = 0;
	ed_load();
	for (char *t = strtok(buf, ";"); t; t = strtok(NULL, ";")) {
		struct row rows[80];
		int n = ed_rows(rows, 80);
		if (!strcmp(t, "newtable"))
			ed_newtable();
		else if (!strncmp(t, "del", 3)) {
			int k = atoi(t + 3);
			if (k >= 0 && k < n && rows[k].part >= 0)
				ed_delete(rows[k].part);
		} else if (!strncmp(t, "mk:", 3)) {
			char kind[8];
			double g = 0;
			if (sscanf(t + 3, "%7[a-z]:%lf", kind, &g) == 2)
				for (int i = 0; i < n; i++)
					if (rows[i].part < 0) {
						ed_create(&rows[i], kind, (long long)(g * FI_GIB));
						break;
					}
		} else if (!strncmp(t, "role", 4)) {
			int k = atoi(t + 4), r = strchr(t, ':') ? atoi(strchr(t, ':') + 1) : 0;
			if (k >= 0 && k < n && rows[k].part >= 0)
				ed_set_role(&S.ep[rows[k].part], r);
		}
	}
}

/* FLINSTALL_TEST=item,item,...: disk=N mode=N user=NAME pw=PW edit=1 plan=KIND
 * running=PCT done failed. Only with FLUI_PPM. */
static void apply_test(const char *spec)
{
	char buf[512];
	snprintf(buf, sizeof buf, "%s", spec);
	for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
		if (!strncmp(t, "disk=", 5))
			S.sel_disk = atoi(t + 5);
		else if (!strncmp(t, "mode=", 5))
			S.mode = atoi(t + 5);
		else if (!strncmp(t, "user=", 5)) {
			snprintf(S.user, sizeof S.user, "%s", t + 5);
			S.user_edited = 1;
		} else if (!strncmp(t, "full=", 5))
			snprintf(S.full, sizeof S.full, "%s", t + 5);
		else if (!strncmp(t, "pw=", 3)) {
			snprintf(S.pw, sizeof S.pw, "%s", t + 3);
			snprintf(S.pw2, sizeof S.pw2, "%s", t + 3);
		} else if (!strncmp(t, "pw2=", 4))
			snprintf(S.pw2, sizeof S.pw2, "%s", t + 4);
		else if (!strncmp(t, "lang=", 5))
			pick_lang(atoi(t + 5));
		else if (!strncmp(t, "kbd=", 4)) {
			S.kbd = atoi(t + 4);
			S.kbd_touched = 1;
			fl_app_set_layout(S.app, fl_kbd_layouts[S.kbd].xkb);
		} else if (!strncmp(t, "zone=", 5)) {
			S.zone = fl_tz_find(t + 5);
			S.tz_touched = 1;
		} else if (!strcmp(t, "edit"))
			S.editing = 1;
		else if (!strncmp(t, "sel=", 4))
			S.ed_sel = atoi(t + 4);
		else if (!strncmp(t, "edops=", 6)) {
			snprintf(S.test_edops, sizeof S.test_edops, "%s", t + 6);
			for (char *c = S.test_edops; *c; c++)
				if (*c == '+')
					*c = ',';
		}
		else if (!strncmp(t, "running=", 8)) {
			S.inst = 1;
			S.cur_step = 3;
			S.total_steps = 5;
			S.pct = atoi(t + 8);
		} else if (!strcmp(t, "failed")) {
			S.inst = 3;
			S.cur_step = 3;
			S.total_steps = 5;
			snprintf(S.fail_msg, sizeof S.fail_msg, "copying the system failed (exit 1)");
		} else if (!strcmp(t, "confirm"))
			S.confirm = 1;
		else if (!strcmp(t, "done")) {
			S.inst = 2;
			S.cur_step = S.total_steps = 5;
			S.pct = 100;
		} else if (!strcmp(t, "nodry"))
			S.force_live = 1; /* screenshots: look like the real thing */
	}
}

static void test_log(void)
{
	static const char *const l[] = {
		"sfdisk --quiet --wipe always --wipe-partitions always /dev/sda", "wipefs --all --quiet /dev/sda1",
		"mkfs.fat -F 32 -n ESP -i 3FA287FE /dev/sda1", "mount -t overlay overlay /run/flinstall.k2Hd9a/stage/@",
		"write /etc/fstab (file systems by UUID)", "mkfs.btrfs -f -q -L freelfs --rootdir /run/flinstall.k2Hd9a/stage /dev/sda2"};
	for (int i = 0; i < 6; i++)
		addlog(l[i]);
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		fprintf(stderr, "usage: flinstall\nThe FreeLFS installer (needs flinstall-helper)\n");
		return strcmp(argv[1], "-h") && strcmp(argv[1], "--help") ? 2 : 0;
	}
	S.app = fl_app_new(NULL, NULL);
	if (!S.app) {
		fprintf(stderr, "flinstall: cannot connect to a Wayland compositor\n");
		return 1;
	}
	fl_conf_load();
	S.title = fl_font_ui(600, 24);
	S.head = fl_font_ui(600, 17);
	S.small = fl_font_ui(400, 12);
	S.mono = fl_font_mono(400, 13);
	S.zone = -1;
	S.sel_disk = 0;
	S.ed_for = -1;
	S.lang = 0;
	int cur = fl_kbd_conf_read();
	S.kbd = cur >= 0 ? cur : 1;
	fl_app_set_layout(S.app, fl_kbd_layouts[S.kbd].xkb);
	if (cur >= 0)
		S.kbd_touched = 1;
	S.map = fl_worldmap_new();
	pick_lang(0); /* the first guess for the time zone; the keyboard keeps what the live system has */
	/* the names the live system already uses */
	fi_reserved_file(&S.res, "/etc/passwd");
	fi_reserved_file(&S.res, "/etc/group");
	fi_reserved_dir(&S.res, "/usr");

	fl_win_opts o = {
		.title = "FreeLFS install", .app_id = "flinstall", .w = 1080, .h = 720, .min_w = 900, .min_h = 640,
		.decorated = 1, .resizable = 1, .font_px = 14, .draw = draw, .close = closed, .ud = NULL,
	};
	S.win = fl_win_new(S.app, &o);
	if (!S.win)
		return 1;
	S.wifi = wifi_new(S.app, S.head, S.small);
	wifi_set_win(S.wifi, S.win);
	if (helper_start() < 0) {
		S.disks_state = 2;
		snprintf(S.helper_msg, sizeof S.helper_msg, "Could not start the installer helper.");
	} else
		ask_disks();
	if (fl_app_ppm(S.app)) {
		const char *t = getenv("FLINSTALL_TEST");
		if (t)
			apply_test(t);
		const char *st = getenv("FLINSTALL_STEP");
		if (st) {
			int n = atoi(st);
			if (n > 0)
				S.maxstep = n;
			if (n == ST_INSTALL && S.inst)
				test_log();
			goto_step(n);
		}
	}
	fl_app_run(S.app);
	if (S.hin >= 0) {
		hsend("quit");
		close(S.hin);
	}
	if (S.hpid > 0)
		waitpid(S.hpid, NULL, 0);
	explicit_bzero(S.pw, sizeof S.pw);
	explicit_bzero(S.pw2, sizeof S.pw2);
	wifi_free(S.wifi);
	fl_app_free(S.app);
	return 0;
}
