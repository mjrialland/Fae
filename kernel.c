#include <stdint.h>
#include <stddef.h>
#include "serial.h"
#include "idt.h"
#include "malloc.h"
#include "tar.h"
#include "sysinfo.h"
#include "gguf.h"
#include "shell.h"
#include "igpu.h"
#include "efi.h"


// Linker symbols
extern char _kernel_end[];

// Assembly symbols (from boot.asm)
extern uint32_t apic_base;
extern volatile uint8_t cores_ready;
extern uint8_t trampoline_start[];
extern uint8_t trampoline_end[];

// Multiboot structures
struct multiboot_module {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t pad;
};

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
} __attribute__((packed));

static void delay(int iterations) {
    for (volatile int i = 0; i < iterations; i++) {
        __asm__ volatile ("nop");
    }
}

static void wakeup_ap(uint8_t apic_id) {
    volatile uint32_t* lapic = (volatile uint32_t*)(uint64_t)apic_base;
    
    // Clear APIC errors
    lapic[0x280 / 4] = 0;
    
    // Send INIT IPI
    lapic[0x310 / 4] = (uint32_t)apic_id << 24;
    lapic[0x300 / 4] = 0x00004500;  // INIT assert, physical level
    delay(10000000);                // wait ~10ms
    
    // Send Startup IPI (SIPI)
    lapic[0x310 / 4] = (uint32_t)apic_id << 24;
    lapic[0x300 / 4] = 0x00004608;  // Startup, vector 0x08 (executes at 0x8000)
    delay(200000);                  // wait ~200us
    
    // Send second SIPI
    lapic[0x310 / 4] = (uint32_t)apic_id << 24;
    lapic[0x300 / 4] = 0x00004608;
    delay(200000);
}

uint32_t g_boot_magic = 0;
uint32_t g_boot_info_addr = 0;

