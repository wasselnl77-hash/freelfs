/* Files on the volume flboot was loaded from: the EFI system partition.
 * SPDX-License-Identifier: MIT */
#include "flboot.h"

static EFI_FILE_PROTOCOL *root;
static EFI_HANDLE device;

/* The volume flboot was loaded from. When the firmware shows no file system
 * there (a Ventoy virtual disk or a CD without a FAT boot image handle), use
 * any volume that has flboot/flboot.conf, so the live ISO still boots. */
EFI_STATUS fs_init(void)
{
	static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	static const EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_LOADED_IMAGE_PROTOCOL *li;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
	EFI_STATUS st = BS->HandleProtocol(image_handle, &li_guid, (void **)&li);
	if (EFI_ERROR(st))
		return st;
	device = li->DeviceHandle;
	st = device ? BS->HandleProtocol(device, &fs_guid, (void **)&sfs) : EFI_NOT_FOUND;
	if (!EFI_ERROR(st))
		return sfs->OpenVolume(sfs, &root);

	UINTN n = 0;
	EFI_HANDLE *h = NULL;
	if (EFI_ERROR(BS->LocateHandleBuffer(2 /* ByProtocol */, &fs_guid, NULL, &n, &h)))
		return st;
	for (UINTN i = 0; i < n; i++) {
		EFI_FILE_PROTOCOL *r, *f;
		if (EFI_ERROR(BS->HandleProtocol(h[i], &fs_guid, (void **)&sfs)) ||
		    EFI_ERROR(sfs->OpenVolume(sfs, &r)))
			continue;
		if (!EFI_ERROR(r->Open(r, &f, L"\\flboot\\flboot.conf", EFI_FILE_MODE_READ, 0))) {
			f->Close(f);
			device = h[i];
			root = r;
			BS->FreePool(h);
			return EFI_SUCCESS;
		}
		r->Close(r);
	}
	BS->FreePool(h);
	return st;
}

EFI_HANDLE fs_device(void)
{
	return device;
}

static EFI_FILE_INFO *file_info(EFI_FILE_PROTOCOL *f)
{
	static const EFI_GUID info_guid = EFI_FILE_INFO_GUID;
	UINTN size = 0;
	EFI_STATUS st = f->GetInfo(f, &info_guid, &size, NULL);
	if (st != EFI_BUFFER_TOO_SMALL)
		return NULL;
	EFI_FILE_INFO *info = xalloc(size);
	if (EFI_ERROR(f->GetInfo(f, &info_guid, &size, info))) {
		xfree(info);
		return NULL;
	}
	return info;
}

EFI_STATUS fs_read(const CHAR16 *path, char **data, UINTN *size)
{
	EFI_FILE_PROTOCOL *f;
	EFI_STATUS st = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(st))
		return st;
	EFI_FILE_INFO *info = file_info(f);
	if (!info) {
		f->Close(f);
		return EFI_LOAD_ERROR;
	}
	UINTN n = info->FileSize;
	xfree(info);
	char *buf = xalloc(n + 1);
	UINTN got = n;
	st = f->Read(f, &got, buf);
	f->Close(f);
	if (EFI_ERROR(st) || got != n) {
		xfree(buf);
		return EFI_ERROR(st) ? st : EFI_LOAD_ERROR;
	}
	*data = buf;
	if (size)
		*size = n;
	return EFI_SUCCESS;
}

EFI_STATUS fs_list(const CHAR16 *path, void (*fn)(const CHAR16 *name, void *ctx), void *ctx)
{
	EFI_FILE_PROTOCOL *dir;
	EFI_STATUS st = root->Open(root, &dir, path, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(st))
		return st;
	UINTN cap = 1024;
	EFI_FILE_INFO *info = xalloc(cap);
	for (;;) {
		UINTN size = cap;
		st = dir->Read(dir, &size, info);
		if (st == EFI_BUFFER_TOO_SMALL) {
			xfree(info);
			cap = size;
			info = xalloc(cap);
			continue;
		}
		if (EFI_ERROR(st) || size == 0)
			break;
		if (!(info->Attribute & EFI_FILE_DIRECTORY))
			fn(info->FileName, ctx);
	}
	xfree(info);
	dir->Close(dir);
	return EFI_ERROR(st) ? st : EFI_SUCCESS;
}
