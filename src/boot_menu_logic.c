#include "boot_menu_logic.h"

#define VGA_TEXT_BUFFER ((volatile u16 *)0xB8000u)
#define VGA_COLUMNS 80u
#define VGA_ROWS 25u
#define PARTITION_ENTRY_OFFSET 446u
#define PARTITION_ENTRY_SIZE 16u
#define GPT_ENTRY_SIZE 128u
#define GPT_ENTRY_COUNT_PER_SECTOR 4u
#define GPT_ENTRY_DISPLAY_COUNT 2u
#define EXT_INCOMPAT_FILETYPE 0x0002u
#define EXT_INCOMPAT_META_BG 0x0010u
#define EXT_INCOMPAT_EXTENTS 0x0040u
#define EXT_INCOMPAT_64BIT 0x0080u
#define EXT_INCOMPAT_FLEX_BG 0x0200u
#define EXT_INCOMPAT_INLINE_DATA 0x8000u
#define EXT_INCOMPAT_ENCRYPT 0x10000u
#define EXT_INCOMPAT_CASEFOLD 0x20000u
#define EXT4_INODE_FLAG_EXTENTS 0x00080000u
#define EXT_MODE_DIRECTORY 0x4000u
#define EXT_MODE_REGULAR 0x8000u

typedef struct MbrPartitionEntry {
    u8 boot_indicator;
    u8 start_chs[3];
    u8 partition_type;
    u8 end_chs[3];
    u8 start_lba[4];
    u8 sector_count[4];
} MbrPartitionEntry;

typedef struct GptPartitionEntry {
    u8 type_guid[16];
    u8 unique_guid[16];
    u8 first_lba[8];
    u8 last_lba[8];
    u8 attributes[8];
    u8 name[72];
} GptPartitionEntry;

typedef struct TargetCandidate {
    const char *origin;
    const char *kind;
    u32 first_lba;
    u32 partition_number;
    int found;
    int priority;
} TargetCandidate;

typedef struct BootPlan {
    const char *request_label;
    const char *status_label;
    const char *action_label;
    const char *support_label;
    TargetCandidate candidate;
    int ready;
} BootPlan;

typedef enum VbrKind {
    VBR_KIND_UNREAD = 0,
    VBR_KIND_NTFS,
    VBR_KIND_FAT,
    VBR_KIND_EXFAT,
    VBR_KIND_GENERIC_BOOT,
    VBR_KIND_DATA,
    VBR_KIND_UNKNOWN
} VbrKind;

static void identify_windows_probe_targets(const BootInfo *boot_info,
                                          TargetCandidate *efi_candidate,
                                          TargetCandidate *data_candidate);
static const char *windows_probe_hint(const BootInfo *boot_info,
                                      const TargetCandidate *efi_candidate,
                                      const TargetCandidate *data_candidate);
static const char *windows_probe_next_action(const BootInfo *boot_info,
                                             const TargetCandidate *efi_candidate,
                                             const TargetCandidate *data_candidate);

static const u8 EFI_SYSTEM_GUID[16] = {
    0x28u, 0x73u, 0x2Au, 0xC1u, 0x1Fu, 0xF8u, 0xD2u, 0x11u,
    0xBAu, 0x4Bu, 0x00u, 0xA0u, 0xC9u, 0x3Eu, 0xC9u, 0x3Bu
};
static const u8 MICROSOFT_BASIC_GUID[16] = {
    0xA2u, 0xA0u, 0xD0u, 0xEBu, 0xE5u, 0xB9u, 0x33u, 0x44u,
    0x87u, 0xC0u, 0x68u, 0xB6u, 0xB7u, 0x26u, 0x99u, 0xC7u
};
static const u8 LINUX_FILESYSTEM_GUID[16] = {
    0xAFu, 0x3Du, 0xC6u, 0x0Fu, 0x83u, 0x84u, 0x72u, 0x47u,
    0x8Eu, 0x79u, 0x3Du, 0x69u, 0xD8u, 0x47u, 0x7Du, 0xE4u
};
static const u8 LINUX_SWAP_GUID[16] = {
    0x6Du, 0xFDu, 0x57u, 0x06u, 0xABu, 0xA4u, 0xC4u, 0x43u,
    0x84u, 0xE5u, 0x09u, 0x33u, 0xC8u, 0x4Bu, 0x4Fu, 0x4Fu
};
static const u8 MICROSOFT_RESERVED_GUID[16] = {
    0x16u, 0xE3u, 0xC9u, 0xE3u, 0x5Cu, 0x0Bu, 0xB8u, 0x4Du,
    0x81u, 0x7Du, 0xF9u, 0x2Du, 0xF0u, 0x02u, 0x15u, 0xAEu
};
static const u8 WINDOWS_RECOVERY_GUID[16] = {
    0xA4u, 0xBBu, 0x94u, 0xDEu, 0xD1u, 0x06u, 0x40u, 0x4Du,
    0xA1u, 0x6Au, 0xBFu, 0xD5u, 0x01u, 0x79u, 0xD6u, 0xACu
};


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

static u16 read_le_u16(const u8 *bytes) {
    return (u16)((u16)bytes[0] | ((u16)bytes[1] << 8));
}

static u32 read_le_u32(const u8 *bytes) {
    return (u32)bytes[0] |
           ((u32)bytes[1] << 8) |
           ((u32)bytes[2] << 16) |
           ((u32)bytes[3] << 24);
}

static int bytes_match(const u8 *left, const u8 *right, u32 length) {
    u32 index;

    for (index = 0u; index < length; ++index) {
        if (left[index] != right[index]) {
            return 0;
        }
    }

    return 1;
}

static int bytes_are_zero(const u8 *bytes, u32 length) {
    u32 index;

    for (index = 0u; index < length; ++index) {
        if (bytes[index] != 0u) {
            return 0;
        }
    }

    return 1;
}

static u32 c_string_length(const char *text) {
    u32 length;

    length = 0u;
    while (text[length] != '\0') {
        ++length;
    }

    return length;
}

static int sector_has_jump_opcode(const u8 *sector) {
    return sector[0] == 0xEBu || sector[0] == 0xE9u;
}

static VbrKind detect_vbr_kind(const u8 *sector) {
    u16 signature;

    signature = read_le_u16(sector + 510u);

    if (signature != 0xAA55u) {
        if (bytes_are_zero(sector, 64u)) {
            return VBR_KIND_DATA;
        }

        return VBR_KIND_UNKNOWN;
    }

    if (bytes_match(sector + 3u, (const u8 *)"NTFS    ", 8u)) {
        return VBR_KIND_NTFS;
    }

    if (bytes_match(sector + 3u, (const u8 *)"EXFAT   ", 8u)) {
        return VBR_KIND_EXFAT;
    }

    if (bytes_match(sector + 3u, (const u8 *)"MSDOS5.0", 8u) ||
        bytes_match(sector + 3u, (const u8 *)"MSWIN4.1", 8u) ||
        bytes_match(sector + 3u, (const u8 *)"mkfs.fat", 8u) ||
        bytes_match(sector + 3u, (const u8 *)"FRDOS5.1", 8u)) {
        return VBR_KIND_FAT;
    }

    if (sector_has_jump_opcode(sector)) {
        return VBR_KIND_GENERIC_BOOT;
    }

    return VBR_KIND_UNKNOWN;
}

static const char *vbr_kind_label(VbrKind kind) {

    switch (kind)
    {
    case VBR_KIND_NTFS:
        return "NTFS VBR";
    case VBR_KIND_FAT:
        return "FAT VBR";
    case VBR_KIND_EXFAT:
        return "exFAT VBR";
    case VBR_KIND_GENERIC_BOOT:
        return "generic boot";
    case VBR_KIND_DATA:
        return "plain data";
    case VBR_KIND_UNREAD:
        return "not read";
    default:
        return "unknown VBR";
    }
}

static int linux_fat_vbr_is_valid(const BootInfo *boot_info) {
    const u8 *vbr;

    if (boot_info->selected_target != BOOT_TARGET_LINUX) {
        return 0;
    }

    if ((boot_info->inspection_flags & BOOT_INFO_FLAG_CHAINLOAD_READ_OK) == 0u) {
        return 0;
    }

    vbr = (const u8 *)CHAINLOAD_BUFFER_ADDR;
    if (detect_vbr_kind(vbr) != VBR_KIND_FAT) {
        return 0;
    }

    if (read_le_u16(vbr + 11u) != 512u) {
        return 0;
    }

    if (vbr[13] == 0u || vbr[16] == 0u) {
        return 0;
    }

    if (read_le_u32(vbr + 36u) == 0u || read_le_u32(vbr + 44u) < 2u) {
        return 0;
    }

    return 1;
}

static int linux_layout_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) ||
           (boot_info->inspection_flags & BOOT_INFO_FLAG_LINUX_LAYOUT_VALID) != 0u;
}

static u32 linux_fat_bytes_per_sector(const BootInfo *boot_info) {
    if (!linux_fat_vbr_is_valid(boot_info)) {
        return 0u;
    }

    return read_le_u16((const u8 *)CHAINLOAD_BUFFER_ADDR + 11u);
}

static u32 linux_fat_sectors_per_cluster(const BootInfo *boot_info) {
    if (!linux_fat_vbr_is_valid(boot_info)) {
        return 0u;
    }

    return ((const u8 *)CHAINLOAD_BUFFER_ADDR)[13];
}

static u32 linux_fat_reserved_sectors(const BootInfo *boot_info) {
    if (!linux_fat_vbr_is_valid(boot_info)) {
        return 0u;
    }

    return read_le_u16((const u8 *)CHAINLOAD_BUFFER_ADDR + 14u);
}

static u32 linux_fat_count(const BootInfo *boot_info) {
    if (!linux_fat_vbr_is_valid(boot_info)) {
        return 0u;
    }

    return ((const u8 *)CHAINLOAD_BUFFER_ADDR)[16];
}

static u32 linux_fat_size_sectors(const BootInfo *boot_info) {
    if (!linux_fat_vbr_is_valid(boot_info)) {
        return 0u;
    }

    return read_le_u32((const u8 *)CHAINLOAD_BUFFER_ADDR + 36u);
}

static u32 linux_fat_root_cluster(const BootInfo *boot_info) {
    if (!linux_fat_vbr_is_valid(boot_info)) {
        return 0u;
    }

    return read_le_u32((const u8 *)CHAINLOAD_BUFFER_ADDR + 44u);
}

static u32 linux_fat_root_dir_lba(const BootPlan *boot_plan, const BootInfo *boot_info) {
    u32 reserved_sectors;
    u32 fat_count;
    u32 fat_size_sectors;
    u32 root_cluster;
    u32 sectors_per_cluster;
    u32 first_data_sector;

    if (!linux_fat_vbr_is_valid(boot_info)) {
        return 0u;
    }

    reserved_sectors = linux_fat_reserved_sectors(boot_info);
    fat_count = linux_fat_count(boot_info);
    fat_size_sectors = linux_fat_size_sectors(boot_info);
    root_cluster = linux_fat_root_cluster(boot_info);
    sectors_per_cluster = linux_fat_sectors_per_cluster(boot_info);
    if (root_cluster < 2u || sectors_per_cluster == 0u) {
        return 0u;
    }

    first_data_sector = reserved_sectors + fat_count * fat_size_sectors;
    return boot_plan->candidate.first_lba + first_data_sector +
           (root_cluster - 2u) * sectors_per_cluster;
}

static u32 linux_fat_cluster_lba(const BootPlan *boot_plan,
                                 const BootInfo *boot_info,
                                 u32 cluster) {
    u32 reserved_sectors;
    u32 fat_count;
    u32 fat_size_sectors;
    u32 sectors_per_cluster;
    u32 first_data_sector;

    if (!linux_fat_vbr_is_valid(boot_info) || cluster < 2u) {
        return 0u;
    }

    reserved_sectors = linux_fat_reserved_sectors(boot_info);
    fat_count = linux_fat_count(boot_info);
    fat_size_sectors = linux_fat_size_sectors(boot_info);
    sectors_per_cluster = linux_fat_sectors_per_cluster(boot_info);
    if (sectors_per_cluster == 0u) {
        return 0u;
    }

    first_data_sector = reserved_sectors + fat_count * fat_size_sectors;
    return boot_plan->candidate.first_lba + first_data_sector +
           (cluster - 2u) * sectors_per_cluster;
}

static u8 boot_info_extra_flags(void) {
    return *(const u8 *)BOOT_INFO_EXTRA_FLAGS_ADDR;
}

static u8 boot_info_windows_flags(void) {
    return *(const u8 *)BOOT_INFO_WINDOWS_FLAGS_ADDR;
}

static u8 boot_info_windows_drive(void) {
    return *(const u8 *)BOOT_INFO_WINDOWS_DRIVE_ADDR;
}

static int linux_fat_root_dir_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) &&
           (boot_info->inspection_flags & BOOT_INFO_FLAG_ROOT_DIR_VALID) != 0u;
}

static int linux_fat_loader_dir_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) &&
           (boot_info->inspection_flags & BOOT_INFO_FLAG_CHILD_DIR_VALID) != 0u;
}

static int linux_fat_entries_dir_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) &&
           (boot_info->detail_flags & BOOT_INFO_DETAIL_FAT_ENTRIES_VALID) != 0u;
}

static int linux_fat_entry_file_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) &&
           (boot_info->detail_flags & BOOT_INFO_DETAIL_FAT_ENTRY_FILE_VALID) != 0u;
}

static int linux_fat_kernel_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) &&
           (boot_info->detail_flags & BOOT_INFO_DETAIL_FAT_KERNEL_VALID) != 0u;
}

static int linux_fat_initrd_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) &&
           (boot_info->detail_flags & BOOT_INFO_DETAIL_FAT_INITRD_VALID) != 0u;
}

static int linux_fat_efi_dir_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) &&
           (boot_info_extra_flags() & BOOT_INFO_EXTRA_FAT_EFI_DIR_VALID) != 0u;
}

static int linux_fat_efi_boot_dir_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) &&
           (boot_info_extra_flags() & BOOT_INFO_EXTRA_FAT_EFI_BOOT_DIR_VALID) != 0u;
}

static int linux_fat_efi_systemd_dir_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) &&
           (boot_info_extra_flags() & BOOT_INFO_EXTRA_FAT_EFI_SYSTEMD_DIR_VALID) != 0u;
}

static int linux_fat_efi_boot_file_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) &&
           (boot_info_extra_flags() & BOOT_INFO_EXTRA_FAT_EFI_BOOT_FILE_VALID) != 0u;
}

static int linux_fat_efi_systemd_file_probe_is_available(const BootInfo *boot_info) {
    return linux_fat_vbr_is_valid(boot_info) &&
           (boot_info_extra_flags() & BOOT_INFO_EXTRA_FAT_EFI_SYSTEMD_FILE_VALID) != 0u;
}

static int windows_probe_drive_is_available(const BootInfo *boot_info) {
    (void)boot_info;
    return boot_info_windows_drive() >= 0x80u;
}

static int windows_probe_mbr_is_valid(const BootInfo *boot_info) {
    return windows_probe_drive_is_available(boot_info) &&
           (boot_info_windows_flags() & BOOT_INFO_WINDOWS_FLAG_MBR_VALID) != 0u &&
           read_le_u16((const u8 *)WINDOWS_PROBE_MBR_ADDR + 510u) == 0xAA55u;
}

static int windows_probe_gpt_header_is_valid(const BootInfo *boot_info) {
    return windows_probe_drive_is_available(boot_info) &&
           (boot_info_windows_flags() & BOOT_INFO_WINDOWS_FLAG_GPT_HEADER_VALID) != 0u &&
           bytes_match((const u8 *)WINDOWS_PROBE_GPT_HEADER_ADDR,
                       (const u8 *)"EFI PART",
                       8u);
}

