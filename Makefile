AS := nasm
CC := gcc
LD := ld
OBJCOPY := objcopy

BIN_DIR := bin
BUILD_DIR := build

BOOT_SRC := src/boot.asm
BOOT_BIN := $(BIN_DIR)/boot.bin
BOOT_IMAGE := $(BIN_DIR)/bootloader.img

MENU_ENTRY_SRC := src/menu_loader_entry.asm
MENU_LOGIC_SRC := src/boot_menu_logic.c
MENU_LOGIC_HDR := src/boot_menu_logic.h
MENU_LINKER := memory_layout.ld

MENU_ENTRY_OBJ := $(BUILD_DIR)/menu_loader_entry.o
MENU_LOGIC_OBJ := $(BUILD_DIR)/boot_menu_logic.o
MENU_ELF := $(BIN_DIR)/menu_loader.elf
MENU_BIN := $(BIN_DIR)/menu_loader.bin

BOOT_ASFLAGS := -f bin -Wall -w-reloc-abs-word -w-reloc-abs-dword
MENU_ASFLAGS := -f elf32 -Wall -w-reloc-rel-dword
MENU_CFLAGS := -m32 -ffreestanding -fno-pie -fno-stack-protector -nostdlib -Wall -Wextra -Werror
MENU_LDFLAGS := -m elf_i386 -T $(MENU_LINKER) -nostdlib

.PHONY: all clean

all: $(MENU_ELF) $(MENU_BIN) $(BOOT_BIN) $(BOOT_IMAGE)

$(BIN_DIR) $(BUILD_DIR):
	mkdir -p $@

$(BOOT_BIN): $(BOOT_SRC) $(MENU_BIN) | $(BIN_DIR)
	sectors=$$((($$(stat -c %s $(MENU_BIN)) + 511) / 512)); \
	$(AS) $(BOOT_ASFLAGS) -DMENU_LOADER_SECTORS=$$sectors $(BOOT_SRC) -o $(BOOT_BIN)

$(MENU_ENTRY_OBJ): $(MENU_ENTRY_SRC) | $(BUILD_DIR)
	$(AS) $(MENU_ASFLAGS) $(MENU_ENTRY_SRC) -o $(MENU_ENTRY_OBJ)

$(MENU_LOGIC_OBJ): $(MENU_LOGIC_SRC) $(MENU_LOGIC_HDR) | $(BUILD_DIR)
	$(CC) $(MENU_CFLAGS) -c $(MENU_LOGIC_SRC) -o $(MENU_LOGIC_OBJ)

$(MENU_ELF): $(MENU_ENTRY_OBJ) $(MENU_LOGIC_OBJ) $(MENU_LINKER) | $(BIN_DIR)
	$(LD) $(MENU_LDFLAGS) $(MENU_ENTRY_OBJ) $(MENU_LOGIC_OBJ) -o $(MENU_ELF)

$(MENU_BIN): $(MENU_ELF) | $(BIN_DIR)
	$(OBJCOPY) -O binary $(MENU_ELF) $(MENU_BIN)

$(BOOT_IMAGE): $(BOOT_BIN) $(MENU_BIN) | $(BIN_DIR)
	loader_sectors=$$((($$(stat -c %s $(MENU_BIN)) + 511) / 512)); \
	dd if=/dev/zero of=$(BOOT_IMAGE) bs=512 count=$$((1 + $$loader_sectors)) status=none; \
	dd if=$(BOOT_BIN) of=$(BOOT_IMAGE) conv=notrunc status=none; \
	dd if=$(MENU_BIN) of=$(BOOT_IMAGE) bs=512 seek=1 conv=notrunc status=none

clean:
	rm -f $(BOOT_BIN) $(BOOT_IMAGE) $(MENU_ELF) $(MENU_BIN) $(MENU_ENTRY_OBJ) $(MENU_LOGIC_OBJ)
