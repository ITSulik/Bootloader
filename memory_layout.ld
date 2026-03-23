/*
 * Linker script for the larger menu loader.
 *
 * The BIOS boot sector is only 512 bytes, so the bigger logic has to live
 * somewhere else in memory. We will teach stage 1 to read this binary into
 * 0x10000 later, so we link it as if it will execute from that address.
 */
OUTPUT_FORMAT(elf32-i386)
ENTRY(menu_loader_entry)

SECTIONS
{
    . = 0x10000;
    menu_loader_start = .;

    .text ALIGN(16) :
    {
        *(.text*)
    }

    .rodata ALIGN(16) :
    {
        *(.rodata*)
    }

    .data ALIGN(16) :
    {
        *(.data*)
    }

    .bss ALIGN(16) :
    {
        *(COMMON)
        *(.bss*)
    }

    menu_loader_end = .;
    menu_loader_size = menu_loader_end - menu_loader_start;

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame)
        *(.note*)
    }
}
