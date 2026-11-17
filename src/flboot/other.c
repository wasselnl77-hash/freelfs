/* Entries for the rest of the computer: the firmware's setup screen, the firmware's own boot options (BootNext) and
 * other drives that hold an EFI program. A laptop with "fast boot" never shows its F2/F12 keys, so the way into the
 * BIOS or onto a USB stick has to be here.
 * SPDX-License-Identifier: MIT */
#include "flboot.h"

static const EFI_GUID global_guid = EFI_GLOBAL_VARIABLE_GUID;
static const EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
static const EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;

static struct entry *new_entry(struct config *c, int kind, char *title)
{
	if (c->n >= MAX_ENTRIES)
		return NULL;
	struct entry *e = &c->entries[c->n++];
	memset(e, 0, sizeof *e);
	e->kind = kind;
	e->title = title;
	e->cmdline = "";
	return e;
}

static char *cat3(const char *a, const char *b, const char *c)
{
	size_t la = strlen(a), lb = strlen(b), lc = strlen(c);
	char *r = xalloc(la + lb + lc + 1);
	memcpy(r, a, la);
	memcpy(r + la, b, lb);
	memcpy(r + la + lb, c, lc + 1);
	return r;
}

static char *wide_to_ascii(const CHAR16 *w)
{
	size_t n = wcslen16(w);
	char *r = xalloc(n + 1);
	for (size_t i = 0; i < n; i++)
		r[i] = w[i] >= 32 && w[i] < 127 ? (char)w[i] : '?';
	r[n] = 0;
	return r;
}

static void hex4(UINT16 v, CHAR16 *out)
{
	for (int i = 0; i < 4; i++) {
		int d = (v >> (12 - 4 * i)) & 15;
		out[i] = (CHAR16)(d < 10 ? '0' + d : 'A' + d - 10);
	}
}

/* is there a file PATH on the volume of HANDLE? */
static bool has_file(EFI_HANDLE h, const CHAR16 *path)
{
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
	EFI_FILE_PROTOCOL *r, *f;
	if (EFI_ERROR(BS->HandleProtocol(h, &fs_guid, (void **)&sfs)) || EFI_ERROR(sfs->OpenVolume(sfs, &r)))
		return false;
	bool ok = !EFI_ERROR(r->Open(r, &f, path, EFI_FILE_MODE_READ, 0));
	if (ok)
		f->Close(f);
	r->Close(r);
	return ok;
}

static void add_chain(struct config *c, EFI_HANDLE h, const CHAR16 *path, const char *what, int volno)
{
	char num[4] = {(char)('0' + volno % 10), 0, 0, 0};
	struct entry *e = new_entry(c, K_CHAIN, cat3(what, " (drive ", num));
	if (!e)
		return;
	size_t n = strlen(e->title);
	char *t = xalloc(n + 2);
	memcpy(t, e->title, n);
	t[n] = ')';
	t[n + 1] = 0;
	e->title = t;
	e->volume = h;
	e->kernel = str16_cat(path, L"");
}

