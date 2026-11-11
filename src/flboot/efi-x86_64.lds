/* Linker script for flboot on x86_64: an ELF shared object at address 0
 * whose sections objcopy turns into a PE image (efi-app-x86_64).
 * SPDX-License-Identifier: MIT */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_start)

SECTIONS
{
	. = 0;
	ImageBase = .;
	/* room for the PE headers objcopy writes */
	.hash : { *(.hash) }
	.gnu.hash : { *(.gnu.hash) }
	. = ALIGN(4096);
	.text : {
		*(.text .text.*)
		. = ALIGN(16);
	}
	. = ALIGN(4096);
	.reloc : { *(.reloc) }
	. = ALIGN(4096);
	/* everything else that is loaded, bss included (the PE image gets it
	 * as initialised zeroes) */
	.data : {
		*(.rodata .rodata.*)
		*(.got.plt) *(.got)
		*(.data .data.*)
		*(.sdata) *(.sbss) *(.scommon)
		*(.dynbss) *(.bss .bss.*) *(COMMON)
	}
	. = ALIGN(4096);
	.dynamic : { *(.dynamic) }
	. = ALIGN(4096);
	.rela : { *(.rela.dyn) *(.rela.*) }
	. = ALIGN(4096);
	.dynsym : { *(.dynsym) }
	. = ALIGN(4096);
	.dynstr : { *(.dynstr) }
	/DISCARD/ : {
		*(.eh_frame) *(.note.GNU-stack) *(.note.gnu.property) *(.comment)
		*(.gnu_debuglink) *(.interp)
	}
}
