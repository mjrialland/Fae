# aiOS: A Bare-Metal AI-First Operating System

`aiOS` is a custom, bare-metal 64-bit x86_64 operating system designed from the ground up to natively load, dequantize, and execute Large Language Models (LLMs) locally on physical hardware. It features a custom physical memory manager, symmetric multiprocessing (SMP) core initialization, an ACPI-compliant shutdown handler, a TAR RAM disk parser, and an innovative Unified Memory Architecture (UMA) gating driver that restricts CPU access to shared graphics memory during iGPU inference.

---

## Key Features

### 1. Bare-Metal local LLM Inference (`gguf.c` / `gguf.h`)
- **GGUF V3 Parser**: Parses GGUF metadata key-value pairs and tensor alignments directly from RAM.
- **Grouped Query Attention (GQA)**: Supports modern attention topologies like Llama-3, Mistral, and Qwen-2.5-Coder by mapping query heads dynamically onto grouped key/value head slices.
- **Quantization Formats**: Natively dequantizes and executes `Q8_0` and `Q4_0` quantized tensors block-by-block.
- **ChatML Prompt Wrapping**: Preprocesses raw prompts inside standard ChatML tags (`<|im_start|>`/`<|im_end|>`) using companion `.instruct` system configuration files.

### 2. UMA iGPU Shared-Memory Gate Driver (`igpu.c` / `igpu.h`)
- **PCI Device Scan**: Detects the integrated AMD Radeon Graphics controller (Vega 7, GFX9 architecture) on Ryzen 5500U APUs using Vendor ID `0x1002` and Class Code `0x03` (Display Controller).
- **Page-Table Based Memory Lock**: Restricts CPU cores from reading/writing to weight and input activation pages during active GPU operations by clearing the `Present (bit 0)` bit in the CPU's PDT entries and reloading `CR3` to flush the TLB.
- **Private Core Page Tables**: Bootstraps a software-emulated GPU processor on AP Core 3. This core runs on a private cloned PML4 page directory block (`igpu_pml4`) so that it can access locked pages to compute math tasks without triggering access faults.
- **Hardware Access Conflict Trap**: Catches CPU memory violations inside the Page Fault handler (`pf_handler` in `idt.c`), outputting detailed register dumps and halting CPU cores to guarantee data integrity.

### 3. Symmetric Multiprocessing (SMP) & Memory Manager
- **AP Wakeup Trampoline**: Boots secondary CPU cores (APs) by copying a 16-bit real-mode assembly bootloader to `0x8000` and sending APIC Startup/Init IPIs.
- **Dynamic 2MB Page Allocator**: Dynamically builds page directories mapping the entire physical RAM layout in 2MB blocks.
- **Contiguous Heap Allocator**: Custom malloc/free implementation with boundary-tag coalescence, supporting large contiguous memory footprints needed for LLM weights and KV caches.

### 4. Hardware Diagnostics & Console Control
- **ACPI Poweroff**: Walks the ACPI RSDT to locate the FADT (FACP) table, extracting PM1a/PM1b control ports to signal soft-poweroff, with fallbacks for hypervisors (QEMU/Bochs/VirtualBox).
- **VGA/Serial Log Redirector**: Directs heavy tokenization logs and raw logits exclusively to host serial port COM1, keeping the main VGA console clean.

### 5. Dual-Partition UEFI Bootloader (`bootloader.c`, `efi_stub.asm`, `efi.h`)
- **UEFI Bootloader Wrapper**: A custom 64-bit PE32+ UEFI application (`bootloader.efi`) that runs before the kernel, scans all FAT32 partitions, lists available GGUF models, and lets the user choose one.
- **High-Memory GGUF Loading**: Allocates memory above 4GB using UEFI `AllocatePages` to load large GGUF files (e.g. Qwen 2.5 1.5B) into 64-bit space, bypassing the 32-bit physical mapping limits of GRUB and Multiboot.
- **CPU Mode Demotion Stub**: Switches the processor from 64-bit UEFI long mode back to 32-bit protected mode, setting up Multiboot registers (`eax` magic, `ebx` custom boot info) before jumping to the kernel.

---

## Repository Structure

