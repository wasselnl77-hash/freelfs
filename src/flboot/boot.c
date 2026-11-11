/* Starting a kernel: load its EFI stub image from the ESP, hand it the
 * command line as load options, and start it.
 * SPDX-License-Identifier: MIT */
#include "flboot.h"

EFI_STATUS boot_entry(const struct entry *e)
{
	static const EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	char *data;
	UINTN size;
	EFI_STATUS st = fs_read(e->kernel, &data, &size);
	if (EFI_ERROR(st))
		return st;

	EFI_HANDLE kernel;
	st = BS->LoadImage(false, image_handle, NULL, data, size, &kernel);
	xfree(data);
	if (EFI_ERROR(st))
		return st;

	EFI_LOADED_IMAGE_PROTOCOL *li;
	st = BS->HandleProtocol(kernel, &li_guid, (void **)&li);
	if (EFI_ERROR(st))
		return st;
	CHAR16 *options = ascii_to16(e->cmdline);
	li->LoadOptions = options;
	li->LoadOptionsSize = (UINT32)((wcslen16(options) + 1) * sizeof(CHAR16));
	li->DeviceHandle = fs_device(); /* so initrd= paths resolve on the ESP */

	st = BS->StartImage(kernel, NULL, NULL);
	/* only back here if the kernel failed to start */
	BS->UnloadImage(kernel);
	xfree(options);
	return st;
}