void kmain(uint32_t magic, uint32_t multiboot_addr) {
    g_boot_magic = magic;
    g_boot_info_addr = multiboot_addr;
    // 1. Initialize serial and VGA screen
    serial_init();
    
    if (magic == 0xAE105E1F) { // UEFI Bootloader
        struct aios_boot_info* info = (struct aios_boot_info*)(uint64_t)multiboot_addr;
        if (info) {
            serial_init_framebuffer(info->framebuffer_addr, info->framebuffer_pitch,
                                    info->framebuffer_width, info->framebuffer_height,
                                    info->framebuffer_bpp);
        }
    } else if (magic == 0x36d76289) { // Multiboot2
        struct multiboot2_tag_framebuffer {
            uint32_t type;
            uint32_t size;
            uint64_t framebuffer_addr;
            uint32_t framebuffer_pitch;
            uint32_t framebuffer_width;
            uint32_t framebuffer_height;
            uint8_t framebuffer_bpp;
            uint8_t framebuffer_type;
            uint16_t reserved;
        } __attribute__((packed))* fb = sysinfo_find_multiboot2_tag(multiboot_addr, 8);
        if (fb) {
            serial_init_framebuffer(fb->framebuffer_addr, fb->framebuffer_pitch,
                                    fb->framebuffer_width, fb->framebuffer_height,
                                    fb->framebuffer_bpp);
        }
    } else { // Multiboot1
        struct multiboot_info* mbi = (struct multiboot_info*)(uint64_t)multiboot_addr;
        if (mbi && (mbi->flags & (1 << 11))) {
            serial_init_framebuffer(mbi->framebuffer_addr, mbi->framebuffer_pitch,
                                    mbi->framebuffer_width, mbi->framebuffer_height,
                                    mbi->framebuffer_bpp);
        }
    }
    
    serial_printf("----------------------------------------\n");
    serial_printf("kmain: magic = 0x%x, multiboot_addr = 0x%x\n", magic, multiboot_addr);
    serial_printf("aiOS: Booting AI-First Operating System...\n");
    serial_printf("----------------------------------------\n");
    
    serial_printf("BSP: Kernel end address: %p\n", _kernel_end);
    serial_printf("BSP: APIC base address: %p\n", (void*)(uint64_t)apic_base);
    
    // 2. Detect CPU cores, PCI, and ACPI configurations
    sysinfo_detect(magic, multiboot_addr);
    
    // 3. Initialize Memory Manager
    pmm_init((uint64_t)_kernel_end, magic, multiboot_addr);
    malloc_init();
    igpu_init();
    
    // 4. Copy trampoline code to 0x8000 for AP cores
    uint64_t tramp_size = (uint64_t)trampoline_end - (uint64_t)trampoline_start;
    serial_printf("BSP: Copying AP trampoline (%d bytes) to 0x8000...\n", tramp_size);
    memcpy((void*)0x8000, trampoline_start, tramp_size);
    
    // 5. Wake up AP cores dynamically
    int cores_detected = sysinfo_get_core_count();
    total_cores = 1; // BSP is active
    
    for (int i = 1; i < cores_detected; i++) {
        uint8_t apic_id = sysinfo_get_lapic_id(i);
        serial_printf("BSP: Waking AP core %d (LAPIC ID %d)...\n", i, apic_id);
        
        int prev_cores = cores_ready;
        wakeup_ap(apic_id);
        
        // Wait for AP core to report ready
        volatile int timeout = 10000000;
        while (cores_ready == prev_cores && timeout > 0) {
            timeout--;
            __asm__ volatile ("pause");
        }
        
        if (cores_ready > prev_cores) {
            serial_printf("BSP: AP core %d is online.\n", i);
            total_cores++;
        } else {
            serial_printf("BSP WARNING: AP core %d failed to respond.\n", i);
        }
    }
    
    serial_printf("BSP: Multi-core initialization complete. Total active cores: %d\n", total_cores);
    
    // 6. Mount RAM disk (initrd)
    void* tar_addr = NULL;
    size_t tar_size = 0;
    
    if (magic == 0xAE105E1F) { // UEFI Bootloader
        struct aios_boot_info* info = (struct aios_boot_info*)(uint64_t)multiboot_addr;
        if (info && info->instruct_addr && info->instruct_size > 0) {
            extern void tar_set_uefi_instruct(void* addr, size_t size);
            tar_set_uefi_instruct((void*)info->instruct_addr, info->instruct_size);
        }
    } else if (magic == 0x36d76289) { // Multiboot2
        struct multiboot2_tag_module {
            uint32_t type;
            uint32_t size;
            uint32_t mod_start;
            uint32_t mod_end;
            char cmdline[1];
        } __attribute__((packed))* mod = sysinfo_find_multiboot2_tag(multiboot_addr, 3);
        if (mod) {
            tar_addr = (void*)(uint64_t)mod->mod_start;
            tar_size = mod->mod_end - mod->mod_start;
            tar_init(tar_addr, tar_size);
        } else {
            serial_printf("BSP ERROR: No initrd module detected under Multiboot2!\n");
        }
    } else { // Multiboot1
        struct multiboot_info* mbi = (struct multiboot_info*)(uint64_t)multiboot_addr;
        if (mbi && (mbi->flags & (1 << 3)) && mbi->mods_count > 0) {
            struct multiboot_module* mod = (struct multiboot_module*)(uint64_t)mbi->mods_addr;
            tar_addr = (void*)(uint64_t)mod->mod_start;
            tar_size = mod->mod_end - mod->mod_start;
            tar_init(tar_addr, tar_size);
        } else {
            serial_printf("BSP ERROR: No initrd module detected under Multiboot1!\n");
        }
    }
    
    // 7. Load local GGUF model
    if (magic == 0xAE105E1F) {
        struct aios_boot_info* info = (struct aios_boot_info*)(uint64_t)multiboot_addr;
        if (info && info->model_addr && info->model_size > 0) {
            serial_printf("BSP: Found UEFI pre-loaded GGUF model at %p (%llu bytes)\n", (void*)info->model_addr, info->model_size);
            if (gguf_init((void*)info->model_addr, info->model_size)) {
                serial_printf("BSP: GGUF model successfully loaded into RAM.\n");
            } else {
                serial_printf("BSP ERROR: Failed to initialize GGUF model!\n");
            }
        } else {
            serial_printf("BSP WARNING: No UEFI pre-loaded GGUF model found!\n");
        }
    } else if (tar_addr) {
        size_t gguf_size = 0;
        char model_name[100];
        void* gguf_buf = tar_find_by_suffix(".gguf", model_name, &gguf_size);
        if (gguf_buf) {
            serial_printf("BSP: Found model %s (%d bytes)\n", model_name, gguf_size);
            if (gguf_init(gguf_buf, gguf_size)) {
                serial_printf("BSP: GGUF model successfully loaded into RAM.\n");
            } else {
                serial_printf("BSP ERROR: Failed to initialize GGUF model!\n");
            }
        } else {
            serial_printf("BSP WARNING: No GGUF model found in initrd!\n");
        }
    }
    
    // 8. Initialize Interrupts (enables keyboard typing)
    idt_init();
    
    // 9. Launch interactive shell
    shell_start();
    
    // Halt (should never reach here)
    while (1) {
        __asm__ volatile ("hlt");
    }
}