static int windows_probe_gpt_entries_are_available(const BootInfo *boot_info) {
    return windows_probe_gpt_header_is_valid(boot_info) &&
           (boot_info_windows_flags() & BOOT_INFO_WINDOWS_FLAG_GPT_ENTRY_VALID) != 0u;
}

static int windows_probe_esp_vbr_is_available(const BootInfo *boot_info) {
    return windows_probe_drive_is_available(boot_info) &&
           (boot_info_windows_flags() & BOOT_INFO_WINDOWS_FLAG_ESP_VBR_VALID) != 0u;
}

static int windows_probe_data_vbr_is_available(const BootInfo *boot_info) {
    return windows_probe_drive_is_available(boot_info) &&
           (boot_info_windows_flags() & BOOT_INFO_WINDOWS_FLAG_DATA_VBR_VALID) != 0u;
}

static const u8 *windows_probe_esp_vbr_sector_bytes(const BootInfo *boot_info) {
    if (!windows_probe_esp_vbr_is_available(boot_info)) {
        return 0;
    }

    return (const u8 *)WINDOWS_PROBE_ESP_VBR_ADDR;
}

static const u8 *windows_probe_data_vbr_sector_bytes(const BootInfo *boot_info) {
    if (!windows_probe_data_vbr_is_available(boot_info)) {
        return 0;
    }

    return (const u8 *)WINDOWS_PROBE_DATA_VBR_ADDR;
}

static const char *windows_probe_esp_vbr_label(const BootInfo *boot_info) {
    const u8 *sector;

    sector = windows_probe_esp_vbr_sector_bytes(boot_info);
    if (sector == 0) {
        return "(n/a)";
    }

    return vbr_kind_label(detect_vbr_kind(sector));
}

static const char *windows_probe_data_vbr_label(const BootInfo *boot_info) {
    const u8 *sector;

    sector = windows_probe_data_vbr_sector_bytes(boot_info);
    if (sector == 0) {
        return "(n/a)";
    }

    return vbr_kind_label(detect_vbr_kind(sector));
}

static int linux_ext_superblock_is_valid(const BootInfo *boot_info) {
    const u8 *probe_sector;

    if (!linux_layout_probe_is_available(boot_info)) {
        return 0;
    }

    probe_sector = (const u8 *)GPT_ENTRY_SECTOR_ADDR;
    return read_le_u16(probe_sector + 56u) == 0xEF53u;
}

static u32 linux_ext_block_size(const BootInfo *boot_info) {
    const u8 *probe_sector;
    u32 log_block_size;

    if (!linux_ext_superblock_is_valid(boot_info)) {
        return 0u;
    }

    probe_sector = (const u8 *)GPT_ENTRY_SECTOR_ADDR;
    log_block_size = read_le_u32(probe_sector + 24u);
    if (log_block_size > 4u) {
        return 0u;
    }

    return 1024u << log_block_size;
}

static u32 linux_ext_inode_size(const BootInfo *boot_info) {
    const u8 *probe_sector;
    u32 inode_size;

    if (!linux_ext_superblock_is_valid(boot_info)) {
        return 0u;
    }

    probe_sector = (const u8 *)GPT_ENTRY_SECTOR_ADDR;
    inode_size = read_le_u16(probe_sector + 88u);
    if (inode_size == 0u) {
        return 128u;
    }

    return inode_size;
}

static u32 linux_ext_blocks_per_group(const BootInfo *boot_info) {
    if (!linux_ext_superblock_is_valid(boot_info)) {
        return 0u;
    }

    return read_le_u32((const u8 *)GPT_ENTRY_SECTOR_ADDR + 32u);
}

static u32 linux_ext_inodes_per_group(const BootInfo *boot_info) {
    if (!linux_ext_superblock_is_valid(boot_info)) {
        return 0u;
    }

    return read_le_u32((const u8 *)GPT_ENTRY_SECTOR_ADDR + 40u);
}

static u32 linux_ext_revision(const BootInfo *boot_info) {
    if (!linux_ext_superblock_is_valid(boot_info)) {
        return 0u;
    }

    return read_le_u32((const u8 *)GPT_ENTRY_SECTOR_ADDR + 76u);
}

static u32 linux_ext_incompat_features(const BootInfo *boot_info) {
    if (!linux_ext_superblock_is_valid(boot_info)) {
        return 0u;
    }

    return read_le_u32((const u8 *)GPT_ENTRY_SECTOR_ADDR + 96u);
}

static u32 linux_ext_first_data_block(const BootInfo *boot_info) {
    if (!linux_ext_superblock_is_valid(boot_info)) {
        return 0u;
    }

    return read_le_u32((const u8 *)GPT_ENTRY_SECTOR_ADDR + 20u);
}

static u32 linux_ext_gdt_lba(const BootPlan *boot_plan, const BootInfo *boot_info) {
    u32 block_size;
    u32 gdt_block;

    if (!linux_ext_superblock_is_valid(boot_info)) {
        return 0u;
    }

    block_size = linux_ext_block_size(boot_info);
    if (block_size == 0u) {
        return 0u;
    }

    gdt_block = (block_size == 1024u) ? 2u : 1u;
    return boot_plan->candidate.first_lba + (gdt_block * (block_size / 512u));
}

static int linux_ext_gdt_probe_is_available(const BootInfo *boot_info) {
    if (!linux_ext_superblock_is_valid(boot_info)) {
        return 0;
    }

    return (boot_info->inspection_flags & BOOT_INFO_FLAG_GPT_ENTRY_VALID) != 0u;
}

static int linux_root_inode_probe_is_available(const BootInfo *boot_info) {
    return linux_ext_superblock_is_valid(boot_info) &&
           (boot_info->inspection_flags & BOOT_INFO_FLAG_GPT_HEADER_VALID) != 0u;
}

static u32 linux_ext_root_inode_offset(const BootInfo *boot_info) {
    u32 inode_size;

    inode_size = linux_ext_inode_size(boot_info);
    if (inode_size == 0u) {
        return 0u;
    }

    return inode_size % 512u;
}

static const u8 *linux_root_inode_bytes(const BootInfo *boot_info) {
    if (!linux_root_inode_probe_is_available(boot_info)) {
        return 0;
    }

    return (const u8 *)GPT_HEADER_ADDR + linux_ext_root_inode_offset(boot_info);
}

static u16 linux_root_inode_mode(const BootInfo *boot_info) {
    const u8 *inode;

    inode = linux_root_inode_bytes(boot_info);
    if (inode == 0) {
        return 0u;
    }

    return read_le_u16(inode + 0u);
}

static u32 linux_root_inode_size(const BootInfo *boot_info) {
    const u8 *inode;

    inode = linux_root_inode_bytes(boot_info);
    if (inode == 0) {
        return 0u;
    }

    return read_le_u32(inode + 4u);
}

static u32 linux_root_inode_flags(const BootInfo *boot_info) {
    const u8 *inode;

    inode = linux_root_inode_bytes(boot_info);
    if (inode == 0) {
        return 0u;
    }

    return read_le_u32(inode + 32u);
}

static int linux_root_inode_is_directory(const BootInfo *boot_info) {
    return (linux_root_inode_mode(boot_info) & 0xF000u) == EXT_MODE_DIRECTORY;
}

static int linux_root_inode_uses_extents(const BootInfo *boot_info) {
    return (linux_root_inode_flags(boot_info) & EXT4_INODE_FLAG_EXTENTS) != 0u;
}

static u32 linux_root_inode_first_data_word(const BootInfo *boot_info) {
    const u8 *inode;

    inode = linux_root_inode_bytes(boot_info);
    if (inode == 0) {
        return 0u;
    }

    return read_le_u32(inode + 40u);
}

static u32 linux_ext_block_to_lba(const BootPlan *boot_plan,
                                  const BootInfo *boot_info,
                                  u32 filesystem_block) {
    u32 sectors_per_block;
    u32 block_size;

    block_size = linux_ext_block_size(boot_info);
    if (block_size == 0u) {
        return 0u;
    }

    sectors_per_block = block_size / 512u;
    return boot_plan->candidate.first_lba + filesystem_block * sectors_per_block;
}

static int linux_root_extent_header_is_valid(const BootInfo *boot_info) {
    const u8 *inode;

    if (!linux_root_inode_uses_extents(boot_info)) {
        return 0;
    }

    inode = linux_root_inode_bytes(boot_info);
    if (inode == 0) {
        return 0;
    }

    return read_le_u16(inode + 40u) == 0xF30Au;
}

static u16 linux_root_extent_entries(const BootInfo *boot_info) {
    const u8 *inode;

    if (!linux_root_extent_header_is_valid(boot_info)) {
        return 0u;
    }

    inode = linux_root_inode_bytes(boot_info);
    return read_le_u16(inode + 42u);
}

static u16 linux_root_extent_depth(const BootInfo *boot_info) {
    const u8 *inode;

    if (!linux_root_extent_header_is_valid(boot_info)) {
        return 0u;
    }

    inode = linux_root_inode_bytes(boot_info);
    return read_le_u16(inode + 46u);
}

static u32 linux_root_extent_first_logical_block(const BootInfo *boot_info) {
    const u8 *inode;

    if (!linux_root_extent_header_is_valid(boot_info) ||
        linux_root_extent_entries(boot_info) == 0u) {
        return 0u;
    }

    inode = linux_root_inode_bytes(boot_info);
    return read_le_u32(inode + 52u);
}

static u32 linux_root_extent_first_length(const BootInfo *boot_info) {
    const u8 *inode;

    if (!linux_root_extent_header_is_valid(boot_info) ||
        linux_root_extent_entries(boot_info) == 0u) {
        return 0u;
    }

    inode = linux_root_inode_bytes(boot_info);
    return (u32)(read_le_u16(inode + 56u) & 0x7FFFu);
}

static u32 linux_root_extent_first_physical_block(const BootInfo *boot_info) {
    const u8 *inode;

    if (!linux_root_extent_header_is_valid(boot_info) ||
        linux_root_extent_entries(boot_info) == 0u ||
        linux_root_extent_depth(boot_info) != 0u) {
        return 0u;
    }

    inode = linux_root_inode_bytes(boot_info);
    return read_le_u32(inode + 60u);
}

static u32 linux_root_dir_target_lba(const BootPlan *boot_plan,
                                     const BootInfo *boot_info) {
    u32 filesystem_block;

    if (!linux_root_inode_probe_is_available(boot_info) ||
        !linux_root_inode_is_directory(boot_info)) {
        return 0u;
    }

    if (linux_root_extent_header_is_valid(boot_info)) {
        filesystem_block = linux_root_extent_first_physical_block(boot_info);
    } else {
        filesystem_block = linux_root_inode_first_data_word(boot_info);
    }

    if (filesystem_block == 0u) {
        return 0u;
    }

    return linux_ext_block_to_lba(boot_plan, boot_info, filesystem_block);
}

static int linux_root_dir_probe_is_available(const BootInfo *boot_info) {
    return linux_root_inode_probe_is_available(boot_info) &&
           (boot_info->inspection_flags & BOOT_INFO_FLAG_ROOT_DIR_VALID) != 0u;
}

static int linux_dir_name_matches(const u8 *entry, const char *name, u8 name_length) {
    u8 index;

    if (entry[6] != name_length) {
        return 0;
    }

    for (index = 0u; index < name_length; ++index) {
        if (entry[8u + index] != (u8)name[index]) {
            return 0;
        }
    }

    return 1;
}

static int linux_dir_sector_has_name(const u8 *sector,
                                     const char *name,
                                     u8 name_length) {
    u32 offset;
    offset = 0u;
    while (offset + 8u <= 512u) {
        const u8 *entry;
        u32 inode;
        u16 record_length;

        entry = sector + offset;
        inode = read_le_u32(entry + 0u);
        record_length = read_le_u16(entry + 4u);
        if (record_length < 8u || offset + record_length > 512u) {
            break;
        }

        if (inode != 0u && entry[6] == name_length &&
            linux_dir_name_matches(entry, name, name_length)) {
            return 1;
        }

        offset += record_length;
    }

    return 0;
}

static int linux_root_dir_has_name(const BootInfo *boot_info,
                                   const char *name,
                                   u8 name_length) {
    if (!linux_root_dir_probe_is_available(boot_info)) {
        return 0;
    }

    return linux_dir_sector_has_name((const u8 *)CHAINLOAD_BUFFER_ADDR, name, name_length);
}

static int linux_child_dir_probe_is_available(const BootInfo *boot_info) {
    return (boot_info->inspection_flags & BOOT_INFO_FLAG_CHILD_DIR_VALID) != 0u;
}

static int linux_grub_dir_probe_is_available(const BootInfo *boot_info) {
    return (boot_info->detail_flags & BOOT_INFO_DETAIL_GRUB_DIR_VALID) != 0u;
}

static int linux_grub_cfg_inode_probe_is_available(const BootInfo *boot_info) {
    return (boot_info->detail_flags & BOOT_INFO_DETAIL_GRUB_CFG_VALID) != 0u;
}

static int linux_child_dir_has_name(const BootInfo *boot_info,
                                    const char *name,
                                    u8 name_length) {
    if (!linux_child_dir_probe_is_available(boot_info)) {
        return 0;
    }

    return linux_dir_sector_has_name((const u8 *)CHILD_DIR_SECTOR_ADDR, name, name_length);
}

static int linux_grub_dir_has_name(const BootInfo *boot_info,
                                   const char *name,
                                   u8 name_length) {
    if (!linux_grub_dir_probe_is_available(boot_info)) {
        return 0;
    }

    return linux_dir_sector_has_name((const u8 *)GRANDCHILD_DIR_SECTOR_ADDR, name, name_length);
}

static int fat_short_name_matches(const u8 *entry, const char *name_11) {
    return bytes_match(entry, (const u8 *)name_11, 11u);
}

static const u8 *linux_fat_dir_entry_by_index(const u8 *sector, u32 wanted_index) {
    u32 offset;
    u32 current_index;

    offset = 0u;
    current_index = 0u;
    while (offset + 32u <= 512u) {
        const u8 *entry;
        u8 first_byte;
        u8 attributes;

        entry = sector + offset;
        first_byte = entry[0];
        attributes = entry[11];
        if (first_byte == 0x00u) {
            break;
        }

        if (first_byte != 0xE5u && attributes != 0x0Fu) {
            if (current_index == wanted_index) {
                return entry;
            }
            ++current_index;
        }

        offset += 32u;
    }

    return 0;
}

static const u8 *linux_fat_root_dir_entry_by_index(const BootInfo *boot_info, u32 wanted_index) {
    if (!linux_fat_root_dir_probe_is_available(boot_info)) {
        return 0;
    }

    return linux_fat_dir_entry_by_index((const u8 *)GPT_HEADER_ADDR, wanted_index);
}

static const u8 *linux_fat_loader_dir_entry_by_index(const BootInfo *boot_info, u32 wanted_index) {
    if (!linux_fat_loader_dir_probe_is_available(boot_info)) {
        return 0;
    }

    return linux_fat_dir_entry_by_index((const u8 *)CHILD_DIR_SECTOR_ADDR, wanted_index);
}

static const u8 *linux_fat_entries_dir_entry_by_index(const BootInfo *boot_info,
                                                      u32 wanted_index) {
    if (!linux_fat_entries_dir_probe_is_available(boot_info)) {
        return 0;
    }

    return linux_fat_dir_entry_by_index((const u8 *)GRANDCHILD_DIR_SECTOR_ADDR, wanted_index);
}

