/* Small helpers: memory, strings, version comparison, output.
 * There is no C library here; gcc may still emit calls to memcpy, memset,
 * memmove and memcmp, so those are defined too.
 * SPDX-License-Identifier: MIT */
#include "flboot.h"

void *memcpy(void *dst, const void *src, size_t n)
{
	UINT8 *d = dst;
	const UINT8 *s = src;
	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	UINT8 *d = dst;
	const UINT8 *s = src;
	if (d < s)
		while (n--)
			*d++ = *s++;
	else
		while (n--)
			d[n] = s[n];
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	UINT8 *d = dst;
	while (n--)
		*d++ = (UINT8)c;
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const UINT8 *x = a, *y = b;
	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x < *y ? -1 : 1;
	return 0;
}

size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

void *xalloc(UINTN size)
{
	void *p = NULL;
	EFI_STATUS st = BS->AllocatePool(EfiLoaderData, size ? size : 1, &p);
	if (EFI_ERROR(st))
		fatal(L"out of memory", st);
	memset(p, 0, size);
	return p;
}

void xfree(void *p)
{
	if (p)
		BS->FreePool(p);
}

size_t wcslen16(const CHAR16 *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

CHAR16 *ascii_to16(const char *s)
{
	size_t n = strlen(s);
	CHAR16 *w = xalloc((n + 1) * sizeof(CHAR16));
	for (size_t i = 0; i < n; i++)
		w[i] = (UINT8)s[i];
	return w;
}

char *ascii_dup(const char *s, size_t n)
{
	char *d = xalloc(n + 1);
	memcpy(d, s, n);
	return d;
}

CHAR16 *str16_cat(const CHAR16 *a, const CHAR16 *b)
{
	size_t na = wcslen16(a), nb = wcslen16(b);
	CHAR16 *w = xalloc((na + nb + 1) * sizeof(CHAR16));
	memcpy(w, a, na * sizeof(CHAR16));
	memcpy(w + na, b, nb * sizeof(CHAR16));
	return w;
}

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static bool is_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Segment-wise comparison like flpkg's: digit runs numerically, letter
 * runs as strings, digits beat letters. Enough to order kernel versions. */
int vercmp(const char *a, const char *b)
{
	for (;;) {
		while (*a && !is_digit(*a) && !is_alpha(*a))
			a++;
		while (*b && !is_digit(*b) && !is_alpha(*b))
			b++;
		if (!*a || !*b)
			return *a ? 1 : *b ? -1 : 0;
		bool da = is_digit(*a), db = is_digit(*b);
		if (da != db)
			return da ? 1 : -1;
		const char *ea = a, *eb = b;
		if (da) {
			while (*a == '0')
				a++, ea++;
			while (*b == '0')
				b++, eb++;
			while (is_digit(*ea))
				ea++;
			while (is_digit(*eb))
				eb++;
			if (ea - a != eb - b)
				return ea - a < eb - b ? -1 : 1;
		} else {
			while (is_alpha(*ea))
				ea++;
			while (is_alpha(*eb))
				eb++;
		}
		for (; a < ea && b < eb; a++, b++)
			if (*a != *b)
				return *a < *b ? -1 : 1;
		if (a != ea || b != eb)
			return a != ea ? 1 : -1;
	}
}

void print(const CHAR16 *s)
{
	ST->ConOut->OutputString(ST->ConOut, s);
}

void print_ascii(const char *s)
{
	CHAR16 *w = ascii_to16(s);
	print(w);
	xfree(w);
}

static void print_hex(UINT64 v)
{
	CHAR16 buf[19] = L"0x";
	for (int i = 0; i < 16; i++)
		buf[2 + i] = L"0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
	buf[18] = 0;
	print(buf);
}

void fatal(const CHAR16 *what, EFI_STATUS st)
{
	print(L"\r\nflboot: ");
	print(what);
	if (st) {
		print(L" (status ");
		print_hex(st);
		print(L")");
	}
	print(L"\r\nPress any key to return to the firmware.\r\n");
	EFI_INPUT_KEY key;
	UINTN idx;
	ST->ConIn->Reset(ST->ConIn, false);
	BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &idx);
	ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
	BS->Exit(image_handle, st ? st : EFI_LOAD_ERROR, 0, NULL);
	for (;;)
		;
}
