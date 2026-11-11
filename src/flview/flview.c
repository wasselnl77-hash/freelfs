/* flview -- the FreeLFS image viewer ("Images")
 * SPDX-License-Identifier: MIT
 *
 * Opens a picture or a folder of pictures (PNG, JPEG, WebP), keeps the whole
 * folder at hand: arrow keys go to the next and previous image, a strip of
 * thumbnails shows the neighbours, zoom and pan work with the wheel and the
 * mouse, R rotates, Del moves the file to the Trash, W makes it the desktop
 * wallpaper. Decoders are libpng, libjpeg(-turbo) and libwebp directly, no
 * gdk-pixbuf: a JPEG is decoded at 1/2, 1/4 or 1/8 size when that is enough
 * (thumbnails, very large photos), so memory and time stay small.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/demux.h>

#include <flui.h>

#define TOOL_H 44
#define STRIP_H 82
#define CELL_W 84
#define THUMB_W 76
#define THUMB_H 58
#define MAX_PIXELS (24u * 1024 * 1024) /* decode larger pictures smaller */
#define MAX_THUMBS 600

/* ---- pictures ---------------------------------------------------------------------------- */

/* ARGB8888, premultiplied alpha */
struct img {
	int w, h;
	uint32_t *px;
};

static void img_free(struct img *im)
{
	free(im->px);
	im->px = NULL;
	im->w = im->h = 0;
}

static void premultiply(struct img *im)
{
	size_t n = (size_t)im->w * (size_t)im->h;
	for (size_t i = 0; i < n; i++) {
		uint32_t p = im->px[i], a = p >> 24;
		if (a == 255)
			continue;
		uint32_t r = ((p >> 16) & 255) * a / 255, g = ((p >> 8) & 255) * a / 255, b = (p & 255) * a / 255;
		im->px[i] = a << 24 | r << 16 | g << 8 | b;
	}
}

/* Box-filter resize to w x h (both <= the source). */
static int img_shrink(const struct img *src, struct img *dst, int w, int h)
{
	dst->px = malloc((size_t)w * (size_t)h * 4);
	if (!dst->px)
		return -1;
	dst->w = w;
	dst->h = h;
	for (int y = 0; y < h; y++) {
		int y0 = (int)((long long)y * src->h / h), y1 = (int)((long long)(y + 1) * src->h / h);
		if (y1 <= y0)
			y1 = y0 + 1;
		for (int x = 0; x < w; x++) {
			int x0 = (int)((long long)x * src->w / w), x1 = (int)((long long)(x + 1) * src->w / w);
			if (x1 <= x0)
				x1 = x0 + 1;
			unsigned a = 0, r = 0, g = 0, b = 0, n = 0;
			for (int yy = y0; yy < y1; yy++) {
				const uint32_t *row = src->px + (size_t)yy * (size_t)src->w;
				for (int xx = x0; xx < x1; xx++) {
					uint32_t p = row[xx];
					a += p >> 24;
					r += (p >> 16) & 255;
					g += (p >> 8) & 255;
					b += p & 255;
					n++;
				}
			}
			dst->px[(size_t)y * (size_t)w + (size_t)x] =
				(a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
		}
	}
	return 0;
}

/* view transform: flip left-right first, then rotate clockwise by rot * 90 degrees */
struct orient {
	int rot, flip;
};

/* JPEG ---------------------------------------------------------------------------------------- */

struct jerr {
	struct jpeg_error_mgr pub;
	jmp_buf jb;
};

static void jerr_exit(j_common_ptr c)
{
	longjmp(((struct jerr *)c->err)->jb, 1);
}

static void jerr_silent(j_common_ptr c, int level) {}

static int exif_orientation(const unsigned char *d, size_t n)
{
	/* d: APP1 payload without the marker length; "Exif\0\0" TIFF header ... */
	if (n < 14 || memcmp(d, "Exif\0\0", 6))
		return 1;
	const unsigned char *t = d + 6;
	size_t tn = n - 6;
	int le = t[0] == 'I';
	if (!le && t[0] != 'M')
		return 1;
#define U16(p) (le ? (unsigned)((p)[0] | (p)[1] << 8) : (unsigned)((p)[0] << 8 | (p)[1]))
#define U32(p) (le ? (unsigned)((p)[0] | (p)[1] << 8 | (p)[2] << 16 | (unsigned)(p)[3] << 24) \
	               : (unsigned)((unsigned)(p)[0] << 24 | (p)[1] << 16 | (p)[2] << 8 | (p)[3]))
	unsigned off = U32(t + 4);
	if (off + 2 > tn)
		return 1;
	unsigned cnt = U16(t + off);
	for (unsigned i = 0; i < cnt && off + 2 + (i + 1) * 12 <= tn; i++) {
		const unsigned char *e = t + off + 2 + i * 12;
		if (U16(e) == 0x0112) {
			unsigned v = U16(e + 8);
			return v >= 1 && v <= 8 ? (int)v : 1;
		}
	}
	return 1;
}

static struct orient orient_from_exif(int o)
{
	switch (o) {
	case 2: return (struct orient){0, 1};
	case 3: return (struct orient){2, 0};
	case 4: return (struct orient){2, 1};
	case 5: return (struct orient){3, 1};
	case 6: return (struct orient){1, 0};
	case 7: return (struct orient){1, 1};
	case 8: return (struct orient){3, 0};
	default: return (struct orient){0, 0};
	}
}

/* want > 0: a decode of at least want px on the long side is enough */
static int load_jpeg(const char *path, int want, struct img *out, struct orient *ori, int *full_w, int *full_h)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;
	struct jpeg_decompress_struct c;
	struct jerr je;
	c.err = jpeg_std_error(&je.pub);
	je.pub.error_exit = jerr_exit;
	je.pub.emit_message = jerr_silent;
	out->px = NULL;
	if (setjmp(je.jb)) {
		jpeg_destroy_decompress(&c);
		fclose(fp);
		free(out->px);
		out->px = NULL;
		return -1;
	}
	jpeg_create_decompress(&c);
	jpeg_stdio_src(&c, fp);
	jpeg_save_markers(&c, JPEG_APP0 + 1, 0xffff);
	jpeg_read_header(&c, TRUE);
	*full_w = (int)c.image_width;
	*full_h = (int)c.image_height;
	if (ori) {
		*ori = (struct orient){0, 0};
		for (jpeg_saved_marker_ptr m = c.marker_list; m; m = m->next)
			if (m->marker == JPEG_APP0 + 1) {
				*ori = orient_from_exif(exif_orientation(m->data, m->data_length));
				break;
			}
	}
	if (c.jpeg_color_space == JCS_CMYK || c.jpeg_color_space == JCS_YCCK)
		c.out_color_space = JCS_CMYK;
	else
		c.out_color_space = JCS_EXT_BGRA;
	int denom = 1;
	size_t pixels = (size_t)c.image_width * c.image_height;
	long long longest = c.image_width > c.image_height ? c.image_width : c.image_height;
	while (denom < 8 && ((want > 0 && longest / (denom * 2) >= want) || (pixels / ((size_t)denom * denom) > MAX_PIXELS)))
		denom *= 2;
	c.scale_num = 1;
	c.scale_denom = (unsigned)denom;
	if (want > 0) {
		c.dct_method = JDCT_IFAST;
		c.do_fancy_upsampling = FALSE;
	}
	jpeg_start_decompress(&c);
	out->w = (int)c.output_width;
	out->h = (int)c.output_height;
	out->px = malloc((size_t)out->w * (size_t)out->h * 4);
	if (!out->px)
		longjmp(je.jb, 1);
	if (c.out_color_space == JCS_CMYK) {
		unsigned char *row = malloc((size_t)out->w * 4);
		if (!row)
			longjmp(je.jb, 1);
		while (c.output_scanline < c.output_height) {
			uint32_t *d = out->px + (size_t)c.output_scanline * (size_t)out->w;
			unsigned char *rp = row;
			jpeg_read_scanlines(&c, &rp, 1);
			for (int x = 0; x < out->w; x++) {
				/* Adobe CMYK is stored inverted */
				unsigned k = row[x * 4 + 3];
				d[x] = 0xff000000u | (row[x * 4] * k / 255) << 16 | (row[x * 4 + 1] * k / 255) << 8 |
				       (row[x * 4 + 2] * k / 255);
			}
		}
		free(row);
	} else {
		while (c.output_scanline < c.output_height) {
			unsigned char *rp = (unsigned char *)(out->px + (size_t)c.output_scanline * (size_t)out->w);
			jpeg_read_scanlines(&c, &rp, 1);
		}
	}
	jpeg_finish_decompress(&c);
	jpeg_destroy_decompress(&c);
	fclose(fp);
	return 0;
}

