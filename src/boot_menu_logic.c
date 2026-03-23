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
    if (kind == VBR_KIND_NTFS) {
        return "NTFS VBR";
    }

    if (kind == VBR_KIND_FAT) {
        return "FAT VBR";
    }

    if (kind == VBR_KIND_EXFAT) {
        return "exFAT VBR";
    }

    if (kind == VBR_KIND_GENERIC_BOOT) {
        return "generic boot";
    }

    if (kind == VBR_KIND_DATA) {
        return "plain data";
    }

    if (kind == VBR_KIND_UNREAD) {
        return "not read";
    }

    return "unknown VBR";
}

static int linux_layout_probe_is_available(const BootInfo *boot_info) {
    return (boot_info->inspection_flags & BOOT_INFO_FLAG_LINUX_LAYOUT_VALID) != 0u;
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
    u32 block_size;

    if (!linux_ext_superblock_is_valid(boot_info)) {
        return 0;
    }

    block_size = linux_ext_block_size(boot_info);
    return block_size == 1024u || block_size == 2048u;
}

static u32 linux_ext_group_desc_field(const BootInfo *boot_info, u32 offset) {
    if (!linux_ext_gdt_probe_is_available(boot_info)) {
        return 0u;
    }

    return read_le_u32((const u8 *)GPT_HEADER_ADDR + offset);
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

static u32 linux_ext_root_inode_lba(const BootPlan *boot_plan,
                                    const BootInfo *boot_info) {
    u32 inode_table_block;
    u32 inode_size;

    inode_table_block = linux_ext_group_desc_field(boot_info, 8u);
    inode_size = linux_ext_inode_size(boot_info);
    if (inode_table_block == 0u || inode_size == 0u) {
        return 0u;
    }

    return linux_ext_block_to_lba(boot_plan, boot_info, inode_table_block) +
           (inode_size / 512u);
}

static u32 linux_ext_root_inode_offset(const BootInfo *boot_info) {
    u32 inode_size;

    inode_size = linux_ext_inode_size(boot_info);
    if (inode_size == 0u) {
        return 0u;
    }

    return inode_size % 512u;
}

static void write_trimmed_ascii(u32 row,
                                u32 column,
                                u8 color,
                                const u8 *bytes,
                                u32 max_length) {
    u32 length;
    u32 index;

    length = 0u;
    while (length < max_length && bytes[length] != 0u) {
        ++length;
    }

    while (length > 0u && bytes[length - 1u] == ' ') {
        --length;
    }

    if (length == 0u) {
        write_string(row, column, color, "(empty)");
        return;
    }

    for (index = 0u; index < length; ++index) {
        VGA_TEXT_BUFFER[row * VGA_COLUMNS + column + index] =
            make_vga_cell((char)bytes[index], color);
    }
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
                                         int gdt_probe_available) {
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

    if (gdt_probe_available) {
        return "read root inode";
    }

    return "read ext group descriptors";
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

static const char *mbr_partition_type_label(u8 partition_type) {
    if (partition_type == 0x00u) {
        return "Unused";
    }

    if (partition_type == 0x07u) {
        return "NTFS/exFAT";
    }

    if (partition_type == 0x83u) {
        return "Linux";
    }

    if (partition_type == 0x82u) {
        return "Linux swap";
    }

    if (partition_type == 0x0Bu || partition_type == 0x0Cu) {
        return "FAT32";
    }

    if (partition_type == 0xEEu) {
        return "GPT protective";
    }

    return "Other";
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
    if (partition_type == 0x07u) {
        if (kind == VBR_KIND_NTFS || kind == VBR_KIND_EXFAT) {
            return "0x07 matches Windows-style VBR";
        }
        if (kind == VBR_KIND_FAT) {
            return "0x07 type but FAT-like VBR";
        }
        if (kind == VBR_KIND_GENERIC_BOOT) {
            return "0x07 type with custom boot code";
        }
        if (kind == VBR_KIND_DATA) {
            return "0x07 data partition, no VBR";
        }
        return "0x07 type but VBR is unclear";
    }

    if (partition_type == 0x0Bu || partition_type == 0x0Cu) {
        if (kind == VBR_KIND_FAT) {
            return "FAT type matches FAT VBR";
        }
        if (kind == VBR_KIND_EXFAT) {
            return "FAT type but exFAT-style VBR";
        }
        if (kind == VBR_KIND_NTFS) {
            return "FAT type but NTFS-style VBR";
        }
        if (kind == VBR_KIND_GENERIC_BOOT) {
            return "FAT type with custom boot code";
        }
        if (kind == VBR_KIND_DATA) {
            return "FAT type but no boot sector";
        }
        return "FAT type with unclear VBR";
    }

    if (partition_type == 0x83u) {
        if (kind == VBR_KIND_DATA) {
            return "Linux data partition, not legacy VBR";
        }
        if (kind == VBR_KIND_GENERIC_BOOT) {
            return "Linux partition has custom boot code";
        }
        if (kind == VBR_KIND_FAT || kind == VBR_KIND_NTFS || kind == VBR_KIND_EXFAT) {
            return "0x83 type but VBR says other FS";
        }
        if (kind == VBR_KIND_UNKNOWN) {
            return "Linux partition with unusual first sector";
        }
        return "Linux partition without classic VBR";
    }

    if (kind == VBR_KIND_DATA) {
        return "partition starts like plain data";
    }

    if (kind == VBR_KIND_GENERIC_BOOT) {
        return "partition has bootable custom code";
    }

    if (kind == VBR_KIND_UNKNOWN) {
        return "partition type and VBR are unclear";
    }

    return "partition type and VBR both visible";
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

    if (linux_ext_superblock_is_valid(boot_info)) {
        if (linux_ext_gdt_probe_is_available(boot_info)) {
            boot_plan->status_label = "ext group desc read";
        } else {
            boot_plan->status_label = "ext filesystem found";
        }
        boot_plan->action_label = linux_ext_next_action(
            boot_info, linux_ext_gdt_probe_is_available(boot_info));
        boot_plan->support_label = linux_ext_support_label(boot_info);
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
    const u8 *target_sector;
    VbrKind kind;

    write_string(row, 4u, 0x1F, "Fallback:");

    if (boot_plan_uses_mbr_path(boot_plan)) {
        entry = selected_mbr_entry(boot_info, boot_plan);

        if ((boot_info->inspection_flags & BOOT_INFO_FLAG_CHAINLOAD_MATCH) == 0u) {
            write_string(row, 16u, 0x4F, "no MBR target matched the key");
        } else if ((boot_info->inspection_flags & BOOT_INFO_FLAG_CHAINLOAD_READ_OK) == 0u) {
            write_string(row, 16u, 0x4F, "target boot sector read failed");
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
        if (linux_ext_superblock_is_valid(boot_info)) {
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

            if (mbr_entry->partition_type == 0x83u) {
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

static void inspect_gpt_header(const BootInfo *boot_info) {
    const u8 *gpt_header;

    gpt_header = (const u8 *)GPT_HEADER_ADDR;

    write_string(21u, 4u, 0x1F, "Sector 1:");

    if ((boot_info->inspection_flags & BOOT_INFO_FLAG_GPT_HEADER_VALID) == 0u) {
        write_string(21u, 15u, 0x4F, "read failed");
        return;
    }

    if (gpt_header_is_valid(boot_info, gpt_header)) {
        write_string(21u, 15u, 0x1E, "GPT header found");
        return;
    }

    write_string(21u, 15u, 0x4F, "not a GPT header");
}

static void inspect_linux_layout(const BootInfo *boot_info, const BootPlan *boot_plan) {
    u32 block_size;
    u32 inode_size;
    u32 blocks_per_group;
    u32 inodes_per_group;
    u32 gdt_lba;
    u32 incompat_features;
    u32 block_bitmap_block;
    u32 inode_bitmap_block;
    u32 inode_table_block;
    u32 root_inode_lba;

    write_string(21u, 4u, 0x1F, "Linux probe:");

    if (!linux_layout_probe_is_available(boot_info)) {
        write_string(21u, 17u, 0x4F, "not collected on this path");
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

    write_string(22u, 4u, 0x1F, "Volume:");
    write_trimmed_ascii(22u, 13u, 0x1E, (const u8 *)GPT_ENTRY_SECTOR_ADDR + 120u, 16u);

    write_string(23u, 4u, 0x1F, "Groups:");
    write_string(23u, 13u, 0x1F, "blk/grp");
    write_hex_u32(23u, 20u, 0x1E, blocks_per_group);
    write_string(23u, 29u, 0x1F, "ino/grp");
    write_hex_u32(23u, 36u, 0x1E, inodes_per_group);
    write_string(23u, 45u, 0x1F, "fd");
    write_hex_u32(23u, 47u, 0x1E, linux_ext_first_data_block(boot_info));

    write_string(24u, 4u, 0x1F, "Next ext:");
    if (!linux_ext_gdt_probe_is_available(boot_info)) {
        write_string(24u, 14u, 0x1F, "GDT LBA 0x");
        write_hex_u32(24u, 24u, 0x1E, gdt_lba);
        write_string(24u, 33u, 0x4F, "needs blk>=4K follow-up read");
        return;
    }

    block_bitmap_block = linux_ext_group_desc_field(boot_info, 0u);
    inode_bitmap_block = linux_ext_group_desc_field(boot_info, 4u);
    inode_table_block = linux_ext_group_desc_field(boot_info, 8u);
    root_inode_lba = linux_ext_root_inode_lba(boot_plan, boot_info);

    write_string(24u, 14u, 0x1F, "bb");
    write_hex_u32(24u, 16u, 0x1E, block_bitmap_block);
    write_string(24u, 25u, 0x1F, "ib");
    write_hex_u32(24u, 27u, 0x1E, inode_bitmap_block);
    write_string(24u, 36u, 0x1F, "it");
    write_hex_u32(24u, 38u, 0x1E, inode_table_block);
    write_string(24u, 47u, 0x1F, "rLBA");
    write_hex_u32(24u, 51u, 0x1E, root_inode_lba);
    VGA_TEXT_BUFFER[24u * VGA_COLUMNS + 60u] = make_vga_cell('+', 0x1F);
    write_hex_u32(24u, 61u, 0x1E, linux_ext_root_inode_offset(boot_info));
    write_string(24u, 70u, 0x1F, "if");
    write_hex_u32(24u, 72u, 0x1E, incompat_features);
}

static void inspect_gpt_entries(const BootInfo *boot_info) {
    const u8 *gpt_header;
    const u8 *gpt_entries;
    const GptPartitionEntry *entry;
    u32 entry_index;

    gpt_header = (const u8 *)GPT_HEADER_ADDR;
    gpt_entries = (const u8 *)GPT_ENTRY_SECTOR_ADDR;

    write_string(22u, 4u, 0x1F, "Sector 2:");

    if ((boot_info->inspection_flags & BOOT_INFO_FLAG_GPT_ENTRY_VALID) == 0u) {
        write_string(22u, 15u, 0x4F, "read failed");
        return;
    }

    if (!gpt_header_is_valid(boot_info, gpt_header)) {
        write_string(22u, 15u, 0x4F, "not used without GPT");
        return;
    }

    for (entry_index = 0u; entry_index < GPT_ENTRY_DISPLAY_COUNT; ++entry_index) {
        entry = (const GptPartitionEntry *)(gpt_entries + entry_index * GPT_ENTRY_SIZE);
        write_string(23u + entry_index, 4u, 0x1F, "GPT part ");
        VGA_TEXT_BUFFER[(23u + entry_index) * VGA_COLUMNS + 13u] =
            make_vga_cell((char)('1' + entry_index), 0x1F);
        write_string(23u + entry_index, 16u, 0x1E,
                     gpt_partition_type_label(entry->type_guid));
    }
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

    if (!boot_info_is_valid(boot_info)) {
        write_string(6u, 4u, 0x4F, "Boot info block is missing or invalid.");
        return;
    }

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
    write_target_vbr_line(7u, boot_info, &boot_plan);
    write_selected_mbr_line(9u, boot_info, &boot_plan);
    write_fallback_reason_line(12u, boot_info, &boot_plan);
    write_boot_plan_line(13u, &boot_plan);
    write_boot_action_line(14u, &boot_plan);
    write_boot_support_line(15u, &boot_plan);

    inspect_mbr(boot_info);
    if (linux_layout_probe_is_available(boot_info)) {
        inspect_linux_layout(boot_info, &boot_plan);
    } else {
        inspect_gpt_header(boot_info);
        inspect_gpt_entries(boot_info);
    }
}
