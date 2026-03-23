#ifndef BOOT_MENU_LOGIC_H
#define BOOT_MENU_LOGIC_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

enum {
    BOOT_INFO_ADDR = 0x0500u,
    BOOT_TARGET_LINUX = 0x01u,
    BOOT_TARGET_WINDOWS = 0x02u
};

typedef struct BootInfo {
    u8 signature[4];
    u8 version;
    u8 selected_target;
    u8 boot_drive;
    u8 reserved;
} BootInfo;

/*
 * The C side of the larger boot menu loader starts here after the
 * assembly entry point has set up a protected-mode stack.
 */
void boot_menu_main(void);

#endif /* BOOT_MENU_LOGIC_H */