static int linux_fat_root_dir_has_short_name(const BootInfo *boot_info, const char *name_11) {
    u32 offset;
    const u8 *sector;

    if (!linux_fat_root_dir_probe_is_available(boot_info)) {
        return 0;
    }

    sector = (const u8 *)GPT_HEADER_ADDR;
    for (offset = 0u; offset + 32u <= 512u; offset += 32u) {
        const u8 *entry;
        u8 first_byte;
        u8 attributes;

        entry = sector + offset;
        first_byte = entry[0];
        attributes = entry[11];
        if (first_byte == 0x00u) {
            break;
        }

        if (first_byte == 0xE5u || attributes == 0x0Fu) {
            continue;
        }

        if (fat_short_name_matches(entry, name_11)) {
            return 1;
        }
    }

    return 0;
}

static int linux_fat_dir_sector_has_short_name(const u8 *sector, const char *name_11) {
    u32 offset;

    for (offset = 0u; offset + 32u <= 512u; offset += 32u) {
        const u8 *entry;
        u8 first_byte;
        u8 attributes;

        entry = sector + offset;
        first_byte = entry[0];
        attributes = entry[11];
        if (first_byte == 0x00u) {
            break;
        }

        if (first_byte == 0xE5u || attributes == 0x0Fu) {
            continue;
        }

        if (fat_short_name_matches(entry, name_11)) {
            return 1;
        }
    }

    return 0;
}

static int linux_fat_loader_dir_has_short_name(const BootInfo *boot_info, const char *name_11) {
    if (!linux_fat_loader_dir_probe_is_available(boot_info)) {
        return 0;
    }

    return linux_fat_dir_sector_has_short_name((const u8 *)CHILD_DIR_SECTOR_ADDR, name_11);
}

static int linux_fat_entries_dir_has_conf_like_name(const BootInfo *boot_info) {
    u32 offset;
    const u8 *sector;

    if (!linux_fat_entries_dir_probe_is_available(boot_info)) {
        return 0;
    }

    sector = (const u8 *)GRANDCHILD_DIR_SECTOR_ADDR;
    for (offset = 0u; offset + 32u <= 512u; offset += 32u) {
        const u8 *entry;
        u8 first_byte;
        u8 attributes;

        entry = sector + offset;
        first_byte = entry[0];
        attributes = entry[11];
        if (first_byte == 0x00u) {
            break;
        }

        if (first_byte == 0xE5u || attributes == 0x0Fu) {
            continue;
        }

        if (entry[8] == 'C' && entry[9] == 'O' && entry[10] == 'N') {
            return 1;
        }
    }

    return 0;
}

static const u8 *linux_fat_entry_file_value_bytes(const BootInfo *boot_info,
                                                  const char *keyword) {
    const u8 *sector;
    u32 offset;
    u32 keyword_length;

    if (!linux_fat_entry_file_probe_is_available(boot_info)) {
        return 0;
    }

    sector = (const u8 *)CONFIG_FILE_INODE_SECTOR_ADDR;
    keyword_length = c_string_length(keyword);
    if (keyword_length == 0u || keyword_length > 511u) {
        return 0;
    }

    for (offset = 0u; offset + keyword_length <= 512u; ++offset) {
        const u8 *value;

        if (offset != 0u && sector[offset - 1u] != '\n' && sector[offset - 1u] != '\r') {
            continue;
        }

        if (!bytes_match(sector + offset, (const u8 *)keyword, keyword_length)) {
            continue;
        }

        value = sector + offset + keyword_length;
        while (value < sector + 512u && (*value == ' ' || *value == '\t')) {
            ++value;
        }

        if (value >= sector + 512u || *value == '\n' || *value == '\r') {
            return 0;
        }

        return value;
    }

    return 0;
}

static int linux_fat_entry_file_has_title(const BootInfo *boot_info) {
    return linux_fat_entry_file_value_bytes(boot_info, "title ") != 0;
}

static int linux_fat_entry_file_has_linux(const BootInfo *boot_info) {
    return linux_fat_entry_file_value_bytes(boot_info, "linux ") != 0;
}

static int linux_fat_entry_file_has_initrd(const BootInfo *boot_info) {
    return linux_fat_entry_file_value_bytes(boot_info, "initrd ") != 0;
}

static int linux_fat_entry_file_has_options(const BootInfo *boot_info) {
    return linux_fat_entry_file_value_bytes(boot_info, "options ") != 0;
}

static u8 ascii_lowercase(u8 value) {
    if (value >= 'A' && value <= 'Z') {
        return (u8)(value + ('a' - 'A'));
    }

    return value;
}

static u32 path_value_component_length(const u8 *value) {
    u32 length;

    if (value == 0) {
        return 0u;
    }

    if (*value == '/') {
        ++value;
    }

    length = 0u;
    while (length < 96u) {
        u8 character;

        character = value[length];
        if (character == '\0' || character == '\n' || character == '\r' ||
            character == ' ' || character == '\t' || character == '/') {
            break;
        }

        ++length;
    }

    return length;
}

static int path_value_is_single_root_component(const u8 *value) {
    u32 length;
    u8 terminator;

    length = path_value_component_length(value);
    if (length == 0u) {
        return 0;
    }

    if (*value == '/') {
        ++value;
    }

    terminator = value[length];
    return terminator == '\0' || terminator == '\n' || terminator == '\r' ||
           terminator == ' ' || terminator == '\t';
}

static int ascii_path_component_matches(const char *name,
                                        const u8 *value,
                                        u32 component_length) {
    u32 index;

    if (value == 0 || component_length == 0u) {
        return 0;
    }

    if (*value == '/') {
        ++value;
    }

    for (index = 0u; index < component_length; ++index) {
        if (name[index] == '\0') {
            return 0;
        }

        if (ascii_lowercase((u8)name[index]) != ascii_lowercase(value[index])) {
            return 0;
        }
    }

    return name[component_length] == '\0';
}

static void fat_lfn_copy_name_part(char *name, u32 name_capacity, u32 name_index, u16 value) {
    if (name_index + 1u >= name_capacity) {
        return;
    }

    if (value == 0x0000u || value == 0xFFFFu) {
        return;
    }

    if ((value & 0xFF00u) != 0u) {
        name[name_index] = '?';
        return;
    }

    name[name_index] = (char)(value & 0x00FFu);
}

