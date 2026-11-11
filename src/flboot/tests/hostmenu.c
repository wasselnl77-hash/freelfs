/* Host-side stand-in for the firmware, to run flboot's real conf.c / menu.c /
 * gfx.c / lib.c (and the font) as a normal program: a GOP frame buffer that is
 * written out as PPM, the ESP as a directory, keys from a script.
 *
 *   hostmenu ESPDIR WxH SCRIPT...     (FLBOOT_TEXT=1: no graphics, text console on stdout)
 * SCRIPT words: s:NAME screenshot NAME.ppm of what is on screen now;
 *   d u e x: Down, Up, Enter, Esc;  t: one second passes (the countdown).
 * Prints the chosen entry (number, title, kernel command line) when the menu returns.
 * SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../flboot.h"

EFI_HANDLE image_handle;
EFI_SYSTEM_TABLE *ST;
EFI_BOOT_SERVICES *BS;

static const char *esp;
static char **script;
static int nscript, pos;
static uint32_t *fb;
static int fbw, fbh, text_mode;
static EFI_INPUT_KEY pending;

/* ---- the file system: files of ESP, \ turned into / ---- */
EFI_STATUS fs_init(void) { return EFI_SUCCESS; }
EFI_HANDLE fs_device(void) { return NULL; }

static char *host_path(const CHAR16 *p)
{
	char *s = malloc(strlen(esp) + 300), *o = s + sprintf(s, "%s", esp);
	for (; *p; p++)
		*o++ = *p == '\\' ? '/' : (char)*p;
	*o = 0;
	return s;
}

EFI_STATUS fs_read(const CHAR16 *path, char **data, UINTN *size)
{
	char *hp = host_path(path);
	FILE *f = fopen(hp, "rb");
	free(hp);
	if (!f)
		return EFI_NOT_FOUND;
	char *b = calloc(1, 1 << 16);
	size_t n = fread(b, 1, (1 << 16) - 1, f);
	fclose(f);
	*data = b;
	if (size)
		*size = n;
	return EFI_SUCCESS;
}

EFI_STATUS fs_list(const CHAR16 *path, void (*fn)(const CHAR16 *, void *), void *ctx)
{
	char *hp = host_path(path);
	DIR *d = opendir(hp);
	free(hp);
	if (!d)
		return EFI_NOT_FOUND;
	struct dirent *de;
	while ((de = readdir(d)))
		if (de->d_type == DT_REG) {
			CHAR16 w[256];
			size_t i = 0;
			for (; de->d_name[i] && i < 255; i++)
				w[i] = (unsigned char)de->d_name[i];
			w[i] = 0;
			fn(w, ctx);
		}
	closedir(d);
	return EFI_SUCCESS;
}

/* ---- the script ---- */
static void dump(const char *name)
{
	if (text_mode)
		return;
	char path[512];
	snprintf(path, sizeof path, "%s.ppm", name);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", fbw, fbh);
	for (int i = 0; i < fbw * fbh; i++) {
		unsigned char px[3] = {(unsigned char)(fb[i] >> 16), (unsigned char)(fb[i] >> 8), (unsigned char)fb[i]};
		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

static EFI_STATUS EFIAPI wait_for_event(UINTN n, EFI_EVENT *ev, UINTN *which)
{
	while (pos < nscript && !strncmp(script[pos], "s:", 2))
		dump(script[pos++] + 2);
	if (pos >= nscript) {
		puts("script ended without the menu returning");
		exit(3);
	}
	const char *w = script[pos++];
	*which = 0;
	memset(&pending, 0, sizeof pending);
	switch (w[0]) {
	case 'd': pending.ScanCode = SCAN_DOWN; break;
	case 'u': pending.ScanCode = SCAN_UP; break;
	case 'e': pending.UnicodeChar = '\r'; break;
	case 'x': pending.ScanCode = SCAN_ESC; break;
	case 't': *which = 1; break;
	}
	return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI read_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *p, EFI_INPUT_KEY *k)
{
	*k = pending;
	return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI in_reset(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *p, BOOLEAN b) { return 0; }
static EFI_STATUS EFIAPI alloc_pool(EFI_MEMORY_TYPE t, UINTN n, void **p) { *p = malloc(n); return *p ? 0 : 1; }
static EFI_STATUS EFIAPI free_pool(void *p) { free(p); return 0; }
static EFI_STATUS EFIAPI create_event(UINT32 t, UINTN l, void *f, void *c, EFI_EVENT *e) { *e = (void *)1; return 0; }
static EFI_STATUS EFIAPI set_timer(EFI_EVENT e, EFI_TIMER_DELAY d, UINT64 t) { return 0; }
static EFI_STATUS EFIAPI close_event(EFI_EVENT e) { return 0; }

static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION ginfo;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE gmode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL gop = {.Mode = &gmode};

static EFI_STATUS EFIAPI locate_protocol(const EFI_GUID *g, void *reg, void **out)
{
	if (text_mode)
		return EFI_NOT_FOUND;
	*out = &gop;
	return 0;
}

static EFI_STATUS EFIAPI out_string(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *p, const CHAR16 *s)
{
	for (; *s; s++)
		if (*s != '\r')
			putchar(*s);
	return 0;
}
static EFI_STATUS EFIAPI out_clear(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *p)
{
	puts("\n-------- screen --------");
	return 0;
}
static EFI_STATUS EFIAPI out_cursor(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *p, BOOLEAN b) { return 0; }

int main(int argc, char **argv)
{
	if (argc < 4) {
		fputs("usage: hostmenu ESPDIR WxH SCRIPT...\n", stderr);
		return 2;
	}
	esp = argv[1];
	sscanf(argv[2], "%dx%d", &fbw, &fbh);
	script = argv + 3;
	nscript = argc - 3;
	text_mode = getenv("FLBOOT_TEXT") != NULL;
	fb = calloc((size_t)fbw * fbh, 4);
	ginfo = (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){.HorizontalResolution = (UINT32)fbw, .VerticalResolution = (UINT32)fbh,
	                                              .PixelFormat = PixelBlueGreenRedReserved8BitPerColor,
	                                              .PixelsPerScanLine = (UINT32)fbw};
	gmode.Info = &ginfo;
	gmode.FrameBufferBase = (UINTN)fb;

	static EFI_SIMPLE_TEXT_INPUT_PROTOCOL in = {.Reset = in_reset, .ReadKeyStroke = read_key, .WaitForKey = (void *)2};
	static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL out = {.OutputString = out_string, .ClearScreen = out_clear, .EnableCursor = out_cursor};
	static EFI_BOOT_SERVICES bs = {.AllocatePool = alloc_pool, .FreePool = free_pool, .CreateEvent = create_event,
	                               .SetTimer = set_timer, .WaitForEvent = wait_for_event, .CloseEvent = close_event,
	                               .LocateProtocol = locate_protocol};
	static EFI_SYSTEM_TABLE st = {.ConIn = &in, .ConOut = &out, .StdErr = &out};
	st.BootServices = &bs;
	ST = &st;
	BS = &bs;

	static struct config cfg;
	conf_load(&cfg);
	int r = menu_run(&cfg);
	printf("chosen: %d\ntitle: %s\ncmdline: %s\n", r, cfg.entries[r].title, cfg.entries[r].cmdline);
	return 0;
}