void conf_add_other(struct config *c)
{
	EFI_RUNTIME_SERVICES *rt = ST->RuntimeServices;
	UINT64 supported = 0;
	UINTN sz = sizeof supported;
	if (rt && rt->GetVariable && !EFI_ERROR(rt->GetVariable(L"OsIndicationsSupported", &global_guid, NULL, &sz, &supported)) &&
	    (supported & EFI_OS_INDICATIONS_BOOT_TO_FW_UI))
		new_entry(c, K_FIRMWARE, "Firmware setup (BIOS)");

	/* the firmware's boot options, in its order, except the one that started us */
	if (rt && rt->GetVariable) {
		UINT16 cur = 0xffff, order[64];
		UINTN s2 = sizeof cur;
		if (EFI_ERROR(rt->GetVariable(L"BootCurrent", &global_guid, NULL, &s2, &cur)))
			cur = 0xffff;
		UINTN osz = sizeof order;
		if (!EFI_ERROR(rt->GetVariable(L"BootOrder", &global_guid, NULL, &osz, order))) {
			for (UINTN i = 0; i < osz / 2 && i < 64; i++) {
				if (order[i] == cur)
					continue;
				CHAR16 name[9] = L"Boot0000";
				hex4(order[i], name + 4);
				static UINT8 buf[1024];
				UINTN bsz = sizeof buf;
				if (EFI_ERROR(rt->GetVariable(name, &global_guid, NULL, &bsz, buf)) || bsz < 8)
					continue;
				UINT32 attr = buf[0] | buf[1] << 8 | buf[2] << 16 | (UINT32)buf[3] << 24;
				if (attr & 8) /* LOAD_OPTION_CATEGORY_APP: not a way to boot */
					continue;
				CHAR16 *desc = (CHAR16 *)(buf + 6);
				if (!desc[0])
					continue;
				if (((UINTN)(buf + bsz) - (UINTN)desc) / 2 <= wcslen16(desc)) /* no terminator */
					continue;
				struct entry *e = new_entry(c, K_BOOTNEXT, cat3("Firmware option: ", wide_to_ascii(desc), ""));
				if (!e)
					break;
				e->bootnum = order[i];
			}
		}
	}

	/* other drives with an EFI program on them (a USB stick, the other system's ESP) */
	UINTN n = 0;
	EFI_HANDLE *h = NULL;
	if (EFI_ERROR(BS->LocateHandleBuffer(2, &fs_guid, NULL, &n, &h)))
		return;
	int vol = 0;
	for (UINTN i = 0; i < n && vol < 9; i++) {
		if (h[i] == fs_device())
			continue;
		vol++;
		if (has_file(h[i], L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi"))
			add_chain(c, h[i], L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi", "Windows", vol);
		if (has_file(h[i], L"\\EFI\\BOOT\\BOOTX64.EFI"))
			add_chain(c, h[i], L"\\EFI\\BOOT\\BOOTX64.EFI", "Other drive: EFI boot program", vol);
	}
	BS->FreePool(h);
}

/* Start a K_ entry that is not a kernel. The first two never come back. */
EFI_STATUS boot_other(const struct entry *e)
{
	EFI_RUNTIME_SERVICES *rt = ST->RuntimeServices;
	if (e->kind == K_FIRMWARE) {
		UINT64 v = 0;
		UINTN sz = sizeof v;
		if (EFI_ERROR(rt->GetVariable(L"OsIndications", &global_guid, NULL, &sz, &v)))
			v = 0;
		v |= EFI_OS_INDICATIONS_BOOT_TO_FW_UI;
		EFI_STATUS st = rt->SetVariable(L"OsIndications", &global_guid, EFI_VARIABLE_NV_BS_RT, sizeof v, &v);
		if (EFI_ERROR(st))
			return st;
		rt->ResetSystem(EfiResetCold, 0, 0, NULL);
		return EFI_UNSUPPORTED;
	}
	if (e->kind == K_BOOTNEXT) {
		UINT16 v = e->bootnum;
		EFI_STATUS st = rt->SetVariable(L"BootNext", &global_guid, EFI_VARIABLE_NV_BS_RT, sizeof v, &v);
		if (EFI_ERROR(st))
			return st;
		rt->ResetSystem(EfiResetCold, 0, 0, NULL);
		return EFI_UNSUPPORTED;
	}
	/* K_CHAIN: the device path of the volume plus a file path node, then the firmware loads and starts it */
	static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_DEVICE_PATH_PROTOCOL *base;
	EFI_STATUS st = BS->HandleProtocol(e->volume, &dp_guid, (void **)&base);
	if (EFI_ERROR(st))
		return st;
	UINTN blen = 0;
	for (EFI_DEVICE_PATH_PROTOCOL *p = base; !(p->Type == 0x7f && p->SubType == 0xff);
	     p = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)p + (p->Length[0] | p->Length[1] << 8)))
		blen += p->Length[0] | p->Length[1] << 8;
	UINTN flen = (wcslen16(e->kernel) + 1) * 2, nodelen = 4 + flen;
	UINT8 *dp = xalloc(blen + nodelen + 4);
	memcpy(dp, base, blen);
	dp[blen] = 4; /* media */
	dp[blen + 1] = 4; /* file path */
	dp[blen + 2] = (UINT8)nodelen;
	dp[blen + 3] = (UINT8)(nodelen >> 8);
	memcpy(dp + blen + 4, e->kernel, flen);
	dp[blen + nodelen] = 0x7f;
	dp[blen + nodelen + 1] = 0xff;
	dp[blen + nodelen + 2] = 4;
	dp[blen + nodelen + 3] = 0;
	EFI_HANDLE img;
	st = BS->LoadImage(false, image_handle, (EFI_DEVICE_PATH_PROTOCOL *)dp, NULL, 0, &img);
	xfree(dp);
	if (EFI_ERROR(st))
		return st;
	(void)li_guid;
	st = BS->StartImage(img, NULL, NULL);
	BS->UnloadImage(img);
	return st;
}
