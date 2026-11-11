/* flboot's entry point. flboot is linked as an ELF shared object at address
 * 0 and converted to a PE image; the firmware loads it anywhere, so before
 * anything else it applies its own R_*_RELATIVE relocations.
 * SPDX-License-Identifier: MIT */
#include <elf.h>

#include "efi.h"

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st);

extern char ImageBase[] __attribute__((visibility("hidden")));
extern Elf64_Dyn _DYNAMIC[] __attribute__((visibility("hidden")));

#if defined(__x86_64__)
#define R_RELATIVE R_X86_64_RELATIVE
#elif defined(__aarch64__)
#define R_RELATIVE R_AARCH64_RELATIVE
#endif

static void relocate(void)
{
	uintptr_t base = (uintptr_t)ImageBase;
	Elf64_Rela *rela = NULL;
	UINT64 size = 0, ent = sizeof(Elf64_Rela);
	for (Elf64_Dyn *d = _DYNAMIC; d->d_tag != DT_NULL; d++) {
		if (d->d_tag == DT_RELA)
			rela = (Elf64_Rela *)(base + d->d_un.d_ptr);
		else if (d->d_tag == DT_RELASZ)
			size = d->d_un.d_val;
		else if (d->d_tag == DT_RELAENT)
			ent = d->d_un.d_val;
	}
	for (UINT64 off = 0; rela && off < size; off += ent) {
		Elf64_Rela *r = (Elf64_Rela *)((char *)rela + off);
		if (ELF64_R_TYPE(r->r_info) == R_RELATIVE)
			*(UINT64 *)(base + r->r_offset) = base + r->r_addend;
	}
}

EFI_STATUS EFIAPI efi_start(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
	relocate();
	return efi_main(image, st);
}

/* An empty base relocation block: the firmware only loads images it can
 * relocate, and the real relocations were applied above. */
__attribute__((section(".reloc"), used)) static const UINT8 dummy_reloc[10] = {0, 0, 0, 0, 10};
