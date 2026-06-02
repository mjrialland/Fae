#!/usr/bin/env bash
# run_qemu.sh - Automatically compiles, packs initrd, and launches QEMU

set -e

# Find GGUF files in the current folder
mapfile -t GGUF_FILES < <(find . -maxdepth 1 -name "*.gguf" -printf "%f\n" | sort)

if [ ${#GGUF_FILES[@]} -eq 0 ]; then
    echo "Error: No .gguf files found in the current folder!"
    exit 1
fi

echo "Available GGUF models:"
for i in "${!GGUF_FILES[@]}"; do
    echo "  $((i+1))) ${GGUF_FILES[i]}"
done

while true; do
    read -r -p "Select a model (1-${#GGUF_FILES[@]}): " CHOICE
    if [[ "$CHOICE" -ge 1 && "$CHOICE" -le ${#GGUF_FILES[@]} ]]; then
        GGUF_MODEL="${GGUF_FILES[$((CHOICE-1))]}"
        break
    else
        echo "Invalid choice. Please choose a number between 1 and ${#GGUF_FILES[@]}."
    fi
done

# Try to find or auto-create corresponding .instruct file
BASE="${GGUF_MODEL%.gguf}"
INSTRUCT_FILE="${BASE}.instruct"
if [ ! -f "$INSTRUCT_FILE" ]; then
    echo "Creating default system prompt instructions: $INSTRUCT_FILE"
    echo "System: You are an AI-first OS kernel assistant. You are running on bare-metal x86_64, using multi-core local CPU execution. Be helpful, concise, and technical." > "$INSTRUCT_FILE"
fi

# Configure VM resources
CORES=4
MEM="512M"

# Calculate required memory size based on GGUF file size
FILE_SIZE=$(stat -c%s "$GGUF_MODEL")
# If model is larger than 1.5GB, allocate 8G. If larger than 3GB, allocate 16G. Else 512M
if [ "$FILE_SIZE" -gt 3221225472 ]; then
    MEM="16G"
elif [ "$FILE_SIZE" -gt 1073741824 ]; then
    MEM="8G"
fi

echo "=========================================="
echo "Preparing aiOS boot environment..."
echo "Model: $GGUF_MODEL"
echo "Template: $INSTRUCT_FILE"
echo "Allocating QEMU Memory: $MEM"
echo "=========================================="

# 1. Pack the initial RAM disk (initrd)
echo "Packing initrd.tar..."
tar -cf initrd.tar --format=ustar "$GGUF_MODEL" "$INSTRUCT_FILE"

# 2. Compile kernel
echo "Compiling kernel..."
make kernel.bin

# 3. Check for QEMU
if ! command -v qemu-system-x86_64 &> /dev/null; then
    echo "Error: qemu-system-x86_64 is not installed. Please install it."
    exit 1
fi

# 4. Launch QEMU
echo "Starting QEMU..."
qemu-system-x86_64 \
    -kernel kernel.bin \
    -initrd initrd.tar \
    -cpu max \
    -smp "$CORES" \
    -m "$MEM" \
    -serial stdio