- [bootloader.c](file:///home/mrialland/Documents/aiOS/bootloader.c) / [efi.h](file:///home/mrialland/Documents/aiOS/efi.h): Custom UEFI bootloader code and protocol structures.
- [efi_stub.asm](file:///home/mrialland/Documents/aiOS/efi_stub.asm): Assembly helper to demote CPU mode from 64-bit to 32-bit before kernel boot.
- [boot.asm](file:///home/mrialland/Documents/aiOS/boot.asm): Multiboot entry point, sets up 64-bit long mode, GDT, page directories, and jumps to `kmain`.
- [ap_trampoline.asm](file:///home/mrialland/Documents/aiOS/ap_trampoline.asm): 16-bit real-mode AP startup code to bootstrap secondary CPU cores.
- [kernel.c](file:///home/mrialland/Documents/aiOS/kernel.c): Main kernel startup, configures LAPIC, page directories, mounts models, and boots the shell.
- [malloc.c](file:///home/mrialland/Documents/aiOS/malloc.c) / [malloc.h](file:///home/mrialland/Documents/aiOS/malloc.h): Physical and virtual heap managers.
- [idt.c](file:///home/mrialland/Documents/aiOS/idt.c) / [idt.h](file:///home/mrialland/Documents/aiOS/idt.h): Interrupt Descriptor Table and Page Fault exceptions handlers.
- [igpu.c](file:///home/mrialland/Documents/aiOS/igpu.c) / [igpu.h](file:///home/mrialland/Documents/aiOS/igpu.h): AMD Radeon iGPU PCI scanner, memory protection locks, and software AP core emulator.
- [gguf.c](file:///home/mrialland/Documents/aiOS/gguf.c) / [gguf.h](file:///home/mrialland/Documents/aiOS/gguf.h): Transformer layer evaluation, Q4_0/Q8_0 dequantization, and tokenizer.
- [sysinfo.c](file:///home/mrialland/Documents/aiOS/sysinfo.c) / [sysinfo.h](file:///home/mrialland/Documents/aiOS/sysinfo.h): ACPI, PCI scan, CPUID, and ACPI shutdown tables.
- [tar.c](file:///home/mrialland/Documents/aiOS/tar.c) / [tar.h](file:///home/mrialland/Documents/aiOS/tar.h): RAM disk tar filesystem reader.
- [shell.c](file:///home/mrialland/Documents/aiOS/shell.c) / [shell.h](file:///home/mrialland/Documents/aiOS/shell.h): Interactive shell.
- [run_qemu.sh](file:///home/mrialland/Documents/aiOS/run_qemu.sh): Regression testing wrapper for QEMU emulation.
- [build_usb.sh](file:///home/mrialland/Documents/aiOS/build_usb.sh): Dual-partition bootable MBR/UEFI USB drive installer.

---

## Installation & Emulation Setup

### 1. Host Requirements
Install compiler tools and QEMU:
```bash
sudo apt update
sudo apt install build-essential nasm qemu-system-x86_64 parted dosfstools
```

### 2. Emulation (QEMU Regression Testing)
Runs the lightweight SentencePiece model inside a virtual machine using the standard Multiboot path:
```bash
chmod +x run_qemu.sh
./run_qemu.sh
```

### 3. Bare-Metal Bare Boot (UEFI USB)
Automatically builds, formats, and writes the UEFI bootloader and GGUF models onto a physical USB flash drive:
```bash
chmod +x build_usb.sh
sudo ./build_usb.sh
```
1. Select your GGUF model and USB flash drive.
2. The script will partition the USB drive into `AI_BOOT` (bootloader, kernel, fallback) and `AI_SYSTEM` (containing all GGUF models).
3. Eject the USB drive, plug it into your laptop, and boot under UEFI mode.

---

## Interactive Shell Command Reference

### `help`
Displays the available CLI commands.

### `ls`
Lists the contents loaded from the TAR initrd module.

### `cat <filename>`
Outputs the file content directly.

### `sysinfo`
Runs hardware diagnostics, mapping CPU capabilities (CPUID), listing PCI devices, and parsing ACPI tables.

### `llm "<prompt>"`
Exec feeds prompt to active GGUF LLM. Tokenization logs print to serial COM1; clean response streams directly to VGA.

### `llmstrap`
Prints detailed information about the model loaded in RAM (address, size, and companion instructions) and the bootloader environment.

### `igputest`
Invokes the UMA memory gating diagnostic to verify CPU-GPU race condition protection.

### `shutdown`
Safely exits the operating system via ACPI table PM1 control block registers.