/* PNG ----------------------------------------------------------------------------------------- */

static int load_png(const char *path, struct img *out, int *full_w, int *full_h)
{
	png_image pi;
	memset(&pi, 0, sizeof pi);
	pi.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&pi, path))
		return -1;
	*full_w = (int)pi.width;
	*full_h = (int)pi.height;
	if ((size_t)pi.width * pi.height > 4 * MAX_PIXELS) {
		png_image_free(&pi);
		return -1;
	}
	pi.format = PNG_FORMAT_BGRA;
	out->w = (int)pi.width;
	out->h = (int)pi.height;
	out->px = malloc(PNG_IMAGE_SIZE(pi));
	if (!out->px) {
		png_image_free(&pi);
		return -1;
	}
	if (!png_image_finish_read(&pi, NULL, out->px, 0, NULL)) {
		png_image_free(&pi);
		free(out->px);
		out->px = NULL;
		return -1;
	}
	return 0;
}

/* WebP ---------------------------------------------------------------------------------------- */

static unsigned char *slurp(const char *path, size_t *len)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (n <= 0 || n > 512L * 1024 * 1024) {
		fclose(fp);
		return NULL;
	}
	unsigned char *b = malloc((size_t)n);
	if (!b || fread(b, 1, (size_t)n, fp) != (size_t)n) {
		free(b);
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	*len = (size_t)n;
	return b;
}

static int load_webp(const char *path, int want, struct img *out, int *full_w, int *full_h)
{
	size_t len;
	unsigned char *data = slurp(path, &len);
	if (!data)
		return -1;
	WebPDecoderConfig cfg;
	int rc = -1;
	if (!WebPInitDecoderConfig(&cfg) || WebPGetFeatures(data, len, &cfg.input) != VP8_STATUS_OK)
		goto done;
	*full_w = cfg.input.width;
	*full_h = cfg.input.height;
	if (cfg.input.has_animation) {
		WebPData wd = {data, len};
		WebPAnimDecoderOptions ao;
		WebPAnimDecoderOptionsInit(&ao);
		ao.color_mode = MODE_BGRA;
		WebPAnimDecoder *ad = WebPAnimDecoderNew(&wd, &ao);
		if (!ad)
			goto done;
		uint8_t *buf;
		int ts;
		if (WebPAnimDecoderGetNext(ad, &buf, &ts)) {
			out->w = cfg.input.width;
			out->h = cfg.input.height;
			out->px = malloc((size_t)out->w * (size_t)out->h * 4);
			if (out->px) {
				memcpy(out->px, buf, (size_t)out->w * (size_t)out->h * 4);
				rc = 0;
			}
		}
		WebPAnimDecoderDelete(ad);
		goto done;
	}
	int w = cfg.input.width, h = cfg.input.height;
	size_t pixels = (size_t)w * (size_t)h;
	long long longest = w > h ? w : h;
	if ((want > 0 && longest > want * 2) || pixels > MAX_PIXELS) {
		double f = 1.0;
		if (want > 0 && longest > want * 2)
			f = (double)want * 1.0 / (double)longest;
		if (pixels > MAX_PIXELS && sqrt((double)MAX_PIXELS / (double)pixels) < f)
			f = sqrt((double)MAX_PIXELS / (double)pixels);
		if (want > 0 && f == (double)want / (double)longest)
			f = (double)want * 2.0 / (double)longest > 1.0 ? 1.0 : (double)want * 2.0 / (double)longest;
		w = (int)(cfg.input.width * f);
		h = (int)(cfg.input.height * f);
		if (w < 1)
			w = 1;
		if (h < 1)
			h = 1;
		cfg.options.use_scaling = 1;
		cfg.options.scaled_width = w;
		cfg.options.scaled_height = h;
	}
	out->px = malloc((size_t)w * (size_t)h * 4);
	if (!out->px)
		goto done;
	out->w = w;
	out->h = h;
	cfg.output.colorspace = MODE_BGRA;
	cfg.output.is_external_memory = 1;
	cfg.output.u.RGBA.rgba = (uint8_t *)out->px;
	cfg.output.u.RGBA.stride = w * 4;
	cfg.output.u.RGBA.size = (size_t)w * (size_t)h * 4;
	if (WebPDecode(data, len, &cfg) != VP8_STATUS_OK) {
		free(out->px);
		out->px = NULL;
		goto done;
	}
	rc = 0;
done:
	free(data);
	return rc;
}

/* ---- the folder ---------------------------------------------------------------------------- */

enum { FMT_NONE, FMT_JPEG, FMT_PNG, FMT_WEBP };

static int format_of(const char *name)
{
	const char *e = strrchr(name, '.');
	if (!e)
		return FMT_NONE;
	if (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg") || !strcasecmp(e, ".jpe"))
		return FMT_JPEG;
	if (!strcasecmp(e, ".png"))
		return FMT_PNG;
	if (!strcasecmp(e, ".webp"))
		return FMT_WEBP;
	return FMT_NONE;
}

/* Decode a file. want > 0: only about that many px on the long side are needed (thumbnails). */
static int load_file(const char *path, int want, struct img *out, struct orient *ori, int *full_w, int *full_h)
{
	int fmt = format_of(path), rc = -1;
	*out = (struct img){0};
	if (ori)
		*ori = (struct orient){0, 0};
	switch (fmt) {
	case FMT_JPEG: rc = load_jpeg(path, want, out, ori, full_w, full_h); break;
	case FMT_PNG: rc = load_png(path, out, full_w, full_h); break;
	case FMT_WEBP: rc = load_webp(path, want, out, full_w, full_h); break;
	}
	if (rc < 0 || !out->px || out->w < 1 || out->h < 1) {
		img_free(out);
		return -1;
	}
	premultiply(out);
	return 0;
}

struct thumb {
	struct img im;
	int state; /* 0 not tried, 1 ready, -1 failed */
};

struct file {
	char *name;
	off_t size;
	struct thumb th;
};

/* natural order: "img2" before "img10", case blind */
static int natcmp(const char *a, const char *b)
{
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			while (*a == '0' && isdigit((unsigned char)a[1]))
				a++;
			while (*b == '0' && isdigit((unsigned char)b[1]))
				b++;
			const char *ea = a, *eb = b;
			while (isdigit((unsigned char)*ea))
				ea++;
			while (isdigit((unsigned char)*eb))
				eb++;
			if (ea - a != eb - b)
				return (ea - a) < (eb - b) ? -1 : 1;
			int c = strncmp(a, b, (size_t)(ea - a));
			if (c)
				return c;
			a = ea;
			b = eb;
			continue;
		}
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb)
			return ca < cb ? -1 : 1;
		a++;
		b++;
	}
	return *a ? 1 : *b ? -1 : 0;
}

