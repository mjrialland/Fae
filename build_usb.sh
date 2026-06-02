#!/usr/bin/env bash
# build_usb.sh - Automatically builds a dual UEFI/BIOS bootable USB drive for aiOS
# Requires root privileges (invokes sudo automatically if needed)

set -euo pipefail

# ANSI color codes for premium look
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

echo -e "${BLUE}${BOLD}======================================================================${NC}"
echo -e "${MAGENTA}${BOLD}                   aiOS Bootable USB Drive Builder                    ${NC}"
echo -e "${BLUE}${BOLD}======================================================================${NC}"

# 1. Require Root Privileges
if [ "$EUID" -ne 0 ]; then
    echo -e "${YELLOW}This script needs root privileges to partition and format the USB drive.${NC}"
    echo -e "Rerunning script with sudo..."
    exec sudo "$0" "$@"
fi

# 2. Check dependencies
DEPS=(parted mkfs.vfat grub-install dd tar wipefs blockdev bc)
MISSING_DEPS=()
for dep in "${DEPS[@]}"; do
    if ! command -v "$dep" &>/dev/null; then
        MISSING_DEPS+=("$dep")
    fi
done

if [ ${#MISSING_DEPS[@]} -ne 0 ]; then
    echo -e "${RED}${BOLD}Error: Missing required system dependencies:${NC}"
    for dep in "${MISSING_DEPS[@]}"; do
        echo -e "  - $dep"
    done
    echo -e "${YELLOW}Please install them first (e.g., sudo apt install dosfstools parted grub-pc-bin grub-efi-amd64-bin bc)${NC}"
    exit 1
fi

# 3. Model Selection Flow
echo -e "\n${CYAN}${BOLD}[Step 1] Select GGUF Model & Companion Instructions${NC}"
mapfile -t GGUF_FILES < <(find . -maxdepth 1 -name "*.gguf" -printf "%f\n" | sort)

if [ ${#GGUF_FILES[@]} -eq 0 ]; then
    echo -e "${RED}Error: No .gguf files found in the current folder!${NC}"
    echo -e "Please download a model (e.g. tiny-lm-8M.Q8_0.gguf) into the project folder first."
    exit 1
fi

echo -e "Available GGUF models:"
for i in "${!GGUF_FILES[@]}"; do
    echo -e "  $((i+1))) ${GGUF_FILES[i]}"
done

while true; do
    read -r -p "Select a model (1-${#GGUF_FILES[@]}): " CHOICE
    if [[ "$CHOICE" =~ ^[0-9]+$ ]] && [ "$CHOICE" -ge 1 ] && [ "$CHOICE" -le ${#GGUF_FILES[@]} ]; then
        GGUF_MODEL="${GGUF_FILES[$((CHOICE-1))]}"
        break
    else
        echo -e "${RED}Invalid choice. Choose a number between 1 and ${#GGUF_FILES[@]}.${NC}"
    fi
done

BASE="${GGUF_MODEL%.gguf}"
INSTRUCT_FILE="${BASE}.instruct"
if [ ! -f "$INSTRUCT_FILE" ]; then
    echo -e "${YELLOW}Companion instructions file $INSTRUCT_FILE not found. Auto-generating default system instructions...${NC}"
    echo "System: You are an AI-first OS kernel assistant. You are running on bare-metal x86_64, using multi-core local CPU execution. Be helpful, concise, and technical." > "$INSTRUCT_FILE"
fi

echo -e "${GREEN}Selected Model: $GGUF_MODEL${NC}"
echo -e "${GREEN}Selected System Instructions: $INSTRUCT_FILE${NC}"

# 4. USB Drive Detection Flow
echo -e "\n${CYAN}${BOLD}[Step 2] Identify and Select Target USB Drive${NC}"

USB_DEVS=()
USB_DESCS=()

# Read /sys/class/block to find removable USB storage devices
for dev_path in /sys/block/sd*; do
    if [ -e "$dev_path" ] && [ "$(cat "$dev_path/removable")" = "1" ]; then
        dev_name=$(basename "$dev_path")
        size_blocks=$(cat "$dev_path/size")
        
        # Calculate size in GB
        size_gb=$(echo "scale=2; $size_blocks * 512 / 1073741824" | bc)
        
        vendor=$(cat "$dev_path/device/vendor" 2>/dev/null | xargs || echo "Generic")
        model=$(cat "$dev_path/device/model" 2>/dev/null | xargs || echo "USB Drive")
        
        USB_DEVS+=("/dev/$dev_name")
        USB_DESCS+=("/dev/$dev_name - $vendor $model ($size_gb GB)")
    fi
done

if [ ${#USB_DEVS[@]} -eq 0 ]; then
    echo -e "${RED}${BOLD}Error: No removable USB flash drives detected!${NC}"
    echo -e "Please insert a USB drive and try again."
    exit 1
fi

echo -e "Detected USB Storage Devices:"
for i in "${!USB_DESCS[@]}"; do
    echo -e "  $((i+1))) ${USB_DESCS[i]}"
done

while true; do
    read -r -p "Select target USB drive (1-${#USB_DEVS[@]}): " DEV_CHOICE
    if [[ "$DEV_CHOICE" =~ ^[0-9]+$ ]] && [ "$DEV_CHOICE" -ge 1 ] && [ "$DEV_CHOICE" -le ${#USB_DEVS[@]} ]; then
        TARGET_DEV="${USB_DEVS[$((DEV_CHOICE-1))]}"
        TARGET_DESC="${USB_DESCS[$((DEV_CHOICE-1))]}"
        break
    else
        echo -e "${RED}Invalid choice. Choose a number between 1 and ${#USB_DEVS[@]}.${NC}"
    fi
done

echo -e "${GREEN}Target Device Selected: $TARGET_DESC${NC}"

# 5. Safety Checks & Existing Data Warning
echo -e "\n${CYAN}${BOLD}[Step 3] Safety Scan & Partition Verification${NC}"

# Check for existing partitions
HAS_PARTITIONS=0
if lsblk -no TYPE "$TARGET_DEV" | grep -q "part"; then
    HAS_PARTITIONS=1
fi

if [ "$HAS_PARTITIONS" -eq 0 ]; then
    echo -e "${GREEN}${BOLD}✓ ALL CLEAR: Target USB drive has no partitions and appears empty/blank.${NC}"
else
    echo -e "${RED}${BOLD}⚠ WARNING: TARGET USB DRIVE CONTAINS EXISTING PARTITIONS & DATA!${NC}"
    echo -e "${RED}Proceeding will permanently delete all files and partitions on this device:${NC}"
    lsblk -p -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT "$TARGET_DEV"
    
    echo -e "\n${YELLOW}To prevent accidental data loss, please type the confirmation phrase exactly:${NC}"
    echo -e "${BOLD}ERASE-DATA-ON-${TARGET_DEV}${NC}"
    read -r -p "Enter phrase: " CONFIRM_PHRASE
    
    if [ "$CONFIRM_PHRASE" != "ERASE-DATA-ON-${TARGET_DEV}" ]; then
        echo -e "${RED}Error: Confirmation phrase mismatch. Aborting build process.${NC}"
        exit 1
    fi
    echo -e "${GREEN}Confirmation accepted. Preparing to wipe drive...${NC}"
fi

# 6. Format and Partition USB Drive
echo -e "\n${CYAN}${BOLD}[Step 4] Formatting & Partitioning USB Drive${NC}"

# 6a. Unmount any active partitions on the device
echo "Unmounting any active mounts on $TARGET_DEV..."
for part in "${TARGET_DEV}"[0-9]*; do
    if [ -e "$part" ]; then
        if mountpoint -q "$part" 2>/dev/null || grep -q "$part" /proc/mounts; then
            echo "Unmounting $part..."
            umount -l "$part" || true
        fi
    fi
done

# 6b. Wipe existing partition tables/signatures
echo "Wiping filesystem signatures..."
wipefs -a "$TARGET_DEV"

# Zero out first 1MB of the drive to erase MBR/GPT remnants
echo "Zeroing partition boot records..."
dd if=/dev/zero of="$TARGET_DEV" bs=1M count=1 conv=fdatasync status=none

# 6c. Partitioning
echo "Creating new MBR partition table (Legacy BIOS & UEFI compatible)..."
parted -s "$TARGET_DEV" mklabel msdos

echo "Creating primary FAT32 partition..."
parted -s "$TARGET_DEV" mkpart primary fat32 1MiB 100%

echo "Marking partition 1 as bootable (active)..."
parted -s "$TARGET_DEV" set 1 boot on

echo "Informing the kernel of partition changes..."
blockdev --rereadpt "$TARGET_DEV" || true
partprobe "$TARGET_DEV" || true
sleep 2 # Let the kernel register partition nodes

# 6d. Formatting
BOOT_PART="${TARGET_DEV}1"
SYS_PART="${TARGET_DEV}2"
if [[ "$TARGET_DEV" =~ "nvme" ]] || [[ "$TARGET_DEV" =~ "loop" ]]; then
    BOOT_PART="${TARGET_DEV}p1"
    SYS_PART="${TARGET_DEV}p2"
fi

if [ ! -b "$BOOT_PART" ]; then
    if [ -b "${TARGET_DEV}p1" ]; then
        BOOT_PART="${TARGET_DEV}p1"
        SYS_PART="${TARGET_DEV}p2"
    else
        echo -e "${RED}Error: Partition node ($BOOT_PART) not found by the system.${NC}"
        exit 1
    fi
fi

echo -e "Formatting boot partition $BOOT_PART as FAT32..."
mkfs.vfat -F 32 -n "AI_BOOT" "$BOOT_PART"

echo -e "Formatting system partition $SYS_PART as FAT32..."
mkfs.vfat -F 32 -n "AI_SYSTEM" "$SYS_PART"

# 7. Compile Kernel & Pack initrd.tar
echo -e "\n${CYAN}${BOLD}[Step 5] Compiling Kernel & Bootloader${NC}"
make clean
make

# 8. Copy files and install GRUB Bootloader to Boot Partition
echo -e "\n${CYAN}${BOLD}[Step 6] Copying Files and Installing Bootloaders${NC}"

MNT_DIR=$(mktemp -d /tmp/aios_usb_mnt.XXXXXX)
VERIFY_SUCCESS=1

# --- Mount & Configure Boot Partition ---
echo "Mounting Boot Partition ($BOOT_PART)..."
mount "$BOOT_PART" "$MNT_DIR"

mkdir -p "$MNT_DIR/boot/grub"
mkdir -p "$MNT_DIR/EFI/BOOT"

echo "Copying kernel.bin and bootloader.efi..."
cp kernel.bin "$MNT_DIR/boot/kernel.bin"
cp bootloader.efi "$MNT_DIR/EFI/BOOT/BOOTX64.EFI"

echo "Packaging fallback initrd.tar..."
# Fallback to copy the selected model, but prefer tiny-lm for boot partition space optimization if available
if [ -f "tiny-lm-8M.Q8_0.gguf" ]; then
    tar -cf "$MNT_DIR/boot/initrd.tar" --format=ustar "tiny-lm-8M.Q8_0.gguf" "tiny-lm-8M.Q8_0.instruct"
else
    tar -cf "$MNT_DIR/boot/initrd.tar" --format=ustar "$GGUF_MODEL" "$INSTRUCT_FILE"
fi

# Create grub.cfg
cat << 'EOF' > "$MNT_DIR/boot/grub/grub.cfg"
set timeout=5
set default=0

menuentry "aiOS (AI-First Operating System)" {
    multiboot2 /boot/kernel.bin
    module2 /boot/initrd.tar
    boot
}
EOF

# Install BIOS GRUB Bootloader
echo "Installing GRUB Legacy BIOS bootloader..."
if ! grub-install --target=i386-pc --boot-directory="$MNT_DIR/boot" "$TARGET_DEV"; then
    echo -e "${YELLOW}Warning: BIOS grub-install failed. Your host system may lack BIOS target files (grub-pc-bin).${NC}"
fi

# Install UEFI GRUB Bootloader (as fallback, though BOOTX64.EFI is our primary boot path)
echo "Installing GRUB UEFI bootloader (removable device target)..."
if ! grub-install --target=x86_64-efi --efi-directory="$MNT_DIR" --boot-directory="$MNT_DIR/boot" --removable --recheck; then
    echo -e "${YELLOW}Warning: UEFI grub-install failed. Your host system may lack UEFI target files (grub-efi-amd64-bin).${NC}"
fi

# Verify Boot Partition
echo "Verifying Boot Partition content..."
if [ ! -f "$MNT_DIR/boot/kernel.bin" ] || [ ! -s "$MNT_DIR/boot/kernel.bin" ]; then
    echo -e "${RED}Verification Failed: /boot/kernel.bin missing or empty!${NC}"
    VERIFY_SUCCESS=0
fi
if [ ! -f "$MNT_DIR/boot/initrd.tar" ] || [ ! -s "$MNT_DIR/boot/initrd.tar" ]; then
    echo -e "${RED}Verification Failed: /boot/initrd.tar missing or empty!${NC}"
    VERIFY_SUCCESS=0
fi
if [ ! -f "$MNT_DIR/EFI/BOOT/BOOTX64.EFI" ]; then
    echo -e "${RED}Verification Failed: /EFI/BOOT/BOOTX64.EFI missing or empty!${NC}"
    VERIFY_SUCCESS=0
fi

# Unmount Boot Partition
sync
umount "$MNT_DIR"

# --- Mount & Configure System Partition ---
echo "Mounting System Partition ($SYS_PART)..."
mount "$SYS_PART" "$MNT_DIR"

echo "Copying GGUF models and companion instructions..."
cp *.gguf "$MNT_DIR/" || true
cp *.instruct "$MNT_DIR/" || true

# Verify System Partition
echo "Verifying System Partition content..."
GGUF_COUNT=$(find "$MNT_DIR" -maxdepth 1 -name "*.gguf" | wc -l)
if [ "$GGUF_COUNT" -eq 0 ]; then
    echo -e "${RED}Verification Failed: No GGUF models copied to the system partition!${NC}"
    VERIFY_SUCCESS=0
else
    echo -e "${GREEN}Detected $GGUF_COUNT models on system partition.${NC}"
fi

# Clean up mounts
sync
umount "$MNT_DIR"
rm -rf "$MNT_DIR"
trap - EXIT

# 9. Verification
echo -e "\n${CYAN}${BOLD}[Step 7] Verification Summary${NC}"

if [ "$VERIFY_SUCCESS" -eq 1 ]; then
    echo -e "${GREEN}${BOLD}✓ VERIFICATION SUCCESSFUL: All required files have been written and validated on both partitions!${NC}"
else
    echo -e "${RED}${BOLD}✗ BUILD ERROR: Drive verification failed. Please review errors above.${NC}"
    exit 1
fi

echo -e "\n${BLUE}${BOLD}======================================================================${NC}"
echo -e "${GREEN}${BOLD}                       BUILD PROCESS COMPLETE!                        ${NC}"
echo -e "${BLUE}${BOLD}======================================================================${NC}"
echo -e "You can now safely eject your USB drive."
echo -e "To boot aiOS on your Ryzen laptop:"
echo -e "  1. Plug in the USB drive."
echo -e "  2. Boot the laptop and press the boot menu key (F12, F11, or Esc)."
echo -e "  3. Select your USB drive (either UEFI or Legacy BIOS mode)."
echo -e "======================================================================"