static void fat_build_long_name(const u8 *sector,
                                u32 entry_offset,
                                char *name,
                                u32 name_capacity) {
    u32 index;
    int saw_lfn;

    for (index = 0u; index < name_capacity; ++index) {
        name[index] = '\0';
    }

    saw_lfn = 0;
    while (entry_offset >= 32u) {
        const u8 *entry;
        u32 base_index;
        u8 sequence;

        entry_offset -= 32u;
        entry = sector + entry_offset;
        if (entry[11] != 0x0Fu) {
            break;
        }

        sequence = (u8)(entry[0] & 0x1Fu);
        if (sequence == 0u) {
            break;
        }

        base_index = (u32)(sequence - 1u) * 13u;
        fat_lfn_copy_name_part(name, name_capacity, base_index + 0u, read_le_u16(entry + 1u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 1u, read_le_u16(entry + 3u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 2u, read_le_u16(entry + 5u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 3u, read_le_u16(entry + 7u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 4u, read_le_u16(entry + 9u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 5u, read_le_u16(entry + 14u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 6u, read_le_u16(entry + 16u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 7u, read_le_u16(entry + 18u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 8u, read_le_u16(entry + 20u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 9u, read_le_u16(entry + 22u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 10u, read_le_u16(entry + 24u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 11u, read_le_u16(entry + 28u));
        fat_lfn_copy_name_part(name, name_capacity, base_index + 12u, read_le_u16(entry + 30u));
        saw_lfn = 1;

        if ((entry[0] & 0x40u) != 0u) {
            break;
        }
    }

    if (!saw_lfn) {
        name[0] = '\0';
    }
}

static const u8 *linux_fat_root_dir_entry_for_path_value(const BootInfo *boot_info,
                                                         const u8 *value) {
    const u8 *sector;
    u32 offset;
    u32 component_length;

    if (!linux_fat_root_dir_probe_is_available(boot_info) ||
        !path_value_is_single_root_component(value)) {
        return 0;
    }

    component_length = path_value_component_length(value);
    sector = (const u8 *)GPT_HEADER_ADDR;
    for (offset = 0u; offset + 32u <= 512u; offset += 32u) {
        const u8 *entry;
        char long_name[64];
        u8 first_byte;
        u8 attributes;

        entry = sector + offset;
        first_byte = entry[0];
        attributes = entry[11];
        if (first_byte == 0x00u) {
            break;
        }

        if (first_byte == 0xE5u || attributes == 0x0Fu) {
            continue;
        }

        fat_build_long_name(sector, offset, long_name, sizeof(long_name));
        if (long_name[0] != '\0' &&
            ascii_path_component_matches(long_name, value, component_length)) {
            return entry;
        }
    }

    return 0;
}

static const u8 *linux_fat_entry_linux_root_entry(const BootInfo *boot_info) {
    return linux_fat_root_dir_entry_for_path_value(
        boot_info, linux_fat_entry_file_value_bytes(boot_info, "linux "));
}

static const u8 *linux_fat_entry_initrd_root_entry(const BootInfo *boot_info) {
    return linux_fat_root_dir_entry_for_path_value(
        boot_info, linux_fat_entry_file_value_bytes(boot_info, "initrd "));
}

static u32 linux_fat_dir_entry_first_cluster(const u8 *entry) {
    if (entry == 0) {
        return 0u;
    }

    return ((u32)read_le_u16(entry + 20u) << 16) | (u32)read_le_u16(entry + 26u);
}

static u32 linux_fat_entry_file_lba(const BootPlan *boot_plan,
                                    const BootInfo *boot_info,
                                    const u8 *entry) {
    return linux_fat_cluster_lba(
        boot_plan, boot_info, linux_fat_dir_entry_first_cluster(entry));
}

static const u8 *linux_fat_kernel_sector_bytes(const BootInfo *boot_info) {
    if (!linux_fat_kernel_probe_is_available(boot_info)) {
        return 0;
    }

    return (const u8 *)LINUX_KERNEL_SECTOR_ADDR;
}

static const u8 *linux_fat_initrd_sector_bytes(const BootInfo *boot_info) {
    if (!linux_fat_initrd_probe_is_available(boot_info)) {
        return 0;
    }

    return (const u8 *)LINUX_INITRD_SECTOR_ADDR;
}

static int linux_fat_kernel_has_hdrs(const BootInfo *boot_info) {
    const u8 *sector;

    sector = linux_fat_kernel_sector_bytes(boot_info);
    if (sector == 0) {
        return 0;
    }

    return bytes_match(sector + 0x202u, (const u8 *)"HdrS", 4u);
}

static const char *linux_fat_kernel_header_label(const BootInfo *boot_info) {
    const u8 *sector;

    sector = linux_fat_kernel_sector_bytes(boot_info);
    if (sector == 0) {
        return "(n/a)";
    }

    if (linux_fat_kernel_has_hdrs(boot_info)) {
        return "HdrS";
    }

    if (read_le_u16(sector + 510u) == 0xAA55u &&
        (sector[0] == 0xEBu || sector[0] == 0xE9u)) {
        return "boot";
    }

    if (bytes_match(sector, (const u8 *)"MZ", 2u)) {
        return "MZ";
    }

    return "data";
}

static const char *linux_fat_initrd_header_label(const BootInfo *boot_info) {
    const u8 *sector;

    sector = linux_fat_initrd_sector_bytes(boot_info);
    if (sector == 0) {
        return "(n/a)";
    }

    if (sector[0] == 0x28u && sector[1] == 0xB5u &&
        sector[2] == 0x2Fu && sector[3] == 0xFDu) {
        return "zstd";
    }

    if (sector[0] == 0x1Fu && sector[1] == 0x8Bu) {
        return "gzip";
    }

    if (bytes_match(sector, (const u8 *)"070701", 6u) ||
        bytes_match(sector, (const u8 *)"070702", 6u)) {
        return "cpio";
    }

    if (sector[0] == 0xFDu && sector[1] == 0x37u &&
        sector[2] == 0x7Au && sector[3] == 0x58u &&
        sector[4] == 0x5Au && sector[5] == 0x00u) {
        return "xz";
    }

    if (sector[0] == 0x04u && sector[1] == 0x22u &&
        sector[2] == 0x4Du && sector[3] == 0x18u) {
        return "lz4";
    }

    return "data";
}

static int fat_efi_sector_has_pe_header(const u8 *sector) {
    u32 pe_offset;

    if (sector == 0 || !bytes_match(sector, (const u8 *)"MZ", 2u)) {
        return 0;
    }

    pe_offset = read_le_u32(sector + 60u);
    if (pe_offset > 508u) {
        return 0;
    }

    return bytes_match(sector + pe_offset, (const u8 *)"PE\0\0", 4u);
}

static const char *fat_efi_sector_label(const u8 *sector) {
    u32 pe_offset;

    if (sector == 0) {
        return "(n/a)";
    }

    if (!bytes_match(sector, (const u8 *)"MZ", 2u)) {
        return "data";
    }

    pe_offset = read_le_u32(sector + 60u);
    if (pe_offset > 486u || !bytes_match(sector + pe_offset, (const u8 *)"PE\0\0", 4u)) {
        return "MZ";
    }

    if (read_le_u16(sector + pe_offset + 4u) == 0x8664u &&
        read_le_u16(sector + pe_offset + 24u) == 0x020Bu) {
        return "PE32+";
    }

    return "PE";
}

static const u8 *linux_fat_efi_boot_file_sector_bytes(const BootInfo *boot_info) {
    if (!linux_fat_efi_boot_file_probe_is_available(boot_info)) {
        return 0;
    }

    return (const u8 *)FAT_EFI_BOOT_FILE_SECTOR_ADDR;
}

static const u8 *linux_fat_efi_systemd_file_sector_bytes(const BootInfo *boot_info) {
    if (!linux_fat_efi_systemd_file_probe_is_available(boot_info)) {
        return 0;
    }

    return (const u8 *)FAT_EFI_SYSTEMD_FILE_SECTOR_ADDR;
}

static const char *linux_fat_efi_boot_file_label(const BootInfo *boot_info) {
    return fat_efi_sector_label(linux_fat_efi_boot_file_sector_bytes(boot_info));
}

static const char *linux_fat_efi_systemd_file_label(const BootInfo *boot_info) {
    return fat_efi_sector_label(linux_fat_efi_systemd_file_sector_bytes(boot_info));
}

static int linux_fat_entry_linux_matches_root(const BootInfo *boot_info) {
    return linux_fat_entry_linux_root_entry(boot_info) != 0;
}

static int linux_fat_entry_initrd_matches_root(const BootInfo *boot_info) {
    return linux_fat_entry_initrd_root_entry(boot_info) != 0;
}

static const u8 *linux_grub_cfg_inode_bytes(const BootInfo *boot_info) {
    if (!linux_grub_cfg_inode_probe_is_available(boot_info)) {
        return 0;
    }

    return (const u8 *)CONFIG_FILE_INODE_SECTOR_ADDR;
}

static u16 linux_grub_cfg_inode_mode(const BootInfo *boot_info) {
    const u8 *inode;

    inode = linux_grub_cfg_inode_bytes(boot_info);
    if (inode == 0) {
        return 0u;
    }

    return read_le_u16(inode + 0u);
}

static u32 linux_grub_cfg_inode_size(const BootInfo *boot_info) {
    const u8 *inode;

    inode = linux_grub_cfg_inode_bytes(boot_info);
    if (inode == 0) {
        return 0u;
    }

    return read_le_u32(inode + 4u);
}

static u32 linux_grub_cfg_inode_flags(const BootInfo *boot_info) {
    const u8 *inode;

    inode = linux_grub_cfg_inode_bytes(boot_info);
    if (inode == 0) {
        return 0u;
    }

    return read_le_u32(inode + 32u);
}

static int linux_grub_cfg_inode_is_regular(const BootInfo *boot_info) {
    return (linux_grub_cfg_inode_mode(boot_info) & 0xF000u) == EXT_MODE_REGULAR;
}

static int linux_grub_cfg_inode_uses_extents(const BootInfo *boot_info) {
    return (linux_grub_cfg_inode_flags(boot_info) & EXT4_INODE_FLAG_EXTENTS) != 0u;
}

static u32 linux_grub_cfg_inode_first_data_word(const BootInfo *boot_info) {
    const u8 *inode;

    inode = linux_grub_cfg_inode_bytes(boot_info);
    if (inode == 0) {
        return 0u;
    }

    return read_le_u32(inode + 40u);
}

static int linux_grub_cfg_extent_header_is_valid(const BootInfo *boot_info) {
    const u8 *inode;

    if (!linux_grub_cfg_inode_uses_extents(boot_info)) {
        return 0;
    }

    inode = linux_grub_cfg_inode_bytes(boot_info);
    if (inode == 0) {
        return 0;
    }

    return read_le_u16(inode + 40u) == 0xF30Au;
}

static u16 linux_grub_cfg_extent_entries(const BootInfo *boot_info) {
    const u8 *inode;

    if (!linux_grub_cfg_extent_header_is_valid(boot_info)) {
        return 0u;
    }

    inode = linux_grub_cfg_inode_bytes(boot_info);
    return read_le_u16(inode + 42u);
}

static u16 linux_grub_cfg_extent_depth(const BootInfo *boot_info) {
    const u8 *inode;

    if (!linux_grub_cfg_extent_header_is_valid(boot_info)) {
        return 0u;
    }

    inode = linux_grub_cfg_inode_bytes(boot_info);
    return read_le_u16(inode + 46u);
}

static u32 linux_grub_cfg_extent_first_physical_block(const BootInfo *boot_info) {
    const u8 *inode;

    if (!linux_grub_cfg_extent_header_is_valid(boot_info) ||
        linux_grub_cfg_extent_entries(boot_info) == 0u ||
        linux_grub_cfg_extent_depth(boot_info) != 0u) {
        return 0u;
    }

    inode = linux_grub_cfg_inode_bytes(boot_info);
    return read_le_u32(inode + 60u);
}

static const u8 *linux_dir_entry_by_index(const u8 *sector, u32 wanted_index) {
    u32 offset;
    u32 current_index;
    offset = 0u;
    current_index = 0u;
    while (offset + 8u <= 512u) {
        const u8 *entry;
        u32 inode;
        u16 record_length;
        u8 name_length;

        entry = sector + offset;
        inode = read_le_u32(entry + 0u);
        record_length = read_le_u16(entry + 4u);
        name_length = entry[6];
        if (record_length < 8u || offset + record_length > 512u) {
            break;
        }

        if (inode != 0u && name_length != 0u) {
            if (current_index == wanted_index) {
                return entry;
            }
            ++current_index;
        }

        offset += record_length;
    }

    return 0;
}

static const u8 *linux_root_dir_entry_by_index(const BootInfo *boot_info, u32 wanted_index) {
    if (!linux_root_dir_probe_is_available(boot_info)) {
        return 0;
    }

    return linux_dir_entry_by_index((const u8 *)CHAINLOAD_BUFFER_ADDR, wanted_index);
}

static const u8 *linux_child_dir_entry_by_index(const BootInfo *boot_info, u32 wanted_index) {
    if (!linux_child_dir_probe_is_available(boot_info)) {
        return 0;
    }

    return linux_dir_entry_by_index((const u8 *)CHILD_DIR_SECTOR_ADDR, wanted_index);
}

static const u8 *linux_grub_dir_entry_by_index(const BootInfo *boot_info, u32 wanted_index) {
    if (!linux_grub_dir_probe_is_available(boot_info)) {
        return 0;
    }

    return linux_dir_entry_by_index((const u8 *)GRANDCHILD_DIR_SECTOR_ADDR, wanted_index);
}

static void write_dir_entry_name(u32 row, u32 column, u8 color, const u8 *entry) {
    u8 name_length;
    u8 index;

    if (entry == 0) {
        write_string(row, column, 0x4F, "(none)");
        return;
    }

    name_length = entry[6];
    if (name_length > 6u) {
        name_length = 6u;
    }

    for (index = 0u; index < name_length; ++index) {
        VGA_TEXT_BUFFER[row * VGA_COLUMNS + column + index] =
            make_vga_cell((char)entry[8u + index], color);
    }
}

static void write_fat_dir_entry_name(u32 row, u32 column, u8 color, const u8 *entry) {
    u32 index;
    u32 cursor;
    int has_extension;

    if (entry == 0) {
        write_string(row, column, 0x4F, "(none)");
        return;
    }

    cursor = column;
    for (index = 0u; index < 8u; ++index) {
        if (entry[index] == ' ') {
            break;
        }

        VGA_TEXT_BUFFER[row * VGA_COLUMNS + cursor] =
            make_vga_cell((char)entry[index], color);
        ++cursor;
    }

    has_extension = 0;
    for (index = 8u; index < 11u; ++index) {
        if (entry[index] != ' ') {
            has_extension = 1;
            break;
        }
    }

    if (has_extension && cursor + 4u < VGA_COLUMNS) {
        VGA_TEXT_BUFFER[row * VGA_COLUMNS + cursor] = make_vga_cell('.', color);
        ++cursor;
        for (index = 8u; index < 11u; ++index) {
            if (entry[index] == ' ') {
                break;
            }

            VGA_TEXT_BUFFER[row * VGA_COLUMNS + cursor] =
                make_vga_cell((char)entry[index], color);
            ++cursor;
        }
    }
}

static void write_text_value_snippet(u32 row,
                                     u32 column,
                                     u8 color,
                                     const u8 *text,
                                     u32 max_chars) {
    u32 index;

    if (text == 0) {
        write_string(row, column, 0x4F, "(none)");
        return;
    }

    for (index = 0u; index < max_chars; ++index) {
        u8 character;

        character = text[index];
        if (character == '\0' || character == '\n' || character == '\r') {
            break;
        }

        if (character < 0x20u || character > 0x7Eu) {
            break;
        }

        VGA_TEXT_BUFFER[row * VGA_COLUMNS + column + index] =
            make_vga_cell((char)character, color);
    }
}

static const char *linux_root_dir_hint(const BootInfo *boot_info) {
    if (!linux_root_dir_probe_is_available(boot_info)) {
        return "dir sector not read";
    }

    if (linux_root_dir_has_name(boot_info, "boot", 4u)) {
        return "/boot seen in root";
    }

    if (linux_root_dir_has_name(boot_info, "EFI", 3u)) {
        return "EFI dir seen in root";
    }

    if (linux_root_dir_has_name(boot_info, "grub", 4u)) {
        return "grub dir seen in root";
    }

    if (linux_root_dir_has_name(boot_info, "loader", 6u)) {
        return "loader dir seen in root";
    }

    return "no boot-ish names in first dir sector";
}

static const char *linux_fat_root_hint(const BootInfo *boot_info) {
    if (!linux_fat_root_dir_probe_is_available(boot_info)) {
        return "FAT root dir not read";
    }

    if (linux_fat_root_dir_has_short_name(boot_info, "EFI        ") &&
        linux_fat_root_dir_has_short_name(boot_info, "LOADER     ")) {
        return "systemd-boot layout seen";
    }

    if (linux_fat_root_dir_has_short_name(boot_info, "LOADER     ")) {
        return "loader dir seen in FAT root";
    }

    if (linux_fat_root_dir_has_short_name(boot_info, "EFI        ")) {
        return "EFI dir seen in FAT root";
    }

    return "FAT root read, scan more entries";
}

static const char *linux_fat_loader_hint(const BootInfo *boot_info) {
    if (!linux_fat_loader_dir_probe_is_available(boot_info)) {
        return "/loader dir not read";
    }

    if (linux_fat_loader_dir_has_short_name(boot_info, "ENTRIES    ")) {
        return "entries dir seen in /loader";
    }

    if (linux_fat_loader_dir_has_short_name(boot_info, "KEYS       ")) {
        return "keys dir seen in /loader";
    }

    return "/loader read, scan more names";
}

static const char *linux_fat_entries_hint(const BootInfo *boot_info) {
    if (!linux_fat_entries_dir_probe_is_available(boot_info)) {
        return "/loader/entries not read";
    }

    if (linux_fat_entries_dir_has_conf_like_name(boot_info)) {
        return "entry file alias seen in /loader/entries";
    }

    return "entries dir read, scan more aliases";
}

static const char *linux_fat_efi_hint(const BootInfo *boot_info) {
    if (linux_fat_efi_boot_file_probe_is_available(boot_info) &&
        linux_fat_efi_systemd_file_probe_is_available(boot_info)) {
        if (fat_efi_sector_has_pe_header(linux_fat_efi_boot_file_sector_bytes(boot_info)) &&
            fat_efi_sector_has_pe_header(linux_fat_efi_systemd_file_sector_bytes(boot_info))) {
            return "BOOTX64.EFI and systemd-boot are readable";
        }
        return "EFI manager sectors were read";
    }

    if (linux_fat_efi_boot_file_probe_is_available(boot_info)) {
        return "BOOTX64.EFI sector was read";
    }

    if (linux_fat_efi_systemd_file_probe_is_available(boot_info)) {
        return "systemd-boot EFI sector was read";
    }

    if (linux_fat_efi_boot_dir_probe_is_available(boot_info) &&
        linux_fat_efi_systemd_dir_probe_is_available(boot_info)) {
        return "EFI/BOOT and EFI/systemd dirs were read";
    }

    if (linux_fat_efi_dir_probe_is_available(boot_info)) {
        return "EFI dir read from FAT root";
    }

    return "EFI dir not read";
}

static const char *linux_fat_entry_file_hint(const BootInfo *boot_info) {
    if (!linux_fat_entry_file_probe_is_available(boot_info)) {
        return "entry file sector not read";
    }

    if (linux_fat_kernel_probe_is_available(boot_info) &&
        linux_fat_initrd_probe_is_available(boot_info)) {
        return "kernel and initrd sectors were read";
    }

    if (linux_fat_kernel_probe_is_available(boot_info)) {
        return "kernel first sectors were read";
    }

    if (linux_fat_initrd_probe_is_available(boot_info)) {
        return "initrd first sector was read";
    }

    if (linux_fat_entry_linux_matches_root(boot_info) &&
        linux_fat_entry_initrd_matches_root(boot_info)) {
        return "kernel and initrd paths match FAT root";
    }

    if (linux_fat_entry_linux_matches_root(boot_info)) {
        return "kernel path matches FAT root";
    }

    if (linux_fat_entry_file_has_title(boot_info) &&
        linux_fat_entry_file_has_linux(boot_info) &&
        linux_fat_entry_file_has_initrd(boot_info)) {
        return "entry file has title/linux/initrd";
    }

    if (linux_fat_entry_file_has_linux(boot_info) &&
        linux_fat_entry_file_has_options(boot_info)) {
        return "entry file has linux/options";
    }

    if (linux_fat_entry_file_has_title(boot_info)) {
        return "entry file text starts making sense";
    }

    return "entry file loaded, scan more text";
}

static const char *linux_child_dir_hint(const BootInfo *boot_info) {
    if (!linux_child_dir_probe_is_available(boot_info)) {
        return "child dir not read";
    }

    if (linux_child_dir_has_name(boot_info, "vmlinuz", 7u)) {
        return "kernel-like file seen";
    }

    if (linux_child_dir_has_name(boot_info, "initramf", 8u)) {
        return "initramfs-like file seen";
    }

    if (linux_child_dir_has_name(boot_info, "grub", 4u)) {
        return "grub seen under /boot";
    }

    if (linux_child_dir_has_name(boot_info, "loader", 6u)) {
        return "loader seen under /boot";
    }

    return "no familiar boot names yet";
}

static const char *linux_grub_dir_hint(const BootInfo *boot_info) {
    if (!linux_grub_dir_probe_is_available(boot_info)) {
        return "grub dir not read";
    }

    if (linux_grub_dir_has_name(boot_info, "grub.c", 6u)) {
        return "grub.cfg-like name seen";
    }

    if (linux_grub_dir_has_name(boot_info, "fonts", 5u)) {
        return "fonts dir seen under grub";
    }

    if (linux_grub_dir_has_name(boot_info, "x86_64", 6u)) {
        return "platform dir seen under grub";
    }

    return "grub dir read, names still unfamiliar";
}

static const char *linux_grub_cfg_hint(const BootInfo *boot_info) {
    if (!linux_grub_cfg_inode_probe_is_available(boot_info)) {
        return "grub.cfg inode not read";
    }

    if (!linux_grub_cfg_inode_is_regular(boot_info)) {
        return "grub.cfg entry is not a regular file";
    }

    if (linux_grub_cfg_extent_header_is_valid(boot_info)) {
        return "grub.cfg inode is ready via extents";
    }

    if (linux_grub_cfg_inode_uses_extents(boot_info)) {
        return "grub.cfg needs deeper extent walk";
    }

    if (linux_grub_cfg_inode_first_data_word(boot_info) != 0u) {
        return "grub.cfg direct data block is mapped";
    }

    return "grub.cfg inode is loaded";
}

static const char *linux_ext_support_label(const BootInfo *boot_info) {
    u32 incompat_features;

    incompat_features = linux_ext_incompat_features(boot_info);
    if (incompat_features == 0u || incompat_features == EXT_INCOMPAT_FILETYPE) {
        return "simple ext2/3 path";
    }

    if ((incompat_features & EXT_INCOMPAT_EXTENTS) != 0u) {
        return "needs extents support";
    }

    if ((incompat_features & EXT_INCOMPAT_64BIT) != 0u) {
        return "needs 64-bit ext support";
    }

    if ((incompat_features & EXT_INCOMPAT_INLINE_DATA) != 0u) {
        return "needs inline-data support";
    }

    if ((incompat_features & EXT_INCOMPAT_ENCRYPT) != 0u) {
        return "encrypted fs unsupported";
    }

    if ((incompat_features & EXT_INCOMPAT_CASEFOLD) != 0u) {
        return "needs casefold support";
    }

    if ((incompat_features & EXT_INCOMPAT_META_BG) != 0u) {
        return "needs meta_bg support";
    }

    if ((incompat_features & EXT_INCOMPAT_FLEX_BG) != 0u) {
        return "needs flex_bg support";
    }

    return "needs more ext feature checks";
}

static const char *linux_ext_next_action(const BootInfo *boot_info,
                                         int root_inode_available) {
    u32 incompat_features;

    incompat_features = linux_ext_incompat_features(boot_info);
    if ((incompat_features & EXT_INCOMPAT_EXTENTS) != 0u) {
        return "add extents inode reader";
    }

    if ((incompat_features & EXT_INCOMPAT_64BIT) != 0u) {
        return "add 64-bit ext support";
    }

    if ((incompat_features & EXT_INCOMPAT_INLINE_DATA) != 0u) {
        return "add inline-data support";
    }

    if ((incompat_features & EXT_INCOMPAT_ENCRYPT) != 0u) {
        return "encrypted Linux path blocked";
    }

    if ((incompat_features & EXT_INCOMPAT_CASEFOLD) != 0u) {
        return "add casefold support";
    }

    if ((incompat_features & EXT_INCOMPAT_META_BG) != 0u) {
        return "add meta_bg support";
    }

    if ((incompat_features & EXT_INCOMPAT_FLEX_BG) != 0u) {
        return "add flex_bg support";
    }

    if (root_inode_available) {
        if (linux_grub_cfg_inode_probe_is_available(boot_info)) {
            if (!linux_grub_cfg_inode_is_regular(boot_info)) {
                return "inspect grub.cfg inode type";
            }
            if (linux_grub_cfg_extent_header_is_valid(boot_info)) {
                return "read grub.cfg extent target";
            }
            if (linux_grub_cfg_inode_uses_extents(boot_info)) {
                return "walk grub.cfg extent tree";
            }
            return "read grub.cfg data block";
        }
        if (linux_grub_dir_probe_is_available(boot_info)) {
            if (linux_grub_dir_has_name(boot_info, "grub.cfg", 8u)) {
                return "read grub.cfg inode";
            }
            if (linux_grub_dir_has_name(boot_info, "fonts", 5u)) {
                return "inspect /boot/grub/fonts";
            }
            if (linux_grub_dir_has_name(boot_info, "x86_64", 6u)) {
                return "inspect grub platform dir";
            }
            return "scan more /boot/grub entries";
        }
        if (linux_child_dir_probe_is_available(boot_info)) {
            if (linux_child_dir_has_name(boot_info, "grub", 4u)) {
                return "inspect /boot/grub";
            }
            if (linux_child_dir_has_name(boot_info, "loader", 6u)) {
                return "inspect /boot/loader";
            }
            if (linux_child_dir_has_name(boot_info, "vmlinuz", 7u)) {
                return "inspect kernel file";
            }
            return "scan more /boot entries";
        }
        if (linux_root_dir_probe_is_available(boot_info)) {
            if (linux_root_dir_has_name(boot_info, "boot", 4u)) {
                return "read /boot directory";
            }
            if (linux_root_dir_has_name(boot_info, "EFI", 3u)) {
                return "inspect EFI directory";
            }
            if (linux_root_dir_has_name(boot_info, "grub", 4u)) {
                return "inspect grub directory";
            }
            return "scan more root dir entries";
        }
        if (linux_root_extent_header_is_valid(boot_info) &&
            linux_root_extent_depth(boot_info) == 0u) {
            return "read root dir extent target";
        }
        if (linux_root_inode_uses_extents(boot_info)) {
            return "walk root dir extent tree";
        }
        if (linux_root_inode_is_directory(boot_info)) {
            return "read root dir block";
        }
        return "inspect root inode type";
    }

    if (linux_ext_gdt_probe_is_available(boot_info)) {
        return "read root inode";
    }

    return "read ext group descriptors";
}

static const char *linux_fat_next_action(const BootInfo *boot_info) {
    if (linux_fat_efi_boot_file_probe_is_available(boot_info) &&
        linux_fat_efi_systemd_file_probe_is_available(boot_info)) {
        return "inspect Windows EFI path";
    }

    if (linux_fat_efi_boot_dir_probe_is_available(boot_info) ||
        linux_fat_efi_systemd_dir_probe_is_available(boot_info)) {
        return "read EFI manager sectors";
    }

    if (linux_fat_kernel_probe_is_available(boot_info) &&
        linux_fat_initrd_probe_is_available(boot_info)) {
        return "inspect EFI boot manager";
    }

    if (linux_fat_entry_file_probe_is_available(boot_info)) {
        if (linux_fat_entry_linux_matches_root(boot_info) &&
            linux_fat_entry_initrd_matches_root(boot_info)) {
            return "read kernel and initrd sectors";
        }
        if (linux_fat_entry_linux_matches_root(boot_info)) {
            return "read kernel first sectors";
        }
        if (linux_fat_entry_file_has_linux(boot_info) &&
            linux_fat_entry_file_has_initrd(boot_info)) {
            return "match entry paths to FAT names";
        }
        if (linux_fat_entry_file_has_title(boot_info)) {
            return "parse remaining entry lines";
        }
        return "scan entry file text";
    }

    if (linux_fat_entries_dir_probe_is_available(boot_info)) {
        if (linux_fat_entries_dir_has_conf_like_name(boot_info)) {
            return "inspect boot entry file";
        }
        return "scan more entry aliases";
    }

    if (linux_fat_loader_dir_probe_is_available(boot_info)) {
        if (linux_fat_loader_dir_has_short_name(boot_info, "ENTRIES    ")) {
            return "inspect /loader/entries";
        }
        if (linux_fat_loader_dir_has_short_name(boot_info, "KEYS       ")) {
            return "inspect /loader/keys";
        }
        return "scan more /loader names";
    }

    if (!linux_fat_root_dir_probe_is_available(boot_info)) {
        return "read FAT root directory";
    }

    if (linux_fat_root_dir_has_short_name(boot_info, "LOADER     ")) {
        return "inspect /loader entries";
    }

    if (linux_fat_root_dir_has_short_name(boot_info, "EFI        ")) {
        return "inspect EFI boot files";
    }

    return "scan more FAT root entries";
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

static const char *mbr_partition_type_label(u8 partition_type) {
    switch (partition_type)
    {
    case 0x00u:
        return "Unused";
    case 0x07u:
        return "NTFS/exFAT";
    case 0x83u:
        return "Linux";
    case 0x82u:
        return "Linux swap";
    case 0x0Bu:
    case 0x0Cu:
        return "FAT32";
    case 0xEEu:
        return "GPT protective";
    default:
        return "Other";
    }
}

static int gpt_header_is_valid(const BootInfo *boot_info, const u8 *gpt_header) {
    return (boot_info->inspection_flags & BOOT_INFO_FLAG_GPT_HEADER_VALID) != 0u &&
           gpt_header[0] == 'E' && gpt_header[1] == 'F' &&
           gpt_header[2] == 'I' && gpt_header[3] == ' ' &&
           gpt_header[4] == 'P' && gpt_header[5] == 'A' &&
           gpt_header[6] == 'R' && gpt_header[7] == 'T';
}

static const char *gpt_partition_type_label(const u8 *type_guid) {
    if (bytes_are_zero(type_guid, 16u)) {
        return "Unused";
    }

    if (bytes_match(type_guid, EFI_SYSTEM_GUID, 16u)) {
        return "EFI System";
    }

    if (bytes_match(type_guid, MICROSOFT_BASIC_GUID, 16u)) {
        return "Microsoft Data";
    }

    if (bytes_match(type_guid, LINUX_FILESYSTEM_GUID, 16u)) {
        return "Linux FS";
    }

    if (bytes_match(type_guid, LINUX_SWAP_GUID, 16u)) {
        return "Linux swap";
    }

    if (bytes_match(type_guid, MICROSOFT_RESERVED_GUID, 16u)) {
        return "MS Reserved";
    }

    if (bytes_match(type_guid, WINDOWS_RECOVERY_GUID, 16u)) {
        return "Win Recovery";
    }

    return "Other GPT";
}

static void write_hex_u32(u32 row, u32 column, u8 color, u32 value) {
    static const char hex_digits[] = "0123456789ABCDEF";
    u32 shift;

    for (shift = 0u; shift < 8u; ++shift) {
        u32 nibble_shift = (7u - shift) * 4u;
        u8 digit = (u8)((value >> nibble_shift) & 0x0Fu);
        VGA_TEXT_BUFFER[row * VGA_COLUMNS + column + shift] =
            make_vga_cell(hex_digits[digit], color);
    }
}

static void set_candidate(TargetCandidate *candidate,
                          int priority,
                          const char *origin,
                          const char *kind,
                          u32 partition_number,
                          u32 first_lba) {
    if (candidate->found && priority <= candidate->priority) {
        return;
    }

    candidate->origin = origin;
    candidate->kind = kind;
    candidate->partition_number = partition_number;
    candidate->first_lba = first_lba;
    candidate->found = 1;
    candidate->priority = priority;
}

static void write_candidate_line(u32 row,
                                 const char *label,
                                 const TargetCandidate *candidate) {
    write_string(row, 4u, 0x1F, label);

    if (!candidate->found) {
        write_string(row, 18u, 0x4F, "not found yet");
        return;
    }

    write_string(row, 18u, 0x1E, candidate->origin);
    write_string(row, 22u, 0x1F, " part ");
    VGA_TEXT_BUFFER[row * VGA_COLUMNS + 28u] =
        make_vga_cell((char)('1' + candidate->partition_number), 0x1E);
    write_string(row, 30u, 0x1E, candidate->kind);
    write_string(row, 48u, 0x1F, "LBA 0x");
    write_hex_u32(row, 54u, 0x1E, candidate->first_lba);
}

static void make_boot_plan(u8 requested_target,
                           const TargetCandidate *linux_candidate,
                           const TargetCandidate *windows_candidate,
                           BootPlan *boot_plan) {
    const TargetCandidate *requested_candidate;

    boot_plan->request_label = selected_target_label(requested_target);
    boot_plan->status_label = "missing target";
    boot_plan->action_label = "inspect more sectors";
    boot_plan->support_label = "not bootable yet";
    boot_plan->candidate.origin = "";
    boot_plan->candidate.kind = "";
    boot_plan->candidate.first_lba = 0u;
    boot_plan->candidate.partition_number = 0u;
    boot_plan->candidate.found = 0;
    boot_plan->candidate.priority = 0;
    boot_plan->ready = 0;

    if (requested_target == BOOT_TARGET_LINUX) {
        requested_candidate = linux_candidate;
    } else if (requested_target == BOOT_TARGET_WINDOWS) {
        requested_candidate = windows_candidate;
    } else {
        requested_candidate = 0;
    }

    if (requested_candidate == 0 || !requested_candidate->found) {
        return;
    }

    boot_plan->candidate = *requested_candidate;
    boot_plan->ready = 1;
    boot_plan->status_label = "candidate chosen";

    if (bytes_match((const u8 *)requested_candidate->origin, (const u8 *)"MBR", 3u)) {
        /*
         * If Stage 2 is running, Stage 1 already had its chance to chainload.
         * An MBR target here means the BIOS path exists, but the handoff did
         * not complete and we fell back to the inspector.
         */
        boot_plan->status_label = "fallback after stage 1";
        boot_plan->action_label = "inspect target boot sector";
        boot_plan->support_label = "BIOS path exists";
        return;
    }

    if (requested_target == BOOT_TARGET_WINDOWS &&
        bytes_match((const u8 *)requested_candidate->kind,
                    (const u8 *)"EFI System", 10u)) {
        boot_plan->action_label = "load EFI boot manager";
        boot_plan->support_label = "needs UEFI branch";
    } else if (requested_target == BOOT_TARGET_WINDOWS) {
        boot_plan->action_label = "find EFI System then boot Windows";
        boot_plan->support_label = "needs UEFI branch";
    } else if (bytes_match((const u8 *)requested_candidate->kind,
                           (const u8 *)"Linux FS", 8u)) {
        boot_plan->action_label = "find Linux boot file";
        boot_plan->support_label = "needs filesystem code";
    } else if (bytes_match((const u8 *)requested_candidate->kind,
                           (const u8 *)"Linux swap", 10u)) {
        boot_plan->action_label = "look for better Linux partition";
        boot_plan->support_label = "not a boot target";
    } else {
        boot_plan->action_label = "find Linux boot file";
        boot_plan->support_label = "needs extra disk logic";
    }
}

static void write_boot_plan_line(u32 row, const BootPlan *boot_plan) {
    write_string(row, 4u, 0x1F, "Boot plan:");
    write_string(row, 16u, 0x1E, boot_plan->request_label);
    write_string(row, 23u, 0x1F, "->");
    write_string(row, 26u, boot_plan->ready ? 0x1E : 0x4F, boot_plan->status_label);
}

static void write_boot_action_line(u32 row, const BootPlan *boot_plan) {
    write_string(row, 4u, 0x1F, "Next action:");
    write_string(row, 18u, boot_plan->ready ? 0x1E : 0x4F, boot_plan->action_label);
}

static void write_boot_support_line(u32 row, const BootPlan *boot_plan) {
    write_string(row, 4u, 0x1F, "This branch:");
    write_string(row, 18u, boot_plan->ready ? 0x1E : 0x4F, boot_plan->support_label);
}

static int boot_plan_uses_mbr_path(const BootPlan *boot_plan) {
    return boot_plan->ready &&
           bytes_match((const u8 *)boot_plan->candidate.origin, (const u8 *)"MBR", 3u);
}

static const MbrPartitionEntry *selected_mbr_entry(const BootInfo *boot_info,
                                                   const BootPlan *boot_plan) {
    const u8 *mbr;

    if (!boot_plan_uses_mbr_path(boot_plan)) {
        return 0;
    }

    if ((boot_info->inspection_flags & BOOT_INFO_FLAG_MBR_VALID) == 0u) {
        return 0;
    }

    if (boot_plan->candidate.partition_number >= 4u) {
        return 0;
    }

    mbr = (const u8 *)MBR_SECTOR_ADDR;
    return (const MbrPartitionEntry *)(mbr + PARTITION_ENTRY_OFFSET +
                                       boot_plan->candidate.partition_number *
                                           PARTITION_ENTRY_SIZE);
}

static const char *partition_vbr_fit_label(u8 partition_type, VbrKind kind) {
    switch (partition_type)
    {
    case 0x07u:
        switch (kind)
        {
        case VBR_KIND_NTFS:
        case VBR_KIND_EXFAT:
            return "0x07 matches Windows-style VBR";
        case VBR_KIND_FAT:
            return "0x07 type but FAT-like VBR";
        case VBR_KIND_GENERIC_BOOT:
            return "0x07 type with custom boot code";
        case VBR_KIND_DATA:
            return "0x07 data partition, no VBR";
        default:
            return "0x07 type but VBR is unclear";
        }
    case 0x0Bu:
    case 0x0Cu:
        switch (kind)
        {
        case VBR_KIND_FAT:
            return "FAT type matches FAT VBR";
        case VBR_KIND_EXFAT:
            return "FAT type but exFAT-style VBR";
        case VBR_KIND_NTFS:
            return "FAT type but NTFS-style VBR";
        case VBR_KIND_GENERIC_BOOT:
            return "FAT type with custom boot code";
        case VBR_KIND_DATA:
            return "FAT type but no boot sector";
        default:
            return "FAT type with unclear VBR";
        }
    case 0x83u:
        switch (kind)
        {
        case VBR_KIND_DATA:
            return "Linux data partition, not legacy VBR";
        case VBR_KIND_GENERIC_BOOT:
            return "Linux partition has custom boot code";
        case VBR_KIND_FAT:
        case VBR_KIND_NTFS:
        case VBR_KIND_EXFAT:
            return "0x83 type but VBR says other FS";
        case VBR_KIND_UNKNOWN:
            return "Linux partition with unusual first sector";
        default:
            return "Linux partition without classic VBR";
        }
    default:
        switch (kind)
        {
        case VBR_KIND_DATA:
            return "partition starts like plain data";
        case VBR_KIND_GENERIC_BOOT:
            return "partition has bootable custom code";
        case VBR_KIND_UNKNOWN:
            return "partition type and VBR are unclear";
        default:
            return "partition type and VBR both visible";
        }
    }
}

static void refine_boot_plan_with_linux_probe(const BootInfo *boot_info,
                                              BootPlan *boot_plan) {
    VbrKind kind;

    if (!boot_plan_uses_mbr_path(boot_plan)) {
        return;
    }

    if (!bytes_match((const u8 *)boot_plan->request_label, (const u8 *)"Linux", 5u)) {
        return;
    }

    if (!linux_layout_probe_is_available(boot_info)) {
        return;
    }

    if (linux_fat_vbr_is_valid(boot_info)) {
        if (linux_fat_efi_boot_file_probe_is_available(boot_info) &&
            linux_fat_efi_systemd_file_probe_is_available(boot_info)) {
            boot_plan->status_label = "EFI managers read";
            boot_plan->support_label = "fallback and systemd-boot seen";
        } else if (linux_fat_efi_boot_file_probe_is_available(boot_info)) {
            boot_plan->status_label = "BOOTX64.EFI read";
            boot_plan->support_label = "removable-media path";
        } else if (linux_fat_efi_systemd_file_probe_is_available(boot_info)) {
            boot_plan->status_label = "systemd-boot read";
            boot_plan->support_label = "EFI manager is reachable";
        } else if (linux_fat_efi_dir_probe_is_available(boot_info)) {
            boot_plan->status_label = "/EFI dir read";
            boot_plan->support_label = "UEFI boot tree is visible";
        } else if (linux_fat_kernel_probe_is_available(boot_info) &&
            linux_fat_initrd_probe_is_available(boot_info)) {
            boot_plan->status_label = "kernel/initrd read";
            boot_plan->support_label = "entry files are reachable";
        } else if (linux_fat_kernel_probe_is_available(boot_info)) {
            boot_plan->status_label = "kernel sectors read";
            boot_plan->support_label = "kernel file is reachable";
        } else if (linux_fat_entry_file_probe_is_available(boot_info)) {
            boot_plan->status_label = "entry file sector read";
            if (linux_fat_entry_linux_matches_root(boot_info) &&
                linux_fat_entry_initrd_matches_root(boot_info)) {
                boot_plan->support_label = "entry paths match FAT root";
            } else {
                boot_plan->support_label = "boot entry text is loaded";
            }
        } else if (linux_fat_entries_dir_probe_is_available(boot_info)) {
            boot_plan->status_label = "/loader/entries dir read";
            boot_plan->support_label = "systemd-boot entries path";
        } else if (linux_fat_loader_dir_probe_is_available(boot_info)) {
            boot_plan->status_label = "/loader dir read";
            boot_plan->support_label = "systemd-boot layout";
        } else if (linux_fat_root_dir_probe_is_available(boot_info)) {
            boot_plan->status_label = "FAT root dir read";
            boot_plan->support_label = "practical /boot path";
        } else {
            boot_plan->status_label = "FAT32 boot partition";
            boot_plan->support_label = "good fit for systemd-boot";
        }
        boot_plan->action_label = linux_fat_next_action(boot_info);
        return;
    }

    if (linux_ext_superblock_is_valid(boot_info)) {
        if (linux_root_inode_probe_is_available(boot_info)) {
            if (linux_grub_cfg_inode_probe_is_available(boot_info)) {
                boot_plan->status_label = "grub.cfg inode read";
                boot_plan->support_label = "config file inode is loaded";
            } else if (linux_grub_dir_probe_is_available(boot_info)) {
                boot_plan->status_label = "/boot/grub dir read";
                boot_plan->support_label = "second child dir is loaded";
            } else if (linux_child_dir_probe_is_available(boot_info)) {
                boot_plan->status_label = "/boot dir sector read";
                boot_plan->support_label = "one child dir sector is loaded";
            } else if (linux_root_dir_probe_is_available(boot_info)) {
                boot_plan->status_label = "root dir sector read";
                boot_plan->support_label = "first dir sector is loaded";
            } else {
                boot_plan->status_label = "root inode read";
            }
            if (!linux_root_dir_probe_is_available(boot_info) &&
                !linux_grub_dir_probe_is_available(boot_info) &&
                !linux_child_dir_probe_is_available(boot_info) &&
                linux_root_extent_header_is_valid(boot_info) &&
                linux_root_extent_depth(boot_info) == 0u) {
                boot_plan->support_label = "first extent is mapped";
            } else if (linux_root_inode_is_directory(boot_info) &&
                       !linux_root_inode_uses_extents(boot_info) &&
                       !linux_root_dir_probe_is_available(boot_info) &&
                       !linux_grub_dir_probe_is_available(boot_info) &&
                       !linux_child_dir_probe_is_available(boot_info)) {
                boot_plan->support_label = "direct-block dir path";
            } else if (!linux_root_dir_probe_is_available(boot_info) &&
                       !linux_grub_dir_probe_is_available(boot_info) &&
                       !linux_child_dir_probe_is_available(boot_info)) {
                boot_plan->support_label = linux_ext_support_label(boot_info);
            }
        } else if (linux_ext_gdt_probe_is_available(boot_info)) {
            boot_plan->status_label = "ext group desc read";
            boot_plan->support_label = linux_ext_support_label(boot_info);
        } else {
            boot_plan->status_label = "ext filesystem found";
            boot_plan->support_label = linux_ext_support_label(boot_info);
        }
        boot_plan->action_label = linux_ext_next_action(
            boot_info, linux_root_inode_probe_is_available(boot_info));
        return;
    }

    kind = detect_vbr_kind((const u8 *)CHAINLOAD_BUFFER_ADDR);
    if (kind == VBR_KIND_GENERIC_BOOT) {
        boot_plan->status_label = "custom Linux boot code";
        boot_plan->action_label = "legacy chainload path";
        boot_plan->support_label = "BIOS path exists";
        return;
    }

    boot_plan->status_label = "Linux layout unclear";
    boot_plan->action_label = "inspect more Linux sectors";
    boot_plan->support_label = "needs extra disk logic";
}

static void refine_boot_plan_with_windows_probe(const BootInfo *boot_info,
                                                BootPlan *boot_plan) {
    TargetCandidate efi_candidate;
    TargetCandidate data_candidate;

    if (!bytes_match((const u8 *)boot_plan->request_label, (const u8 *)"Windows", 7u)) {
        return;
    }

    if (!windows_probe_drive_is_available(boot_info)) {
        return;
    }

    identify_windows_probe_targets(boot_info, &efi_candidate, &data_candidate);
    boot_plan->ready = 1;
    if (efi_candidate.found) {
        boot_plan->candidate = efi_candidate;
    } else if (data_candidate.found) {
        boot_plan->candidate = data_candidate;
    } else {
        boot_plan->candidate.origin = "";
        boot_plan->candidate.kind = "";
        boot_plan->candidate.first_lba = 0u;
        boot_plan->candidate.partition_number = 0u;
        boot_plan->candidate.found = 0;
        boot_plan->candidate.priority = 0;
    }

    if (windows_probe_gpt_entries_are_available(boot_info) &&
        efi_candidate.found &&
        windows_probe_esp_vbr_is_available(boot_info) &&
        data_candidate.found &&
        windows_probe_data_vbr_is_available(boot_info)) {
        boot_plan->status_label = "internal GPT sectors read";
        boot_plan->action_label = "read EFI/Microsoft tree";
        boot_plan->support_label = "good base for UEFI branch";
        return;
    }

    if (windows_probe_gpt_entries_are_available(boot_info) &&
        efi_candidate.found &&
        windows_probe_esp_vbr_is_available(boot_info)) {
        boot_plan->status_label = "internal EFI VBR read";
        boot_plan->action_label = "read EFI/Microsoft tree";
        boot_plan->support_label = "good base for UEFI branch";
        return;
    }

    if (windows_probe_gpt_entries_are_available(boot_info) && efi_candidate.found) {
        boot_plan->status_label = "internal EFI System found";
        boot_plan->action_label = "read EFI System FAT root";
        boot_plan->support_label = "good base for UEFI branch";
        return;
    }

    if (windows_probe_gpt_entries_are_available(boot_info) && data_candidate.found) {
        boot_plan->status_label = "internal Windows data found";
        boot_plan->action_label = "find EFI System partition";
        boot_plan->support_label = "needs UEFI branch";
        return;
    }

    if (windows_probe_data_vbr_is_available(boot_info)) {
        boot_plan->status_label = "internal NTFS VBR read";
        boot_plan->action_label = "look for the paired EFI partition";
        boot_plan->support_label = "mixed BIOS/UEFI clues";
        return;
    }

    if (windows_probe_mbr_is_valid(boot_info)) {
        boot_plan->status_label = "other drive answered";
        boot_plan->action_label = windows_probe_next_action(boot_info,
                                                            &efi_candidate,
                                                            &data_candidate);
        boot_plan->support_label = "needs more Windows reads";
    }
}

static void write_target_vbr_line(u32 row,
                                  const BootInfo *boot_info,
                                  const BootPlan *boot_plan) {
    const u8 *target_sector;
    u16 signature;
    VbrKind kind;

    write_string(row, 4u, 0x1F, "Target VBR:");

    if (!boot_plan_uses_mbr_path(boot_plan)) {
        write_string(row, 16u, 0x4F, "not read on this path");
        return;
    }

    if ((boot_info->inspection_flags & BOOT_INFO_FLAG_CHAINLOAD_MATCH) == 0u) {
        write_string(row, 16u, 0x4F, "no matching MBR target");
        return;
    }

    if ((boot_info->inspection_flags & BOOT_INFO_FLAG_CHAINLOAD_READ_OK) == 0u) {
        write_string(row, 16u, 0x4F, "BIOS read failed");
        return;
    }

    if (linux_root_dir_probe_is_available(boot_info)) {
        write_string(row, 16u, 0x4F, "buffer reused for root dir sector");
        return;
    }

    target_sector = (const u8 *)CHAINLOAD_BUFFER_ADDR;
    signature = read_le_u16(target_sector + 510u);
    kind = detect_vbr_kind(target_sector);

    write_string(row, 16u, 0x1E, "sig 0x");
    write_hex_byte(row, 22u, 0x1E, (u8)(signature >> 8));
    write_hex_byte(row, 24u, 0x1E, (u8)signature);
    write_string(row, 27u, 0x1E, " op 0x");
    write_hex_byte(row, 33u, 0x1E, target_sector[0]);

    if (signature == 0xAA55u) {
        write_string(row, 36u, 0x1E, "boot sig ok");
    } else {
        write_string(row, 36u, 0x4F, "missing 0xAA55");
    }

    write_string(row, 52u, 0x1E, vbr_kind_label(kind));
}

static void write_fallback_reason_line(u32 row,
                                       const BootInfo *boot_info,
                                       const BootPlan *boot_plan) {
    const MbrPartitionEntry *entry;
    TargetCandidate efi_candidate;
    TargetCandidate data_candidate;
    const u8 *target_sector;
    VbrKind kind;

    write_string(row, 4u, 0x1F, "Fallback:");

    if (boot_plan_uses_mbr_path(boot_plan)) {
        entry = selected_mbr_entry(boot_info, boot_plan);

        if ((boot_info->inspection_flags & BOOT_INFO_FLAG_CHAINLOAD_MATCH) == 0u) {
            write_string(row, 16u, 0x4F, "no MBR target matched the key");
        } else if ((boot_info->inspection_flags & BOOT_INFO_FLAG_CHAINLOAD_READ_OK) == 0u) {
            write_string(row, 16u, 0x4F, "target boot sector read failed");
        } else if (linux_fat_efi_dir_probe_is_available(boot_info)) {
            write_string(row, 16u, 0x1E, linux_fat_efi_hint(boot_info));
        } else if (linux_fat_entry_file_probe_is_available(boot_info)) {
            write_string(row, 16u, 0x1E, linux_fat_entry_file_hint(boot_info));
        } else if (linux_fat_entries_dir_probe_is_available(boot_info)) {
            write_string(row, 16u, 0x1E, linux_fat_entries_hint(boot_info));
        } else if (linux_fat_loader_dir_probe_is_available(boot_info)) {
            write_string(row, 16u, 0x1E, linux_fat_loader_hint(boot_info));
        } else if (linux_fat_root_dir_probe_is_available(boot_info)) {
            write_string(row, 16u, 0x1E, linux_fat_root_hint(boot_info));
        } else if (linux_fat_vbr_is_valid(boot_info)) {
            write_string(row, 16u, 0x1E, "FAT32 /boot partition is readable");
        } else if (linux_grub_cfg_inode_probe_is_available(boot_info)) {
            write_string(row, 16u, 0x1E, linux_grub_cfg_hint(boot_info));
        } else if (linux_grub_dir_probe_is_available(boot_info)) {
            write_string(row, 16u, 0x1E, linux_grub_dir_hint(boot_info));
        } else if (linux_child_dir_probe_is_available(boot_info)) {
            write_string(row, 16u, 0x1E, linux_child_dir_hint(boot_info));
        } else if (linux_root_dir_probe_is_available(boot_info)) {
            write_string(row, 16u, 0x1E, linux_root_dir_hint(boot_info));
        } else if (linux_root_dir_target_lba(boot_plan, boot_info) != 0u) {
            if (linux_root_extent_header_is_valid(boot_info)) {
                write_string(row, 16u, 0x1E, "root dir extent target is mapped");
            } else {
                write_string(row, 16u, 0x1E, "root dir direct block is mapped");
            }
        } else if (linux_root_inode_probe_is_available(boot_info) &&
                   linux_root_inode_is_directory(boot_info)) {
            if (linux_root_inode_uses_extents(boot_info)) {
                write_string(row, 16u, 0x1E, "root inode uses extents, parse extent tree");
            } else {
                write_string(row, 16u, 0x1E, "root inode is ready, parse directory blocks");
            }
        } else if (bytes_match((const u8 *)boot_plan->request_label, (const u8 *)"Linux", 5u) &&
                   linux_ext_superblock_is_valid(boot_info)) {
            write_string(row, 16u, 0x1E, "ext superblock found, needs file loader");
        } else if (read_le_u16((const u8 *)CHAINLOAD_BUFFER_ADDR + 510u) != 0xAA55u) {
            if (entry == 0) {
                write_string(row, 16u, 0x4F, "target sector is not bootable");
            } else {
                target_sector = (const u8 *)CHAINLOAD_BUFFER_ADDR;
                kind = detect_vbr_kind(target_sector);
                write_string(row, 16u, 0x4F, partition_vbr_fit_label(entry->partition_type, kind));
            }
        } else {
            if (entry == 0) {
                write_string(row, 16u, 0x1E, "MBR path looked bootable");
            } else {
                target_sector = (const u8 *)CHAINLOAD_BUFFER_ADDR;
                kind = detect_vbr_kind(target_sector);
                write_string(row, 16u, 0x1E, partition_vbr_fit_label(entry->partition_type, kind));
            }
        }
        return;
    }

    if (bytes_match((const u8 *)boot_plan->request_label, (const u8 *)"Windows", 7u) &&
        windows_probe_drive_is_available(boot_info)) {
        identify_windows_probe_targets(boot_info, &efi_candidate, &data_candidate);
        write_string(row, 16u, 0x1E, windows_probe_hint(boot_info,
                                                        &efi_candidate,
                                                        &data_candidate));
        return;
    }

    if ((boot_info->inspection_flags & BOOT_INFO_FLAG_MBR_VALID) == 0u) {
        write_string(row, 16u, 0x4F, "sector 0 read failed");
    } else if (!boot_plan->ready) {
        write_string(row, 16u, 0x4F, "no matching MBR partition type");
    } else {
        write_string(row, 16u, 0x1E, "selected target needs another path");
    }
}

static void write_selected_mbr_line(u32 row,
                                    const BootInfo *boot_info,
                                    const BootPlan *boot_plan) {
    const MbrPartitionEntry *entry;
    u32 sector_count;
    u32 block_size;

    write_string(row, 4u, 0x1F, "Chosen MBR:");

    if (!boot_plan_uses_mbr_path(boot_plan)) {
        write_string(row, 16u, 0x4F, "not using BIOS/MBR handoff");
        return;
    }

    entry = selected_mbr_entry(boot_info, boot_plan);
    if (entry == 0) {
        write_string(row, 16u, 0x4F, "sector 0 unavailable");
        return;
    }
    sector_count = read_le_u32(entry->sector_count);

    write_string(row, 16u, 0x1E, "p");
    VGA_TEXT_BUFFER[row * VGA_COLUMNS + 17u] =
        make_vga_cell((char)('1' + boot_plan->candidate.partition_number), 0x1E);
    write_string(row, 19u, 0x1F, "A");
    VGA_TEXT_BUFFER[row * VGA_COLUMNS + 20u] =
        make_vga_cell(entry->boot_indicator == 0x80u ? 'Y' : 'N', 0x1E);
    write_string(row, 22u, 0x1F, "t");
    write_hex_byte(row, 23u, 0x1E, entry->partition_type);
    write_string(row, 26u, 0x1F, "lba");
    write_hex_u32(row, 29u, 0x1E, read_le_u32(entry->start_lba));
    write_string(row, 38u, 0x1F, "sz");
    write_hex_u32(row, 40u, 0x1E, sector_count);

    if (bytes_match((const u8 *)boot_plan->request_label, (const u8 *)"Linux", 5u) &&
        linux_layout_probe_is_available(boot_info)) {
        if (linux_fat_vbr_is_valid(boot_info)) {
            write_string(row, 49u, 0x1F, "fat32");
            write_string(row, 55u, 0x1F, "spc");
            write_hex_byte(row, 58u, 0x1E, (u8)linux_fat_sectors_per_cluster(boot_info));
            write_string(row, 61u, 0x1F, "rt");
            write_hex_u32(row, 63u, 0x1E, linux_fat_root_cluster(boot_info));
        } else if (linux_ext_superblock_is_valid(boot_info)) {
            block_size = linux_ext_block_size(boot_info);
            write_string(row, 49u, 0x1F, "ext");
            write_hex_byte(row, 52u, 0x1E, 0xEFu);
            write_hex_byte(row, 54u, 0x1E, 0x53u);
            if (block_size != 0u) {
                write_string(row, 57u, 0x1F, "blk");
                write_hex_u32(row, 60u, 0x1E, block_size);
            }
        } else {
            write_string(row, 49u, 0x4F, "no ext magic");
        }
    }
}

static void identify_targets(const BootInfo *boot_info,
                             TargetCandidate *linux_candidate,
                             TargetCandidate *windows_candidate) {
    const u8 *mbr;
    const u8 *gpt_header;
    const GptPartitionEntry *entry;
    u32 entry_index;

    mbr = (const u8 *)MBR_SECTOR_ADDR;
    gpt_header = (const u8 *)GPT_HEADER_ADDR;

    linux_candidate->found = 0;
    linux_candidate->priority = 0;
    windows_candidate->found = 0;
    windows_candidate->priority = 0;

    if ((boot_info->inspection_flags & BOOT_INFO_FLAG_MBR_VALID) != 0u) {
        for (entry_index = 0u; entry_index < 4u; ++entry_index) {
            const MbrPartitionEntry *mbr_entry =
                (const MbrPartitionEntry *)(mbr + PARTITION_ENTRY_OFFSET +
                                            entry_index * PARTITION_ENTRY_SIZE);

            if (mbr_entry->partition_type == 0x0Bu ||
                mbr_entry->partition_type == 0x0Cu) {
                set_candidate(linux_candidate, 3, "MBR", "Linux boot FAT32",
                              entry_index, read_le_u32(mbr_entry->start_lba));
            } else if (mbr_entry->partition_type == 0x83u) {
                set_candidate(linux_candidate, 2, "MBR", "Linux",
                              entry_index, read_le_u32(mbr_entry->start_lba));
            }

            if (mbr_entry->partition_type == 0x07u ||
                mbr_entry->partition_type == 0x0Bu ||
                mbr_entry->partition_type == 0x0Cu) {
                set_candidate(windows_candidate, 2, "MBR", "Windows/FAT32",
                              entry_index, read_le_u32(mbr_entry->start_lba));
            }
        }
    }

    if (gpt_header_is_valid(boot_info, gpt_header) &&
        (boot_info->inspection_flags & BOOT_INFO_FLAG_GPT_ENTRY_VALID) != 0u) {
        const u8 *gpt_entries = (const u8 *)GPT_ENTRY_SECTOR_ADDR;

        for (entry_index = 0u; entry_index < GPT_ENTRY_COUNT_PER_SECTOR; ++entry_index) {
            entry = (const GptPartitionEntry *)(gpt_entries + entry_index * GPT_ENTRY_SIZE);

            if (bytes_are_zero(entry->type_guid, 16u)) {
                continue;
            }

            if (bytes_match(entry->type_guid, LINUX_FILESYSTEM_GUID, 16u)) {
                set_candidate(linux_candidate, 4, "GPT", "Linux FS",
                              entry_index, read_le_u32(entry->first_lba));
            } else if (bytes_match(entry->type_guid, LINUX_SWAP_GUID, 16u)) {
                set_candidate(linux_candidate, 1, "GPT", "Linux swap",
                              entry_index, read_le_u32(entry->first_lba));
            }

            if (bytes_match(entry->type_guid, MICROSOFT_BASIC_GUID, 16u)) {
                set_candidate(windows_candidate, 4, "GPT", "Microsoft data",
                              entry_index, read_le_u32(entry->first_lba));
            } else if (bytes_match(entry->type_guid, EFI_SYSTEM_GUID, 16u)) {
                set_candidate(windows_candidate, 3, "GPT", "EFI System",
                              entry_index, read_le_u32(entry->first_lba));
            } else if (bytes_match(entry->type_guid, WINDOWS_RECOVERY_GUID, 16u) ||
                       bytes_match(entry->type_guid, MICROSOFT_RESERVED_GUID, 16u)) {
                set_candidate(windows_candidate, 1, "GPT",
                              gpt_partition_type_label(entry->type_guid),
                              entry_index, read_le_u32(entry->first_lba));
            }
        }
    }
}

static void identify_windows_probe_targets(const BootInfo *boot_info,
                                          TargetCandidate *efi_candidate,
                                          TargetCandidate *data_candidate) {
    u32 entry_index;

    efi_candidate->found = 0;
    efi_candidate->priority = 0;
    data_candidate->found = 0;
    data_candidate->priority = 0;

    if (!windows_probe_mbr_is_valid(boot_info)) {
        return;
    }

    for (entry_index = 0u; entry_index < 4u; ++entry_index) {
        const MbrPartitionEntry *mbr_entry =
            (const MbrPartitionEntry *)((const u8 *)WINDOWS_PROBE_MBR_ADDR +
                                        PARTITION_ENTRY_OFFSET +
                                        entry_index * PARTITION_ENTRY_SIZE);

        if (mbr_entry->partition_type == 0x0Bu ||
            mbr_entry->partition_type == 0x0Cu) {
            set_candidate(efi_candidate,
                          2,
                          "MBR",
                          "FAT32/ESP",
                          entry_index,
                          read_le_u32(mbr_entry->start_lba));
        } else if (mbr_entry->partition_type == 0x07u) {
            set_candidate(data_candidate,
                          2,
                          "MBR",
                          "NTFS/exFAT",
                          entry_index,
                          read_le_u32(mbr_entry->start_lba));
        }
    }

    if (!windows_probe_gpt_entries_are_available(boot_info)) {
        return;
    }

    for (entry_index = 0u; entry_index < GPT_ENTRY_COUNT_PER_SECTOR; ++entry_index) {
        const GptPartitionEntry *entry =
            (const GptPartitionEntry *)((const u8 *)WINDOWS_PROBE_GPT_ENTRY_SECTOR_ADDR +
                                        entry_index * GPT_ENTRY_SIZE);

        if (bytes_are_zero(entry->type_guid, 16u)) {
            continue;
        }

        if (bytes_match(entry->type_guid, EFI_SYSTEM_GUID, 16u)) {
            set_candidate(efi_candidate,
                          4,
                          "GPT",
                          "EFI System",
                          entry_index,
                          read_le_u32(entry->first_lba));
        } else if (bytes_match(entry->type_guid, MICROSOFT_BASIC_GUID, 16u)) {
            set_candidate(data_candidate,
                          4,
                          "GPT",
                          "Microsoft data",
                          entry_index,
                          read_le_u32(entry->first_lba));
        }
    }
}

static const char *windows_probe_hint(const BootInfo *boot_info,
                                      const TargetCandidate *efi_candidate,
                                      const TargetCandidate *data_candidate) {
    if (!windows_probe_drive_is_available(boot_info)) {
        return "no other BIOS hard drive answered";
    }

    if (windows_probe_gpt_entries_are_available(boot_info) &&
        efi_candidate->found &&
        data_candidate->found) {
        if (windows_probe_esp_vbr_is_available(boot_info) &&
            windows_probe_data_vbr_is_available(boot_info)) {
            return "other drive looks like a readable Windows GPT disk";
        }

        return "other drive has EFI System plus Microsoft data";
    }

    if (windows_probe_gpt_header_is_valid(boot_info)) {
        return "other drive has a readable GPT header";
    }

    if (windows_probe_data_vbr_is_available(boot_info)) {
        return "other drive has a Windows-style data VBR";
    }

    if (windows_probe_mbr_is_valid(boot_info)) {
        return "other drive answered with an MBR";
    }

    return "other drive did not answer cleanly";
}

static const char *windows_probe_next_action(const BootInfo *boot_info,
                                             const TargetCandidate *efi_candidate,
                                             const TargetCandidate *data_candidate) {
    if (windows_probe_gpt_entries_are_available(boot_info) &&
        efi_candidate->found &&
        windows_probe_esp_vbr_is_available(boot_info)) {
        return "follow EFI/Microsoft/Boot";
    }

    if (windows_probe_gpt_entries_are_available(boot_info) && efi_candidate->found) {
        return "read EFI System FAT root";
    }

    if (windows_probe_gpt_entries_are_available(boot_info) && data_candidate->found) {
        return "find EFI System beside Windows data";
    }

    if (windows_probe_data_vbr_is_available(boot_info)) {
        return "look for the paired EFI partition";
    }

    if (windows_probe_mbr_is_valid(boot_info)) {
        return "scan more internal Windows partitions";
    }

    return "probe another BIOS hard drive";
}

static void inspect_mbr(const BootInfo *boot_info) {
    const u8 *mbr;
    const MbrPartitionEntry *entry;
    u16 signature;
    u32 entry_index;

    mbr = (const u8 *)MBR_SECTOR_ADDR;

    write_string(16u, 4u, 0x1F, "Sector 0:");

    if ((boot_info->inspection_flags & BOOT_INFO_FLAG_MBR_VALID) == 0u) {
        write_string(16u, 15u, 0x4F, "read failed");
        return;
    }

    signature = read_le_u16(mbr + 510u);
    write_string(16u, 15u, 0x1E, "signature 0x");
    write_hex_byte(16u, 27u, 0x1E, (u8)(signature >> 8));
    write_hex_byte(16u, 29u, 0x1E, (u8)signature);

    for (entry_index = 0u; entry_index < 4u; ++entry_index) {
        entry = (const MbrPartitionEntry *)(mbr + PARTITION_ENTRY_OFFSET +
                                            entry_index * PARTITION_ENTRY_SIZE);

        write_string(17u + entry_index, 4u, 0x1F, "MBR part ");
        VGA_TEXT_BUFFER[(17u + entry_index) * VGA_COLUMNS + 13u] =
            make_vga_cell((char)('1' + entry_index), 0x1F);
        write_string(17u + entry_index, 16u, 0x1F, "type 0x");
        write_hex_byte(17u + entry_index, 24u, 0x1E, entry->partition_type);
        write_string(17u + entry_index, 27u, 0x1E,
                     mbr_partition_type_label(entry->partition_type));
    }
}

static void inspect_windows_probe(const BootInfo *boot_info) {
    TargetCandidate efi_candidate;
    TargetCandidate data_candidate;

    identify_windows_probe_targets(boot_info, &efi_candidate, &data_candidate);

    write_string(16u, 4u, 0x1F, "Win probe:");
    write_string(16u, 15u, 0x1E, "drive 0x");
    write_hex_byte(16u, 23u, 0x1E, boot_info_windows_drive());
    write_string(16u, 27u, 0x1E, windows_probe_hint(boot_info,
                                                    &efi_candidate,
                                                    &data_candidate));

    write_candidate_line(17u, "EFI side:", &efi_candidate);
    write_candidate_line(18u, "OS side:", &data_candidate);

    write_string(19u, 4u, 0x1F, "Probe VBR:");
    write_string(19u, 16u,
                 windows_probe_esp_vbr_is_available(boot_info) ? 0x1E : 0x4F,
                 "ESP");
    write_string(19u, 20u,
                 windows_probe_esp_vbr_is_available(boot_info) ? 0x1E : 0x4F,
                 windows_probe_esp_vbr_label(boot_info));
    write_string(19u, 37u,
                 windows_probe_data_vbr_is_available(boot_info) ? 0x1E : 0x4F,
                 "OS");
    write_string(19u, 40u,
                 windows_probe_data_vbr_is_available(boot_info) ? 0x1E : 0x4F,
                 windows_probe_data_vbr_label(boot_info));
    write_string(19u, 57u, 0x1F, "GPT");
    if (windows_probe_gpt_entries_are_available(boot_info)) {
        write_string(19u, 61u, 0x1E, "entries");
    } else if (windows_probe_gpt_header_is_valid(boot_info)) {
        write_string(19u, 61u, 0x1E, "header");
    } else {
        write_string(19u, 61u, 0x4F, "none");
    }

    write_string(20u, 4u, 0x1F, "Next:");
    write_string(20u, 11u, 0x1E, windows_probe_next_action(boot_info,
                                                           &efi_candidate,
                                                           &data_candidate));
}

static void inspect_gpt_header(const BootInfo *boot_info) {
    (void)boot_info;
    write_string(21u, 4u, 0x1F, "Extra probe:");
    write_string(21u, 17u, 0x4F, "GPT reads skipped on this BIOS branch");
}

static void inspect_linux_layout(const BootInfo *boot_info, const BootPlan *boot_plan) {
    u32 block_size;
    u32 inode_size;
    u32 blocks_per_group;
    u32 inodes_per_group;
    u32 gdt_lba;
    u32 incompat_features;
    u16 root_mode;
    u32 root_size;
    u32 root_flags;
    u32 root_first_word;
    u32 root_target_lba;
    u16 grub_cfg_mode;
    u32 grub_cfg_size;
    const u8 *dir_entry0;
    const u8 *dir_entry1;
    const u8 *dir_entry2;
    const u8 *child_entry0;
    const u8 *child_entry1;
    const u8 *child_entry2;
    const u8 *grub_entry0;
    const u8 *grub_entry1;
    const u8 *grub_entry2;
    const u8 *fat_entry0;
    const u8 *fat_entry1;
    const u8 *fat_entry2;
    const u8 *fat_loader_entry0;
    const u8 *fat_loader_entry1;
    const u8 *fat_entries_entry0;
    const u8 *fat_entries_entry1;
    const u8 *fat_kernel_entry;
    const u8 *fat_initrd_entry;
    const u8 *fat_linux_value;
    const u8 *fat_initrd_value;

    write_string(21u, 4u, 0x1F, "Linux probe:");

    if (!linux_layout_probe_is_available(boot_info)) {
        write_string(21u, 17u, 0x4F, "not collected on this path");
        return;
    }

    if (linux_fat_vbr_is_valid(boot_info)) {
        fat_kernel_entry = linux_fat_entry_linux_root_entry(boot_info);
        fat_initrd_entry = linux_fat_entry_initrd_root_entry(boot_info);

        write_string(21u, 17u, 0x1E, "FAT32 boot partition");
        write_string(21u, 40u, 0x1F, "spc");
        write_hex_byte(21u, 43u, 0x1E, (u8)linux_fat_sectors_per_cluster(boot_info));
        write_string(21u, 47u, 0x1F, "fat");
        write_hex_u32(21u, 50u, 0x1E, linux_fat_size_sectors(boot_info));

        write_string(22u, 4u, 0x1F, "Dir hint:");
        if (linux_fat_efi_dir_probe_is_available(boot_info)) {
            write_string(22u, 14u, 0x1E, linux_fat_efi_hint(boot_info));
        } else {
            write_string(22u, 14u, 0x1E, linux_fat_root_hint(boot_info));
        }
        write_string(22u, 48u, 0x1F, "Names:");
        if (linux_fat_root_dir_probe_is_available(boot_info)) {
            fat_entry0 = linux_fat_root_dir_entry_by_index(boot_info, 0u);
            fat_entry1 = linux_fat_root_dir_entry_by_index(boot_info, 1u);
            fat_entry2 = linux_fat_root_dir_entry_by_index(boot_info, 2u);
            write_fat_dir_entry_name(22u, 55u, 0x1E, fat_entry0);
            write_fat_dir_entry_name(22u, 63u, 0x1E, fat_entry1);
            write_fat_dir_entry_name(22u, 71u, 0x1E, fat_entry2);
        } else {
            write_string(22u, 55u, 0x4F, "(n/a)");
        }

        write_string(23u, 4u, 0x1F, "FAT32:");
        write_string(23u, 12u, 0x1F, "bps");
        write_hex_u32(23u, 15u, 0x1E, linux_fat_bytes_per_sector(boot_info));
        write_string(23u, 24u, 0x1F, "rsv");
        write_hex_u32(23u, 27u, 0x1E, linux_fat_reserved_sectors(boot_info));
        write_string(23u, 36u, 0x1F, "fats");
        write_hex_byte(23u, 40u, 0x1E, (u8)linux_fat_count(boot_info));
        write_string(23u, 44u, 0x1F, "root");
        write_hex_u32(23u, 49u, 0x1E, linux_fat_root_cluster(boot_info));
        if (linux_fat_efi_boot_file_probe_is_available(boot_info) ||
            linux_fat_efi_systemd_file_probe_is_available(boot_info)) {
            write_string(23u, 58u, 0x1F, "Eb");
            write_string(23u, 61u,
                         linux_fat_efi_boot_file_probe_is_available(boot_info) ? 0x1E : 0x4F,
                         linux_fat_efi_boot_file_label(boot_info));
            write_string(23u, 69u, 0x1F, "Es");
            write_string(23u, 72u,
                         linux_fat_efi_systemd_file_probe_is_available(boot_info) ? 0x1E : 0x4F,
                         linux_fat_efi_systemd_file_label(boot_info));
        } else if (fat_kernel_entry != 0 || fat_initrd_entry != 0) {
            write_string(23u, 58u, 0x1F, "kL");
            write_hex_u32(23u, 61u, 0x1E,
                          linux_fat_entry_file_lba(boot_plan, boot_info, fat_kernel_entry));
            write_string(23u, 69u, 0x1F, "iL");
            write_hex_u32(23u, 72u, 0x1E,
                          linux_fat_entry_file_lba(boot_plan, boot_info, fat_initrd_entry));
        } else if (linux_fat_entries_dir_probe_is_available(boot_info)) {
            fat_entries_entry0 = linux_fat_entries_dir_entry_by_index(boot_info, 0u);
            fat_entries_entry1 = linux_fat_entries_dir_entry_by_index(boot_info, 1u);
            write_string(23u, 58u, 0x1F, "entry:");
            write_fat_dir_entry_name(23u, 64u, 0x1E, fat_entries_entry0);
            write_fat_dir_entry_name(23u, 72u, 0x1E, fat_entries_entry1);
        } else if (linux_fat_loader_dir_probe_is_available(boot_info)) {
            fat_loader_entry0 = linux_fat_loader_dir_entry_by_index(boot_info, 0u);
            fat_loader_entry1 = linux_fat_loader_dir_entry_by_index(boot_info, 1u);
            write_string(23u, 58u, 0x1F, "load:");
            write_fat_dir_entry_name(23u, 64u, 0x1E, fat_loader_entry0);
            write_fat_dir_entry_name(23u, 72u, 0x1E, fat_loader_entry1);
        }

        write_string(24u, 4u, 0x1F, "Root dir:");
        write_string(24u, 16u, 0x1F, "LBA");
        write_hex_u32(24u, 20u, 0x1E, linux_fat_root_dir_lba(boot_plan, boot_info));
        if (linux_fat_entry_file_probe_is_available(boot_info)) {
            fat_linux_value = linux_fat_entry_file_value_bytes(boot_info, "linux ");
            fat_initrd_value = linux_fat_entry_file_value_bytes(boot_info, "initrd ");
            write_string(24u, 31u, 0x1F, "lin");
            write_text_value_snippet(24u, 35u, 0x1E, fat_linux_value, 12u);
            write_string(24u, 48u, 0x1F, "ird");
            write_text_value_snippet(24u, 52u, 0x1E, fat_initrd_value, 10u);
            write_string(24u, 64u, 0x1F, "K:");
            write_string(24u, 66u,
                         linux_fat_kernel_probe_is_available(boot_info) ? 0x1E : 0x4F,
                         linux_fat_kernel_header_label(boot_info));
            write_string(24u, 71u, 0x1F, "I:");
            write_string(24u, 73u,
                         linux_fat_initrd_probe_is_available(boot_info) ? 0x1E : 0x4F,
                         linux_fat_initrd_header_label(boot_info));
        } else {
            write_string(24u, 31u, 0x1F, "next");
            write_string(24u, 36u, 0x1E, linux_fat_next_action(boot_info));
        }
        return;
    }

    if (!linux_ext_superblock_is_valid(boot_info)) {
        write_string(21u, 17u, 0x4F, "no ext superblock magic");
        return;
    }

    block_size = linux_ext_block_size(boot_info);
    inode_size = linux_ext_inode_size(boot_info);
    blocks_per_group = linux_ext_blocks_per_group(boot_info);
    inodes_per_group = linux_ext_inodes_per_group(boot_info);
    gdt_lba = linux_ext_gdt_lba(boot_plan, boot_info);
    incompat_features = linux_ext_incompat_features(boot_info);

    write_string(21u, 17u, 0x1E, "ext rev 0x");
    write_hex_u32(21u, 27u, 0x1E, linux_ext_revision(boot_info));
    write_string(21u, 36u, 0x1F, "blk");
    write_hex_u32(21u, 39u, 0x1E, block_size);
    write_string(21u, 48u, 0x1F, "ino");
    write_hex_u32(21u, 51u, 0x1E, inode_size);

    write_string(22u, 4u, 0x1F, "Dir hint:");
    if (linux_root_dir_probe_is_available(boot_info)) {
        write_string(22u, 14u, 0x1E, linux_root_dir_hint(boot_info));
    } else {
        write_string(22u, 14u, 0x1E, "need first root dir sector");
    }

    write_string(22u, 48u, 0x1F, "Names:");
    if (linux_root_dir_probe_is_available(boot_info)) {
        dir_entry0 = linux_root_dir_entry_by_index(boot_info, 0u);
        dir_entry1 = linux_root_dir_entry_by_index(boot_info, 1u);
        dir_entry2 = linux_root_dir_entry_by_index(boot_info, 2u);
        write_dir_entry_name(22u, 55u, 0x1E, dir_entry0);
        write_dir_entry_name(22u, 63u, 0x1E, dir_entry1);
        write_dir_entry_name(22u, 71u, 0x1E, dir_entry2);
    } else {
        write_string(22u, 55u, 0x4F, "(n/a)");
    }

    write_string(23u, 4u, 0x1F, "Groups:");
    write_string(23u, 13u, 0x1F, "blk/grp");
    write_hex_u32(23u, 20u, 0x1E, blocks_per_group);
    write_string(23u, 29u, 0x1F, "ino/grp");
    write_hex_u32(23u, 36u, 0x1E, inodes_per_group);
    write_string(23u, 45u, 0x1F, "fd");
    write_hex_u32(23u, 47u, 0x1E, linux_ext_first_data_block(boot_info));
    if (linux_grub_dir_probe_is_available(boot_info)) {
        grub_entry0 = linux_grub_dir_entry_by_index(boot_info, 0u);
        grub_entry1 = linux_grub_dir_entry_by_index(boot_info, 1u);
        grub_entry2 = linux_grub_dir_entry_by_index(boot_info, 2u);
        write_string(23u, 56u, 0x1F, "grub:");
        write_dir_entry_name(23u, 62u, 0x1E, grub_entry0);
        write_dir_entry_name(23u, 68u, 0x1E, grub_entry1);
        write_dir_entry_name(23u, 74u, 0x1E, grub_entry2);
    } else if (linux_child_dir_probe_is_available(boot_info)) {
        child_entry0 = linux_child_dir_entry_by_index(boot_info, 0u);
        child_entry1 = linux_child_dir_entry_by_index(boot_info, 1u);
        child_entry2 = linux_child_dir_entry_by_index(boot_info, 2u);
        write_string(23u, 56u, 0x1F, "/boot:");
        write_dir_entry_name(23u, 62u, 0x1E, child_entry0);
        write_dir_entry_name(23u, 68u, 0x1E, child_entry1);
        write_dir_entry_name(23u, 74u, 0x1E, child_entry2);
    } else {
        write_string(23u, 56u, 0x1F, "if");
        write_hex_u32(23u, 58u, 0x1E, incompat_features);
    }

    write_string(24u, 4u, 0x1F, "Root inode:");
    if (!linux_ext_gdt_probe_is_available(boot_info)) {
        write_string(24u, 16u, 0x1F, "GDT LBA 0x");
        write_hex_u32(24u, 26u, 0x1E, gdt_lba);
        write_string(24u, 35u, 0x4F, "needs blk>=4K follow-up read");
        return;
    }

    if (!linux_root_inode_probe_is_available(boot_info)) {
        write_string(24u, 16u, 0x4F, "GDT read ok, root inode sector missing");
        return;
    }

    root_mode = linux_root_inode_mode(boot_info);
    root_size = linux_root_inode_size(boot_info);
    root_flags = linux_root_inode_flags(boot_info);
    root_first_word = linux_root_inode_first_data_word(boot_info);
    root_target_lba = linux_root_dir_target_lba(boot_plan, boot_info);
    grub_cfg_mode = linux_grub_cfg_inode_mode(boot_info);
    grub_cfg_size = linux_grub_cfg_inode_size(boot_info);

    write_string(24u, 16u, 0x1F, "m");
    write_hex_byte(24u, 17u, 0x1E, (u8)(root_mode >> 8));
    write_hex_byte(24u, 19u, 0x1E, (u8)root_mode);
    write_string(24u, 22u, 0x1F, linux_root_inode_is_directory(boot_info) ? "dir" : "node");
    write_string(24u, 27u, 0x1F, "sz");
    write_hex_u32(24u, 29u, 0x1E, root_size);
    write_string(24u, 38u, 0x1F, "fl");
    write_hex_u32(24u, 40u, 0x1E, root_flags);
    if (linux_grub_cfg_inode_probe_is_available(boot_info)) {
        write_string(24u, 49u, 0x1F, "cfg");
        write_string(24u, 53u, 0x1F, "m");
        write_hex_byte(24u, 54u, 0x1E, (u8)(grub_cfg_mode >> 8));
        write_hex_byte(24u, 56u, 0x1E, (u8)grub_cfg_mode);
        write_string(24u, 59u, 0x1F, "sz");
        write_hex_u32(24u, 61u, 0x1E, grub_cfg_size);
        if (linux_grub_cfg_extent_header_is_valid(boot_info)) {
            write_string(24u, 70u, 0x1F, "ex");
            write_hex_u32(24u, 72u, 0x1E,
                          linux_grub_cfg_extent_first_physical_block(boot_info));
        } else if (linux_grub_cfg_inode_uses_extents(boot_info)) {
            write_string(24u, 70u, 0x4F, "ext?");
        } else {
            write_string(24u, 70u, 0x1F, "blk");
        }
    } else if (linux_root_extent_header_is_valid(boot_info)) {
        write_string(24u, 49u, 0x1F, "ex");
        write_hex_u32(24u, 51u, 0x1E, linux_root_extent_first_physical_block(boot_info));
        write_string(24u, 60u, 0x1F, "lb");
        write_hex_byte(24u, 62u, 0x1E, (u8)linux_root_extent_first_logical_block(boot_info));
        write_string(24u, 65u, 0x1F, "d");
        write_hex_byte(24u, 66u, 0x1E, (u8)linux_root_extent_depth(boot_info));
        write_string(24u, 69u, 0x1F, "l");
        write_hex_byte(24u, 70u, 0x1E, (u8)linux_root_extent_first_length(boot_info));
    } else {
        write_string(24u, 49u, 0x1F, "blk");
        write_hex_u32(24u, 53u, 0x1E, root_first_word);
        VGA_TEXT_BUFFER[24u * VGA_COLUMNS + 62u] = make_vga_cell('+', 0x1F);
        write_hex_u32(24u, 63u, 0x1E, linux_ext_root_inode_offset(boot_info));
    }

    if (root_target_lba != 0u &&
        !linux_child_dir_probe_is_available(boot_info) &&
        !linux_grub_dir_probe_is_available(boot_info)) {
        write_string(23u, 67u, 0x1F, "LBA");
        write_hex_u32(23u, 70u, 0x1E, root_target_lba);
    }
}

static void inspect_gpt_entries(const BootInfo *boot_info) {
    (void)boot_info;
    write_string(22u, 4u, 0x1F, "Why:");
    write_string(22u, 10u, 0x4F, "boot-sector space went to Linux inode reads");
    write_string(23u, 4u, 0x1F, "Later:");
    write_string(23u, 11u, 0x4F, "bring GPT/UEFI work back on another branch");
}

void boot_menu_main(void) {
    const BootInfo *boot_info;
    TargetCandidate linux_candidate;
    TargetCandidate windows_candidate;
    BootPlan boot_plan;

    /*
     * Stage 1 leaves a tiny handoff structure in low memory before entering
     * protected mode. Reading it here lets us keep Stage 2 focused on logic
     * instead of keyboard handling details from the boot sector.
     */
    boot_info = (const BootInfo *)BOOT_INFO_ADDR;

    clear_screen(0x1F);
    write_string(2u, 4u, 0x1F, "Soul Boot");
    write_string(4u, 4u, 0x1E, "Stage 2 fallback inspector.");

    write_string(6u, 4u, 0x1F, "Requested target:");
    write_string(6u, 22u, 0x1E, selected_target_label(boot_info->selected_target));

    write_string(8u, 4u, 0x1F, "Boot drive:");
    write_string(8u, 16u, 0x1E, "0x");
    write_hex_byte(8u, 18u, 0x1E, boot_info->boot_drive);

    identify_targets(boot_info, &linux_candidate, &windows_candidate);
    write_candidate_line(10u, "Linux target:", &linux_candidate);
    write_candidate_line(11u, "Windows target:", &windows_candidate);

    make_boot_plan(boot_info->selected_target, &linux_candidate, &windows_candidate,
                   &boot_plan);
    refine_boot_plan_with_linux_probe(boot_info, &boot_plan);
    refine_boot_plan_with_windows_probe(boot_info, &boot_plan);
    write_target_vbr_line(7u, boot_info, &boot_plan);
    write_selected_mbr_line(9u, boot_info, &boot_plan);
    write_fallback_reason_line(12u, boot_info, &boot_plan);
    write_boot_plan_line(13u, &boot_plan);
    write_boot_action_line(14u, &boot_plan);
    write_boot_support_line(15u, &boot_plan);

    if (windows_probe_drive_is_available(boot_info)) {
        inspect_windows_probe(boot_info);
    } else {
        inspect_mbr(boot_info);
    }
    if (linux_layout_probe_is_available(boot_info)) {
        inspect_linux_layout(boot_info, &boot_plan);
    } else {
        inspect_gpt_header(boot_info);
        inspect_gpt_entries(boot_info);
    }
}