static int cmp_file(const void *a, const void *b)
{
	const struct file *x = a, *y = b;
	int c = natcmp(x->name, y->name);
	return c ? c : strcmp(x->name, y->name);
}

/* ---- application state ------------------------------------------------------------------ */

enum {
	ID_CANVAS = 1, ID_PREV, ID_NEXT, ID_ZOUT, ID_ZIN, ID_FIT, ID_ONE, ID_ROT, ID_DEL, ID_WALL, ID_STRIP,
	ID_STRIPAREA
};

struct V {
	fl_app *app;
	fl_win *win;
	fl_font *small;
	fl_timer *msg_timer, *thumb_timer;
	char dir[PATH_MAX];
	struct file *files;
	int n, cur;
	/* the picture on screen */
	struct img img;
	int full_w, full_h;      /* size in the file (img may be decoded smaller) */
	struct orient ori;       /* from the file (EXIF), plus the user's turns */
	int load_err;
	unsigned gen;            /* changes with every picture, rotation, ... */
	double zoom;             /* screen px per picture px (of the full size); 0: fit */
	double ox, oy;           /* top left of the rotated picture on the canvas */
	int fitted;              /* zoom follows the window size */
	struct img lv;           /* shrunken copy of img for zoom < 1 */
	int lv_shift;
	unsigned lv_gen;
	/* rendered canvas cache */
	uint32_t *vbuf;
	size_t vcap;
	int vw, vh;
	struct { unsigned gen; double zoom, ox, oy; int w, h, dark, rot, flip; } vkey;
	int vvalid;
	/* strip, drag, messages */
	int show_strip;
	double strip_off;
	int drag, drag_x, drag_y;
	char msg[200];
	int msg_err;
	int nthumbs;
	int test_zoom_pct;
	const char *script; /* FLVIEW_KEYS="Right,Right,Delete": key presses to run in the first frame (tests) */
};

static void say(struct V *v, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(v->msg, sizeof v->msg, fmt, ap);
	va_end(ap);
	v->msg_err = err;
	fl_timer_arm(v->msg_timer, 3500, 0);
}

static void msg_clear(void *ud)
{
	struct V *v = ud;
	v->msg[0] = 0;
	fl_win_redraw(v->win);
}

static void path_of(const struct V *v, int i, char *out, size_t cap)
{
	if (!strcmp(v->dir, "/"))
		snprintf(out, cap, "/%s", v->files[i].name);
	else
		snprintf(out, cap, "%s/%s", v->dir, v->files[i].name);
}

static int scan_dir(struct V *v, const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return -1;
	snprintf(v->dir, sizeof v->dir, "%s", dir);
	struct dirent *e;
	int cap = 0;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || format_of(e->d_name) == FMT_NONE)
			continue;
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(p, &st) < 0 || !S_ISREG(st.st_mode))
			continue;
		if (v->n == cap) {
			cap = cap ? cap * 2 : 64;
			v->files = realloc(v->files, (size_t)cap * sizeof *v->files);
			if (!v->files)
				return -1;
		}
		v->files[v->n++] = (struct file){.name = strdup(e->d_name), .size = st.st_size};
	}
	closedir(d);
	if (v->n)
		qsort(v->files, (size_t)v->n, sizeof *v->files, cmp_file);
	return 0;
}

/* ---- viewing geometry ------------------------------------------------------------------- */

static void rotated_size(const struct V *v, int *rw, int *rh)
{
	if (v->ori.rot & 1) {
		*rw = v->full_h;
		*rh = v->full_w;
	} else {
		*rw = v->full_w;
		*rh = v->full_h;
	}
}

static double fit_zoom(const struct V *v, fl_rect c)
{
	int rw, rh;
	rotated_size(v, &rw, &rh);
	if (rw < 1 || rh < 1)
		return 1;
	double z = fmin((double)c.w / rw, (double)c.h / rh);
	return z > 1 ? 1 : z;
}

static void clamp_view(struct V *v, fl_rect c)
{
	int rw, rh;
	rotated_size(v, &rw, &rh);
	double iw = rw * v->zoom, ih = rh * v->zoom;
	if (iw <= c.w)
		v->ox = (c.w - iw) / 2;
	else
		v->ox = fmin(0, fmax(c.w - iw, v->ox));
	if (ih <= c.h)
		v->oy = (c.h - ih) / 2;
	else
		v->oy = fmin(0, fmax(c.h - ih, v->oy));
}

static void zoom_to(struct V *v, fl_rect c, double z, double fx, double fy)
{
	if (z < 0.02)
		z = 0.02;
	if (z > 32)
		z = 32;
	/* keep the picture point under (fx, fy) in place */
	v->ox = fx - (fx - v->ox) * z / v->zoom;
	v->oy = fy - (fy - v->oy) * z / v->zoom;
	v->zoom = z;
	v->fitted = 0;
	clamp_view(v, c);
}

static void set_fit(struct V *v, fl_rect c)
{
	v->zoom = fit_zoom(v, c);
	v->fitted = 1;
	clamp_view(v, c);
}

/* ---- loading the picture on screen ------------------------------------------------------ */

static void drop_picture(struct V *v)
{
	img_free(&v->img);
	img_free(&v->lv);
	v->lv_gen = 0;
	v->vvalid = 0;
}

static void open_index(struct V *v, int i)
{
	if (i < 0 || i >= v->n)
		return;
	drop_picture(v);
	v->cur = i;
	v->gen++;
	v->load_err = 0;
	v->fitted = 1;
	v->full_w = v->full_h = 0;
	char p[PATH_MAX];
	path_of(v, i, p, sizeof p);
	if (load_file(p, 0, &v->img, &v->ori, &v->full_w, &v->full_h) < 0) {
		v->load_err = 1;
		v->ori = (struct orient){0, 0};
		return;
	}
	/* the decoder may have returned a reduced size: the view works in file pixels */
	if (v->full_w < 1 || v->full_h < 1) {
		v->full_w = v->img.w;
		v->full_h = v->img.h;
	}
	fl_win_set_title(v->win, v->files[i].name);
}

static void goto_index(struct V *v, int i)
{
	if (v->n == 0)
		return;
	if (i < 0)
		i = 0;
	if (i >= v->n)
		i = v->n - 1;
	if (i == v->cur && v->img.px)
		return;
	open_index(v, i);
}

/* ---- thumbnails -------------------------------------------------------------------------- */

/* A new copy of im with the orientation applied (thumbnails are small). */
static int img_orient(struct img *im, struct orient o)
{
	if (!o.rot && !o.flip)
		return 0;
	int w = im->w, h = im->h, rw = (o.rot & 1) ? h : w, rh = (o.rot & 1) ? w : h;
	uint32_t *d = malloc((size_t)rw * (size_t)rh * 4);
	if (!d)
		return -1;
	for (int v = 0; v < rh; v++)
		for (int u = 0; u < rw; u++) {
			int xp, yp;
			switch (o.rot & 3) {
			case 1: xp = v; yp = h - 1 - u; break;
			case 2: xp = w - 1 - u; yp = h - 1 - v; break;
			case 3: xp = w - 1 - v; yp = u; break;
			default: xp = u; yp = v; break;
			}
			if (o.flip)
				xp = w - 1 - xp;
			d[(size_t)v * (size_t)rw + (size_t)u] = im->px[(size_t)yp * (size_t)w + (size_t)xp];
		}
	free(im->px);
	im->px = d;
	im->w = rw;
	im->h = rh;
	return 0;
}

