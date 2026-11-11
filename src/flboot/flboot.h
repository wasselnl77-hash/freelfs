/* flboot -- the FreeLFS boot loader. Shared declarations.
 * SPDX-License-Identifier: MIT */
#ifndef FLBOOT_H
#define FLBOOT_H

#include "efi.h"

#define FLBOOT_VERSION L"0.1"

extern EFI_HANDLE image_handle;
extern EFI_SYSTEM_TABLE *ST;
extern EFI_BOOT_SERVICES *BS;

/* lib.c */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
void *xalloc(UINTN size);
void xfree(void *p);
size_t wcslen16(const CHAR16 *s);
CHAR16 *ascii_to16(const char *s);       /* new string */
char *ascii_dup(const char *s, size_t n); /* new string */
CHAR16 *str16_cat(const CHAR16 *a, const CHAR16 *b);
int vercmp(const char *a, const char *b);
void print(const CHAR16 *s);
void print_ascii(const char *s);
void fatal(const CHAR16 *what, EFI_STATUS st);

/* fs.c: files on the volume flboot was loaded from (the ESP) */
EFI_STATUS fs_init(void);
EFI_HANDLE fs_device(void);
/* Read a whole file (path like L"\\flboot\\flboot.conf"); NUL-terminated. */
EFI_STATUS fs_read(const CHAR16 *path, char **data, UINTN *size);
/* Call FN for every regular file in directory PATH. */
EFI_STATUS fs_list(const CHAR16 *path, void (*fn)(const CHAR16 *name, void *ctx), void *ctx);

/* conf.c */
#define MAX_ENTRIES 48

struct entry {
	char *title;
	CHAR16 *kernel;  /* path on the ESP */
	char *cmdline;   /* kernel command line */
	char *version;   /* kernel version, for sorting */
	bool snapshot;   /* an earlier state (btrfs snapshot), listed under "Start an earlier state" */
};

struct config {
	int timeout;     /* seconds; 0 boots the default at once, -1 waits */
	char *cmdline;   /* default kernel command line */
	char *def;       /* title or kernel version of the default entry */
	struct entry entries[MAX_ENTRIES];
	int n;
};
void conf_load(struct config *c);

/* gfx.c: the graphical console */
bool gfx_init(void);
void gfx_clear(UINT32 rgb);
void gfx_rect(int x, int y, int w, int h, UINT32 rgb);
void gfx_text(int x, int y, const char *s, UINT32 rgb, int scale);
int gfx_width(void);
int gfx_height(void);
int gfx_text_width(const char *s, int scale);
extern const int font_width, font_height;

/* menu.c */
int menu_run(struct config *c); /* index of the entry to boot */

/* boot.c */
EFI_STATUS boot_entry(const struct entry *e);

#endif
