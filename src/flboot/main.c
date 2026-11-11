/* flboot -- the FreeLFS boot loader: shows the menu, starts the kernel.
 * SPDX-License-Identifier: MIT */
#include "flboot.h"

EFI_HANDLE image_handle;
EFI_SYSTEM_TABLE *ST;
EFI_BOOT_SERVICES *BS;

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	image_handle = image;
	ST = st;
	BS = st->BootServices;
	BS->SetWatchdogTimer(0, 0, 0, NULL); /* the firmware's 5 minute limit */

	EFI_STATUS s = fs_init();
	if (EFI_ERROR(s))
		fatal(L"cannot read the EFI system partition", s);
	struct config c;
	conf_load(&c);
	if (!c.n)
		fatal(L"no kernels (\\vmlinuz-*) on the EFI system partition", 0);

	for (;;) {
		int i = menu_run(&c);
		s = boot_entry(&c.entries[i]);
		/* the kernel did not start: say so, then show the menu again */
		ST->ConOut->ClearScreen(ST->ConOut);
		print(L"flboot: could not start ");
		print_ascii(c.entries[i].title);
		print(L"\r\n");
		BS->Stall(3000000);
		c.timeout = -1;
	}
}