static void make_thumb(struct V *v, int i)
{
	struct thumb *t = &v->files[i].th;
	char p[PATH_MAX];
	path_of(v, i, p, sizeof p);
	struct img big;
	int fw, fh;
	struct orient o;
	if (load_file(p, THUMB_W * 2, &big, &o, &fw, &fh) < 0) {
		t->state = -1;
		return;
	}
	int w = big.w, h = big.h;
	double s = fmin((double)THUMB_W / w, (double)THUMB_H / h);
	if (s < 1) {
		w = (int)(w * s + 0.5);
		h = (int)(h * s + 0.5);
		if (w < 1)
			w = 1;
		if (h < 1)
			h = 1;
	}
	if (w == big.w && h == big.h) {
		t->im = big;
	} else {
		int rc = img_shrink(&big, &t->im, w, h);
		img_free(&big);
		if (rc < 0) {
			t->state = -1;
			return;
		}
	}
	if (img_orient(&t->im, o) < 0) {
		img_free(&t->im);
		t->state = -1;
		return;
	}
	t->state = 1;
	v->nthumbs++;
}

static void evict_thumbs(struct V *v)
{
	while (v->nthumbs > MAX_THUMBS) {
		int far = -1, d = -1;
		for (int i = 0; i < v->n; i++)
			if (v->files[i].th.state == 1 && abs(i - v->cur) > d) {
				d = abs(i - v->cur);
				far = i;
			}
		if (far < 0)
			break;
		img_free(&v->files[far].th.im);
		v->files[far].th.state = 0;
		v->nthumbs--;
	}
}

static void thumb_tick(void *ud)
{
	struct V *v = ud;
	/* the nearest picture to the current one that has no thumbnail yet */
	int best = -1;
	for (int d = 0; d < v->n && best < 0; d++) {
		int a = v->cur + d, b = v->cur - d;
		if (a < v->n && v->files[a].th.state == 0)
			best = a;
		else if (b >= 0 && v->files[b].th.state == 0)
			best = b;
		if (d > MAX_THUMBS / 2)
			break;
	}
	if (best < 0) {
		fl_timer_arm(v->thumb_timer, 0, 0);
		return;
	}
	make_thumb(v, best);
	evict_thumbs(v);
	fl_win_redraw(v->win);
}

/* ---- rendering the picture ------------------------------------------------------------- */

static void build_level(struct V *v, int shift)
{
	if (v->lv.px && v->lv_shift == shift && v->lv_gen == v->gen)
		return;
	img_free(&v->lv);
	v->lv_shift = shift;
	v->lv_gen = v->gen;
	int f = 1 << shift;
	int w = v->img.w / f, h = v->img.h / f;
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	if (img_shrink(&v->img, &v->lv, w, h) < 0)
		img_free(&v->lv);
}

static inline uint32_t bilerp(const struct img *im, double fx, double fy)
{
	/* fx, fy in pixel units, pixel centres at +0.5 */
	fx -= 0.5;
	fy -= 0.5;
	int x0 = (int)floor(fx), y0 = (int)floor(fy);
	int tx = (int)((fx - x0) * 256), ty = (int)((fy - y0) * 256);
	int x1 = x0 + 1, y1 = y0 + 1;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x0 >= im->w)
		x0 = im->w - 1;
	if (x1 >= im->w)
		x1 = im->w - 1;
	if (y0 >= im->h)
		y0 = im->h - 1;
	if (y1 >= im->h)
		y1 = im->h - 1;
	const uint32_t *r0 = im->px + (size_t)y0 * (size_t)im->w, *r1 = im->px + (size_t)y1 * (size_t)im->w;
	uint32_t p00 = r0[x0], p10 = r0[x1], p01 = r1[x0], p11 = r1[x1];
	uint32_t out = 0;
	for (int sh = 0; sh < 32; sh += 8) {
		int a = (int)((p00 >> sh) & 255) * (256 - tx) + (int)((p10 >> sh) & 255) * tx;
		int b = (int)((p01 >> sh) & 255) * (256 - tx) + (int)((p11 >> sh) & 255) * tx;
		out |= (uint32_t)(((a * (256 - ty) + b * ty) >> 16) & 255) << sh;
	}
	return out;
}

static inline uint32_t nearest(const struct img *im, double fx, double fy)
{
	int x = (int)floor(fx), y = (int)floor(fy);
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x >= im->w)
		x = im->w - 1;
	if (y >= im->h)
		y = im->h - 1;
	return im->px[(size_t)y * (size_t)im->w + (size_t)x];
}

/* Map a point (u, v) of the rotated picture (file pixel units) to the file's picture. */
static inline void unrotate(const struct V *v, double u, double w_, double h_, double vv, double *x, double *y)
{
	double xp, yp;
	switch (v->ori.rot & 3) {
	case 1: xp = vv; yp = h_ - u; break;
	case 2: xp = w_ - u; yp = h_ - vv; break;
	case 3: xp = w_ - vv; yp = u; break;
	default: xp = u; yp = vv; break;
	}
	if (v->ori.flip)
		xp = w_ - xp;
	*x = xp;
	*y = yp;
}

