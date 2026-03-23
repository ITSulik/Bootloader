#ifndef BOOT_MENU_LOGIC_H
#define BOOT_MENU_LOGIC_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

enum {
    BOOT_INFO_ADDR = 0x0500u,
    MBR_SECTOR_ADDR = 0x0600u,
    GPT_HEADER_ADDR = 0x0800u,
    GPT_ENTRY_SECTOR_ADDR = 0x0A00u,
    CHAINLOAD_BUFFER_ADDR = 0x7E00u,
    BOOT_TARGET_LINUX = 0x01u,
    BOOT_TARGET_WINDOWS = 0x02u,
    BOOT_INFO_FLAG_MBR_VALID = 0x01u,
    BOOT_INFO_FLAG_GPT_HEADER_VALID = 0x02u,
    BOOT_INFO_FLAG_GPT_ENTRY_VALID = 0x04u,
    BOOT_INFO_FLAG_CHAINLOAD_MATCH = 0x08u,
    BOOT_INFO_FLAG_CHAINLOAD_READ_OK = 0x10u
};

typedef struct BootInfo {
    u8 signature[4];
    u8 version;
    u8 selected_target;
    u8 boot_drive;
    u8 inspection_flags;
} BootInfo;

/*
 * The C side of the larger boot menu loader starts here after the
 * assembly entry point has set up a protected-mode stack.
 */
void boot_menu_main(void);

#endif /* BOOT_MENU_LOGIC_H */
