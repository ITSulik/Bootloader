#include "boot_menu_logic.h"

#define VGA_TEXT_BUFFER ((volatile u16 *)0xB8000u)
#define VGA_COLUMNS 80u
#define VGA_ROWS 25u


/// Helper functions for working with VGA text mode. These are not strictly necessary, but they keep the code clean and focused on the logic of the boot menu.
static u16 make_vga_cell(char character, u8 color) {
    return (u16)(((u16)color << 8) | (u8)character);
}

// Clears the screen by filling the VGA text buffer with spaces in the specified color.
static void clear_screen(u8 color) {
    u32 index;

    for (index = 0; index < VGA_COLUMNS * VGA_ROWS; ++index) {
        VGA_TEXT_BUFFER[index] = make_vga_cell(' ', color);
    }
}

// Writes a null-terminated string to the VGA text buffer at the specified row and column with the given color.
static void write_string(u32 row, u32 column, u8 color, const char *text) {
    u32 index;

    index = row * VGA_COLUMNS + column;

    while (*text != '\0') {
        VGA_TEXT_BUFFER[index] = make_vga_cell(*text, color);
        ++index;
        ++text;
    }
}

static void write_hex_byte(u32 row, u32 column, u8 color, u8 value) {
    static const char hex_digits[] = "0123456789ABCDEF";

    VGA_TEXT_BUFFER[row * VGA_COLUMNS + column] =
        make_vga_cell(hex_digits[(value >> 4) & 0x0Fu], color);
    VGA_TEXT_BUFFER[row * VGA_COLUMNS + column + 1u] =
        make_vga_cell(hex_digits[value & 0x0Fu], color);
}

static int boot_info_is_valid(const BootInfo *boot_info) {
    return boot_info->signature[0] == 'S' &&
           boot_info->signature[1] == 'B' &&
           boot_info->signature[2] == 'I' &&
           boot_info->signature[3] == '1' &&
           boot_info->version == 1u;
}

static const char *selected_target_label(u8 selected_target) {
    if (selected_target == BOOT_TARGET_LINUX) {
        return "Linux";
    }

    if (selected_target == BOOT_TARGET_WINDOWS) {
        return "Windows";
    }

    return "Unknown";
}

void boot_menu_main(void) {
    const BootInfo *boot_info;

    /*
     * Stage 1 leaves a tiny handoff structure in low memory before entering
     * protected mode. Reading it here lets us keep Stage 2 focused on logic
     * instead of keyboard handling details from the boot sector.
     */
    boot_info = (const BootInfo *)BOOT_INFO_ADDR;

    clear_screen(0x1F);
    write_string(2u, 4u, 0x1F, "Soul Boot");
    write_string(4u, 4u, 0x1E, "Menu loader build is alive.");

    if (!boot_info_is_valid(boot_info)) {
        write_string(6u, 4u, 0x4F, "Boot info block is missing or invalid.");
        return;
    }

    write_string(6u, 4u, 0x1F, "Requested target:");
    write_string(6u, 22u, 0x1E, selected_target_label(boot_info->selected_target));

    write_string(8u, 4u, 0x1F, "Boot drive:");
    write_string(8u, 16u, 0x1E, "0x");
    write_hex_byte(8u, 18u, 0x1E, boot_info->boot_drive);

    write_string(10u, 4u, 0x1F, "Next step: inspect the disk layout for that target.");
}