static void render_view(struct V *v, int cw, int ch, int dark)
{
	if (v->vcap < (size_t)cw * (size_t)ch) {
		free(v->vbuf);
		v->vbuf = malloc((size_t)cw * (size_t)ch * 4);
		v->vcap = v->vbuf ? (size_t)cw * (size_t)ch : 0;
		if (!v->vbuf)
			return;
	}
	v->vw = cw;
	v->vh = ch;
	int rw, rh;
	rotated_size(v, &rw, &rh);
	/* source: a shrunken level when zoomed out, else the picture itself */
	const struct img *src = &v->img;
	int shift = 0;
	if (v->zoom < 1) {
		while (shift < 6 && v->zoom * (double)(1 << (shift + 1)) <= 1.0 + 1e-9)
			shift++;
	}
	/* the decoded picture may be smaller than the file (huge JPEG) */
	double dec_scale = (double)v->img.w / (double)v->full_w;
	if (shift > 0) {
		build_level(v, shift);
		if (v->lv.px)
			src = &v->lv;
		else
			shift = 0;
	}
	double s = dec_scale / (double)(1 << shift); /* source pixels per file pixel */
	double w_ = v->full_w, h_ = v->full_h;
	int ss = 1; /* supersampling when the level is still more than 1.3x too big */
	if (v->zoom < 1 && s / v->zoom > 1.3)
		ss = 2;
	uint32_t bg = fl_th.bg;
	/* checkerboard under transparent pixels */
	uint32_t ck0 = dark ? 0xff2a2f36 : 0xffd0d4d8, ck1 = dark ? 0xff23282e : 0xffc4c8cc;
	int x_lo = (int)floor(v->ox), y_lo = (int)floor(v->oy);
	int x_hi = (int)ceil(v->ox + rw * v->zoom), y_hi = (int)ceil(v->oy + rh * v->zoom);
	for (int y = 0; y < ch; y++) {
		uint32_t *row = v->vbuf + (size_t)y * (size_t)cw;
		if (y < y_lo || y >= y_hi) {
			for (int x = 0; x < cw; x++)
				row[x] = bg;
			continue;
		}
		for (int x = 0; x < cw; x++) {
			if (x < x_lo || x >= x_hi) {
				row[x] = bg;
				continue;
			}
			uint32_t p;
			if (ss == 1) {
				double u = (x + 0.5 - v->ox) / v->zoom, vv = (y + 0.5 - v->oy) / v->zoom, fx, fy;
				unrotate(v, u, w_, h_, vv, &fx, &fy);
				p = v->zoom > 2.5 ? nearest(src, fx * s, fy * s) : bilerp(src, fx * s, fy * s);
			} else {
				unsigned a = 0, r = 0, g = 0, b = 0;
				for (int j = 0; j < 2; j++)
					for (int i = 0; i < 2; i++) {
						double u = (x + 0.25 + 0.5 * i - v->ox) / v->zoom;
						double vv = (y + 0.25 + 0.5 * j - v->oy) / v->zoom, fx, fy;
						unrotate(v, u, w_, h_, vv, &fx, &fy);
						uint32_t q = bilerp(src, fx * s, fy * s);
						a += q >> 24;
						r += (q >> 16) & 255;
						g += (q >> 8) & 255;
						b += q & 255;
					}
				p = (a / 4) << 24 | (r / 4) << 16 | (g / 4) << 8 | (b / 4);
			}
			uint32_t a = p >> 24;
			if (a != 255) {
				uint32_t under = (((x >> 3) + (y >> 3)) & 1) ? ck1 : ck0;
				uint32_t ia = 255 - a;
				uint32_t r = ((p >> 16) & 255) + ((under >> 16) & 255) * ia / 255;
				uint32_t g = ((p >> 8) & 255) + ((under >> 8) & 255) * ia / 255;
				uint32_t b = (p & 255) + (under & 255) * ia / 255;
				if (r > 255) r = 255;
				if (g > 255) g = 255;
				if (b > 255) b = 255;
				p = 0xff000000u | r << 16 | g << 8 | b;
			}
			row[x] = p | 0xff000000u;
		}
	}
	v->vkey.gen = v->gen;
	v->vkey.zoom = v->zoom;
	v->vkey.ox = v->ox;
	v->vkey.oy = v->oy;
	v->vkey.w = cw;
	v->vkey.h = ch;
	v->vkey.dark = dark;
	v->vkey.rot = v->ori.rot;
	v->vkey.flip = v->ori.flip;
	v->vvalid = 1;
}

static void blit(fl_canvas *cv, fl_rect r, const uint32_t *src, int sw)
{
	if (!cv->px)
		return;
	fl_rect c = fl_intersect(r, cv->clip);
	for (int y = c.y; y < c.y + c.h; y++)
		memcpy(cv->px + (size_t)y * (size_t)cv->stride + (size_t)c.x,
		       src + (size_t)(y - r.y) * (size_t)sw + (size_t)(c.x - r.x), (size_t)c.w * 4);
}

/* ---- actions ---------------------------------------------------------------------------- */

static void pct_encode(const char *s, char *out, size_t cap)
{
	size_t n = 0;
	for (; *s && n + 4 < cap; s++) {
		unsigned char c = (unsigned char)*s;
		if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
			out[n++] = (char)c;
		else
			n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
	}
	out[n] = 0;
}

static int mkdir_p(const char *path)
{
	char t[PATH_MAX];
	snprintf(t, sizeof t, "%s", path);
	for (char *p = t + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		if (mkdir(t, 0700) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(t, 0700) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int copy_file(const char *src, const char *dst)
{
	FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
	if (!in || !out) {
		if (in)
			fclose(in);
		return -1;
	}
	char buf[65536];
	size_t n;
	int rc = 0;
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		if (fwrite(buf, 1, n, out) != n) {
			rc = -1;
			break;
		}
	fclose(in);
	if (fclose(out) != 0)
		rc = -1;
	return rc;
}

/* FreeDesktop trash, the same layout flfiles uses */
static int trash_file(const char *path)
{
	const char *home = getenv("HOME");
	if (!home || !*home) {
		errno = ENOENT;
		return -1;
	}
	const char *xdg = getenv("XDG_DATA_HOME");
	char base[PATH_MAX], files[PATH_MAX], info[PATH_MAX];
	if (xdg && *xdg)
		snprintf(base, sizeof base, "%s/Trash", xdg);
	else
		snprintf(base, sizeof base, "%s/.local/share/Trash", home);
	snprintf(files, sizeof files, "%s/files", base);
	snprintf(info, sizeof info, "%s/info", base);
	if (mkdir_p(files) < 0 || mkdir_p(info) < 0)
		return -1;
	const char *bn = strrchr(path, '/');
	bn = bn ? bn + 1 : path;
	char name[NAME_MAX + 16], dst[PATH_MAX], ipath[PATH_MAX];
	struct stat st;
	for (int i = 1;; i++) {
		if (i == 1)
			snprintf(name, sizeof name, "%.255s", bn);
		else
			snprintf(name, sizeof name, "%.200s.%d", bn, i);
		snprintf(dst, sizeof dst, "%s/%s", files, name);
		snprintf(ipath, sizeof ipath, "%s/%s.trashinfo", info, name);
		if (lstat(dst, &st) < 0 && lstat(ipath, &st) < 0)
			break;
		if (i > 9999) {
			errno = EEXIST;
			return -1;
		}
	}
	char enc[PATH_MAX * 3], date[32];
	pct_encode(path, enc, sizeof enc);
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
	FILE *fp = fopen(ipath, "w");
	if (!fp)
		return -1;
	fprintf(fp, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, date);
	if (fclose(fp) != 0) {
		unlink(ipath);
		return -1;
	}
	if (rename(path, dst) < 0) {
		if (errno != EXDEV || copy_file(path, dst) < 0 || unlink(path) < 0) {
			int e = errno;
			unlink(ipath);
			errno = e;
			return -1;
		}
	}
	return 0;
}

static void act_delete(struct V *v)
{
	if (v->n == 0 || v->cur < 0)
		return;
	char p[PATH_MAX];
	path_of(v, v->cur, p, sizeof p);
	if (trash_file(p) < 0) {
		say(v, 1, "Could not move %s to the Trash: %s", v->files[v->cur].name, strerror(errno));
		return;
	}
	char name[NAME_MAX + 1];
	snprintf(name, sizeof name, "%s", v->files[v->cur].name);
	if (v->files[v->cur].th.state == 1) {
		img_free(&v->files[v->cur].th.im);
		v->nthumbs--;
	}
	free(v->files[v->cur].name);
	memmove(&v->files[v->cur], &v->files[v->cur + 1], (size_t)(v->n - v->cur - 1) * sizeof *v->files);
	v->n--;
	drop_picture(v);
	if (v->n == 0) {
		v->cur = 0;
		v->full_w = v->full_h = 0;
		fl_win_set_title(v->win, "Images");
	} else {
		open_index(v, v->cur < v->n ? v->cur : v->n - 1);
	}
	say(v, 0, "Moved %s to the Trash", name);
}

/* The wallpaper is set by the Settings app: `flsettings --set-wallpaper FILE` converts the
 * picture (PNG or JPEG) into its own .flw format under ~/.local/share/freelfs/wallpapers
 * and writes wallpaper=user:NAME to desktop.conf, which the compositor reads. */
static void act_wallpaper(struct V *v)
{
	if (v->n == 0)
		return;
	char p[PATH_MAX];
	path_of(v, v->cur, p, sizeof p);
	size_t n = strlen(p);
	if (n > 5 && !strcasecmp(p + n - 5, ".webp")) {
		say(v, 1, "Only PNG and JPEG pictures can be the wallpaper");
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		say(v, 1, "Could not start Settings: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
		}
		execlp("flsettings", "flsettings", "--set-wallpaper", p, (char *)NULL);
		_exit(127);
	}
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		say(v, 1, "Could not set %s as the wallpaper", v->files[v->cur].name);
		return;
	}
	say(v, 0, "Set %s as the wallpaper", v->files[v->cur].name);
}

static void act_rotate(struct V *v, fl_rect c, int d)
{
	if (!v->img.px)
		return;
	v->ori.rot = (v->ori.rot + d) & 3;
	v->gen++; /* the cached view and levels are for the old turn */
	/* keep the centre of the canvas where it is */
	if (v->fitted) {
		set_fit(v, c);
	} else {
		/* rotating swaps the axes: re-centre */
		v->ox = c.w / 2.0;
		v->oy = c.h / 2.0;
		int rw, rh;
		rotated_size(v, &rw, &rh);
		v->ox -= rw * v->zoom / 2;
		v->oy -= rh * v->zoom / 2;
		clamp_view(v, c);
	}
}

/* ---- drawing ---------------------------------------------------------------------------- */

static void fmt_size(off_t n, char *out, size_t cap)
{
	if (n < 1024)
		snprintf(out, cap, "%d B", (int)n);
	else if (n < 1024 * 1024)
		snprintf(out, cap, "%.0f KB", (double)n / 1024);
	else
		snprintf(out, cap, "%.1f MB", (double)n / (1024 * 1024));
}

static int tbtn(fl_ui *ui, uint32_t id, fl_rect r, const char *s)
{
	return fl_button(ui, id, r, s, FL_BTN_PLAIN);
}

static void draw_strip(fl_ui *ui, struct V *v, fl_rect st)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, st, fl_th.panel);
	fl_fill_rect(cv, fl_r(st.x, st.y, st.w, 1), fl_th.line);
	int total = v->n * CELL_W;
	double want = v->cur * CELL_W + CELL_W / 2.0 - st.w / 2.0;
	double max = total > st.w ? total - st.w : 0;
	if (want < 0 || total <= st.w)
		want = total <= st.w ? -(st.w - total) / 2.0 : 0;
	if (want > max)
		want = max;
	if (fabs(want - v->strip_off) > 0.5)
		v->strip_off += (want - v->strip_off) * 0.35 + (want > v->strip_off ? 0.5 : -0.5);
	else
		v->strip_off = want;
	if (fabs(want - v->strip_off) > 0.5)
		fl_win_redraw(v->win); /* keep gliding */
	int res = fl_widget(ui, ID_STRIPAREA, st, 0);
	if ((res & FL_HOVER) && ui->wheel) {
		if (ui->wheel > 0)
			goto_index(v, v->cur + 1);
		else
			goto_index(v, v->cur - 1);
		ui->wheel = 0;
	}
	fl_rect save = cv->clip;
	fl_clip(cv, st);
	int first = (int)floor(v->strip_off / CELL_W);
	if (first < 0)
		first = 0;
	int clicked = -1;
	for (int i = first; i < v->n; i++) {
		int cx = st.x + (int)(i * CELL_W - v->strip_off);
		if (cx >= st.x + st.w)
			break;
		fl_rect cell = fl_r(cx + 4, st.y + 8, CELL_W - 8, st.h - 16);
		int r = fl_widget(ui, ID_STRIP + 100 + (uint32_t)i, cell, 0);
		if (r & FL_CLICK)
			clicked = i;
		if (r & FL_HOVER)
			fl_fill_rect(cv, cell, fl_th.raised);
		struct thumb *t = &v->files[i].th;
		if (t->state == 1) {
			int tx = cell.x + (cell.w - t->im.w) / 2, ty = cell.y + (cell.h - t->im.h) / 2;
			fl_rect tr = fl_r(tx, ty, t->im.w, t->im.h);
			/* the thumbnail is from the unrotated file: show it as the viewer would */
			if (cv->px) {
				fl_rect cc = fl_intersect(tr, cv->clip);
				for (int y = cc.y; y < cc.y + cc.h; y++)
					for (int x = cc.x; x < cc.x + cc.w; x++) {
						uint32_t p = t->im.px[(size_t)(y - ty) * (size_t)t->im.w + (size_t)(x - tx)];
						uint32_t a = p >> 24;
						if (a != 255) {
							uint32_t u = fl_th.bg, ia = 255 - a;
							uint32_t rr = ((p >> 16) & 255) + ((u >> 16) & 255) * ia / 255;
							uint32_t gg = ((p >> 8) & 255) + ((u >> 8) & 255) * ia / 255;
							uint32_t bb = (p & 255) + (u & 255) * ia / 255;
							p = 0xff000000u | (rr > 255 ? 255 : rr) << 16 | (gg > 255 ? 255 : gg) << 8 | (bb > 255 ? 255 : bb);
						}
						cv->px[(size_t)y * (size_t)cv->stride + (size_t)x] = p | 0xff000000u;
					}
			}
		} else {
			fl_fill_rect(cv, fl_inset(cell, 8, 8), fl_th.bg);
			if (t->state < 0)
				fl_text_in(cv, v->small, cell, FL_CENTER, 0, fl_th.dim, "?");
		}
		if (i == v->cur)
			fl_stroke_rect(cv, cell, 2, fl_th.accent);
	}
	cv->clip = save;
	if (clicked >= 0)
		goto_index(v, clicked);
}

static void keys(fl_ui *ui, struct V *v, fl_rect c)
{
	uint32_t k = ui->key;
	if (!k)
		return;
	int mods = fl_app_mods(v->app), ctrl = mods & FL_MOD_CTRL, shift = mods & FL_MOD_SHIFT;
	int handled = 1;
	double fx = c.w / 2.0, fy = c.h / 2.0;
	int rw, rh;
	rotated_size(v, &rw, &rh);
	int pan = 0;
	switch (k) {
	case XKB_KEY_Right:
	case XKB_KEY_Page_Down:
		goto_index(v, v->cur + 1);
		break;
	case XKB_KEY_Left:
	case XKB_KEY_Page_Up:
	case XKB_KEY_BackSpace:
		goto_index(v, v->cur - 1);
		break;
	case XKB_KEY_Home: goto_index(v, 0); break;
	case XKB_KEY_End: goto_index(v, v->n - 1); break;
	case XKB_KEY_Up:
	case XKB_KEY_Down:
		/* Up and Down pan a zoomed picture, else they zoom */
		if (v->fitted || (rw * v->zoom <= c.w && rh * v->zoom <= c.h)) {
			zoom_to(v, c, v->zoom * (k == XKB_KEY_Up ? 1.25 : 0.8), fx, fy);
		} else {
			v->oy += k == XKB_KEY_Up ? 60 : -60;
			pan = 1;
		}
		break;
	case XKB_KEY_plus:
	case XKB_KEY_equal:
	case XKB_KEY_KP_Add:
		if (v->img.px)
			zoom_to(v, c, v->zoom * 1.25, fx, fy);
		break;
	case XKB_KEY_minus:
	case XKB_KEY_KP_Subtract:
		if (v->img.px)
			zoom_to(v, c, v->zoom * 0.8, fx, fy);
		break;
	case XKB_KEY_0:
	case XKB_KEY_KP_0:
	case XKB_KEY_f:
		if (v->img.px)
			set_fit(v, c);
		break;
	case XKB_KEY_1:
	case XKB_KEY_KP_1:
		if (v->img.px)
			zoom_to(v, c, 1.0, fx, fy);
		break;
	case XKB_KEY_r:
	case XKB_KEY_R:
		act_rotate(v, c, shift ? 3 : 1);
		break;
	case XKB_KEY_bracketleft: act_rotate(v, c, 3); break;
	case XKB_KEY_bracketright: act_rotate(v, c, 1); break;
	case XKB_KEY_Delete:
	case XKB_KEY_KP_Delete:
		act_delete(v);
		break;
	case XKB_KEY_w:
	case XKB_KEY_W:
		act_wallpaper(v);
		break;
	case XKB_KEY_t:
	case XKB_KEY_T:
		v->show_strip = !v->show_strip;
		break;
	case XKB_KEY_q:
	case XKB_KEY_Escape:
		fl_app_quit(v->app);
		break;
	default:
		handled = 0;
		if (ctrl && (k == XKB_KEY_w))
			fl_app_quit(v->app);
	}
	(void)pan;
	if (handled)
		ui->key = 0;
}

/* while fitted, the zoom follows the window and the current picture */
static void sync_fit(struct V *v, fl_rect rel)
{
	if (v->img.px && v->fitted) {
		v->zoom = fit_zoom(v, rel);
		clamp_view(v, rel);
	}
}

static void draw(fl_ui *ui, void *ud)
{
	struct V *v = ud;
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(ui->win);
	fl_fill_rect(cv, c, fl_th.bg);
	fl_rect area = c;
	fl_rect tb = fl_cut_top(&area, TOOL_H);
	fl_rect strip = {0};
	if (v->show_strip && v->n > 0)
		strip = fl_cut_bottom(&area, STRIP_H);
	fl_rect canvas = area;
	fl_rect rel = fl_r(0, 0, canvas.w, canvas.h); /* the view works in canvas coordinates */

	unsigned gen0 = v->gen;
	sync_fit(v, rel);
	if (v->test_zoom_pct && v->img.px) {
		zoom_to(v, rel, v->test_zoom_pct / 100.0, rel.w / 2.0, rel.h / 2.0);
		v->test_zoom_pct = 0;
	}

	if (v->script) {
		static const struct { const char *n; uint32_t k; } names[] = {
			{"Right", XKB_KEY_Right}, {"Left", XKB_KEY_Left}, {"Home", XKB_KEY_Home}, {"End", XKB_KEY_End},
			{"Delete", XKB_KEY_Delete}, {"Up", XKB_KEY_Up}, {"Down", XKB_KEY_Down},
		};
		char tok[32];
		const char *p = v->script;
		v->script = NULL;
		while (*p) {
			size_t n = strcspn(p, ",");
			snprintf(tok, sizeof tok, "%.*s", (int)n, p);
			p += n + (p[n] ? 1 : 0);
			ui->key = tok[1] ? 0 : (uint32_t)(unsigned char)tok[0];
			for (size_t i = 0; i < sizeof names / sizeof *names; i++)
				if (!strcmp(tok, names[i].n))
					ui->key = names[i].k;
			sync_fit(v, rel);
			keys(ui, v, rel);
		}
	}
	keys(ui, v, rel);
	sync_fit(v, rel);

	/* the picture: wheel zooms at the pointer, drag pans, double click toggles fit / 100% */
	int res = fl_widget(ui, ID_CANVAS, canvas, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	if (v->img.px) {
		int mx = ui->mx - canvas.x, my = ui->my - canvas.y;
		if ((res & FL_HOVER) && ui->wheel) {
			double f = pow(1.18, -ui->wheel);
			zoom_to(v, rel, v->zoom * f, mx, my);
			ui->wheel = 0;
		}
		if ((ui->press & FL_BTN_L) && fl_in(canvas, ui->mx, ui->my)) {
			if (ui->clicks >= 2) {
				if (v->fitted || fabs(v->zoom - 1.0) > 0.01)
					zoom_to(v, rel, 1.0, mx, my);
				else
					set_fit(v, rel);
				v->drag = 0;
			} else {
				v->drag = 1;
			}
			v->drag_x = ui->mx;
			v->drag_y = ui->my;
		}
		if (v->drag && (ui->btn_held & FL_BTN_L)) {
			if (ui->mx != v->drag_x || ui->my != v->drag_y) {
				v->ox += ui->mx - v->drag_x;
				v->oy += ui->my - v->drag_y;
				v->fitted = 0;
				v->drag_x = ui->mx;
				v->drag_y = ui->my;
				clamp_view(v, rel);
			}
		} else {
			v->drag = 0;
		}
		/* draw: from the cache when nothing changed */
		int dark = fl_th.dark;
		int stale = !v->vvalid || v->vkey.gen != v->gen || v->vkey.zoom != v->zoom || v->vkey.ox != v->ox ||
		            v->vkey.oy != v->oy || v->vkey.w != rel.w || v->vkey.h != rel.h || v->vkey.dark != dark ||
		            v->vkey.rot != v->ori.rot || v->vkey.flip != v->ori.flip;
		if (stale && cv->px)
			render_view(v, rel.w, rel.h, dark);
		if (v->vvalid && v->vw == rel.w && v->vh == rel.h)
			blit(cv, canvas, v->vbuf, v->vw);
		if (v->drag)
			ui->cursor = FL_CURSOR_ARROW;
	} else if (v->n == 0) {
		char s[PATH_MAX + 64];
		snprintf(s, sizeof s, "No pictures in %s", v->dir);
		fl_text_in(cv, ui->f_reg, canvas, FL_CENTER, 0, fl_th.dim, s);
	} else if (v->load_err) {
		char s[NAME_MAX + 64];
		snprintf(s, sizeof s, "Cannot open %s", v->files[v->cur].name);
		fl_text_in(cv, ui->f_reg, canvas, FL_CENTER, 0, fl_th.err, s);
	}

	/* the toolbar */
	fl_fill_rect(cv, tb, fl_th.panel);
	fl_fill_rect(cv, fl_r(tb.x, tb.y + tb.h - 1, tb.w, 1), fl_th.line);
	fl_rect t = fl_inset(tb, 8, 6);
	if (fl_icon_button(ui, ID_PREV, fl_cut_left(&t, 32), FL_ICON_BACK, 16, fl_th.text))
		goto_index(v, v->cur - 1);
	t.x += 2;
	t.w -= 2;
	if (fl_icon_button(ui, ID_NEXT, fl_cut_left(&t, 32), FL_ICON_FORWARD, 16, fl_th.text))
		goto_index(v, v->cur + 1);
	t.x += 8;
	t.w -= 8;
	/* buttons from the right */
	int tw = t.w;
	fl_rect right = t;
	static const struct { uint32_t id; const char *label; int w; } rb[] = {
		{ID_STRIP, "Strip", 52}, {ID_WALL, "Wallpaper", 84}, {ID_DEL, "Trash", 56},
		{ID_ROT, "Rotate", 62}, {ID_ONE, "100%", 50}, {ID_FIT, "Fit", 38}, {ID_ZIN, "+", 30}, {ID_ZOUT, "-", 30},
	};
	int used = 0;
	int show[8];
	for (int i = 0; i < 8; i++) {
		show[i] = tw - used > 360 + rb[i].w;
		if (show[i])
			used += rb[i].w + 2;
	}
	fl_rect rr = right;
	for (int i = 0; i < 8; i++) {
		if (!show[i])
			continue;
		fl_rect b = fl_cut_right(&rr, rb[i].w);
		rr.w -= 2;
		int hit = tbtn(ui, rb[i].id, b, rb[i].label);
		if (rb[i].id == ID_STRIP && v->show_strip)
			fl_fill_rect(cv, fl_r(b.x + 8, b.y + b.h - 3, b.w - 16, 2), fl_th.accent);
		if (!hit)
			continue;
		switch (rb[i].id) {
		case ID_STRIP: v->show_strip = !v->show_strip; break;
		case ID_WALL: act_wallpaper(v); break;
		case ID_DEL: act_delete(v); break;
		case ID_ROT: act_rotate(v, rel, 1); break;
		case ID_ONE: if (v->img.px) zoom_to(v, rel, 1.0, rel.w / 2.0, rel.h / 2.0); break;
		case ID_FIT: if (v->img.px) set_fit(v, rel); break;
		case ID_ZIN: if (v->img.px) zoom_to(v, rel, v->zoom * 1.25, rel.w / 2.0, rel.h / 2.0); break;
		case ID_ZOUT: if (v->img.px) zoom_to(v, rel, v->zoom * 0.8, rel.w / 2.0, rel.h / 2.0); break;
		}
	}
	/* name and facts in the middle */
	fl_rect info = fl_r(t.x, t.y, rr.w - 8, t.h);
	if (v->n > 0) {
		char sz[32], line[PATH_MAX + 128], fit[PATH_MAX + 128];
		fmt_size(v->files[v->cur].size, sz, sizeof sz);
		int w1 = (int)strlen(v->files[v->cur].name);
		(void)w1;
		char meta[160];
		int rwd, rht;
		rotated_size(v, &rwd, &rht);
		if (v->img.px)
			snprintf(meta, sizeof meta, "%d / %d    %d x %d    %s    %.0f%%", v->cur + 1, v->n, rwd,
			         rht, sz, v->zoom * 100);
		else
			snprintf(meta, sizeof meta, "%d / %d    %s", v->cur + 1, v->n, sz);
		int mw = fl_text_width(v->small, meta, -1);
		if (mw > info.w / 2)
			mw = info.w / 2;
		int nw = info.w - mw - 16;
		if (nw > 40) {
			snprintf(line, sizeof line, "%s", v->files[v->cur].name);
			fl_text_fit(ui->f_med, line, nw, fit, sizeof fit);
			fl_text_in(cv, ui->f_med, fl_r(info.x, info.y, nw, info.h), FL_LEFT, 0, fl_th.strong, fit);
			fl_text_in(cv, v->small, fl_r(info.x + nw + 16, info.y, mw + 4, info.h), FL_LEFT, 0, fl_th.dim, meta);
		}
	}

	if (v->show_strip && v->n > 0)
		draw_strip(ui, v, strip);

	/* a message, bottom centre of the picture */
	if (v->msg[0]) {
		int mw = fl_text_width(ui->f_reg, v->msg, -1) + 32;
		if (mw > canvas.w - 24)
			mw = canvas.w - 24;
		fl_rect m = fl_r(canvas.x + (canvas.w - mw) / 2, canvas.y + canvas.h - 52, mw, 34);
		fl_fill_rect(cv, m, fl_th.raised);
		fl_stroke_rect(cv, m, 1, v->msg_err ? fl_th.err : fl_th.line);
		fl_text_in(cv, ui->f_reg, m, FL_CENTER, 0, v->msg_err ? fl_th.err : fl_th.text, v->msg);
	}
	if (v->gen != gen0)
		fl_win_redraw(v->win); /* the strip or a button changed the picture after it was drawn */
}

/* ---- main ------------------------------------------------------------------------------- */

static void usage(void)
{
	fprintf(stderr, "usage: flview [--rotate N] [--zoom PERCENT] [--no-strip] [FILE|FOLDER]\n"
	                "View pictures (PNG, JPEG, WebP). Arrow keys: next, previous. Del: Trash. W: wallpaper.\n");
}

int main(int argc, char **argv)
{
	struct V *v = calloc(1, sizeof *v);
	if (!v)
		return 1;
	v->show_strip = 1;
	const char *arg = NULL;
	int rot = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		} else if (!strcmp(argv[i], "--no-strip")) {
			v->show_strip = 0;
		} else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) {
			rot = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) {
			v->test_zoom_pct = atoi(argv[++i]);
		} else if (argv[i][0] == '-' && argv[i][1]) {
			usage();
			return 2;
		} else if (!arg) {
			arg = argv[i];
		} else {
			usage();
			return 2;
		}
	}
	v->script = getenv("FLVIEW_KEYS");
	char dir[PATH_MAX] = "", file[NAME_MAX + 1] = "";
	if (arg) {
		char real[PATH_MAX];
		struct stat st;
		if (!realpath(arg, real) || stat(real, &st) < 0) {
			fprintf(stderr, "flview: cannot open %s: %s\n", arg, strerror(errno));
			return 1;
		}
		if (S_ISDIR(st.st_mode)) {
			snprintf(dir, sizeof dir, "%s", real);
		} else {
			char *sl = strrchr(real, '/');
			snprintf(file, sizeof file, "%s", sl + 1);
			if (sl == real)
				snprintf(dir, sizeof dir, "/");
			else {
				*sl = 0;
				snprintf(dir, sizeof dir, "%s", real);
			}
		}
	} else {
		const char *home = getenv("HOME");
		struct stat st;
		if (home && *home) {
			snprintf(dir, sizeof dir, "%s/Pictures", home);
			if (stat(dir, &st) < 0 || !S_ISDIR(st.st_mode))
				dir[0] = 0;
		}
		if (!dir[0] && !getcwd(dir, sizeof dir))
			snprintf(dir, sizeof dir, "/");
	}

	v->app = fl_app_new(NULL, NULL);
	if (!v->app) {
		fprintf(stderr, "flview: cannot connect to a Wayland compositor\n");
		return 1;
	}
	v->small = fl_font_ui(400, 12);
	v->msg_timer = fl_timer_new(v->app, msg_clear, v);
	v->thumb_timer = fl_timer_new(v->app, thumb_tick, v);
	fl_win_opts o = {
		.title = "Images", .app_id = "flview", .w = 980, .h = 660, .min_w = 480, .min_h = 320,
		.decorated = 1, .resizable = 1, .draw = draw, .ud = v,
	};
	v->win = fl_win_new(v->app, &o);
	if (!v->win)
		return 1;
	if (scan_dir(v, dir) < 0) {
		fprintf(stderr, "flview: cannot read %s: %s\n", dir, strerror(errno));
		return 1;
	}
	int start = 0;
	if (file[0])
		for (int i = 0; i < v->n; i++)
			if (!strcmp(v->files[i].name, file))
				start = i;
	if (file[0] && format_of(file) == FMT_NONE)
		say(v, 1, "%s is not a picture flview can read", file);
	if (v->n) {
		open_index(v, start);
		v->ori.rot = (v->ori.rot + rot) & 3;
		fl_timer_arm(v->thumb_timer, 1, 1);
	}
	fl_app_watch_conf(v->app, NULL, NULL);
	fl_app_run(v->app);
	fl_app_free(v->app);
	return 0;
}
