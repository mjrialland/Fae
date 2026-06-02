#include "sysinfo.h"
#include "io.h"
#include "serial.h"
#include "malloc.h"

// CPUID helpers
static inline void cpuid(uint32_t code, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(code));
}

void sysinfo_print_cpu(void) {
    uint32_t eax, ebx, ecx, edx;
    
    // Check if brand string is supported
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax < 0x80000004) {
        serial_printf("CPU: Brand string not supported.\n");
        return;
    }
    
    char brand[49];
    brand[48] = '\0';
    
    cpuid(0x80000002, (uint32_t*)&brand[0],  (uint32_t*)&brand[4],  (uint32_t*)&brand[8],  (uint32_t*)&brand[12]);
    cpuid(0x80000003, (uint32_t*)&brand[16], (uint32_t*)&brand[20], (uint32_t*)&brand[24], (uint32_t*)&brand[28]);
    cpuid(0x80000004, (uint32_t*)&brand[32], (uint32_t*)&brand[36], (uint32_t*)&brand[40], (uint32_t*)&brand[44]);
    
    // Clean up leading spaces
    char* start = brand;
    while (*start == ' ') start++;
    
    serial_printf("CPU Brand: %s\n", start);
}

// PCI scanning helpers
static uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = ((uint32_t)1 << 31) | 
                       ((uint32_t)bus << 16) | 
                       ((uint32_t)slot << 11) | 
                       ((uint32_t)func << 8) | 
                       (offset & 0xFC);
    outl(0xCF8, address);
    return (uint16_t)((inl(0xCFC) >> ((offset & 2) * 8)) & 0xFFFF);
}

void sysinfo_print_pci(void) {
    serial_printf("PCI Bus Scan:\n");
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint16_t vendor = pci_read_word(bus, slot, func, 0);
                if (vendor == 0xFFFF) {
                    if (func == 0) break; // Skip slot if no function 0
                    continue;
                }
                
                uint16_t device = pci_read_word(bus, slot, func, 2);
                uint16_t class_reg = pci_read_word(bus, slot, func, 0x0A);
                uint8_t base_class = (class_reg >> 8) & 0xFF;
                uint8_t sub_class = class_reg & 0xFF;
                
                serial_printf("  - %02x:%02x.%d Vendor: 0x%04x Device: 0x%04x Class: 0x%02x Subclass: 0x%02x ",
                              bus, slot, func, vendor, device, base_class, sub_class);
                              
                // Class descriptions
                if (base_class == 0x03) {
                    serial_printf("[Display Controller]\n");
                } else if (base_class == 0x02) {
                    serial_printf("[Network Controller]\n");
                } else if (base_class == 0x01) {
                    serial_printf("[Mass Storage Controller]\n");
                } else {
                    serial_printf("\n");
                }
                
                // Multi-function check
                if (func == 0) {
                    uint16_t header = pci_read_word(bus, slot, 0, 0x0E);
                    if (!(header & 0x80)) break; // Not a multi-function device
                }
            }
        }
    }
}

// ACPI definitions
struct rsdp_descriptor {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;
} __attribute__((packed));

struct acpi_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct madt_header {
    struct acpi_header header;
    uint32_t lapic_addr;
    uint32_t flags;
} __attribute__((packed));

struct madt_record {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct madt_lapic {
    struct madt_record header;
    uint8_t processor_id;
    uint8_t lapic_id;
    uint32_t flags;
} __attribute__((packed));

static uint8_t lapic_ids[32];
static int core_count = 0;
static struct rsdp_descriptor* g_rsdp = NULL;

int sysinfo_get_core_count(void) {
    return core_count;
}

uint8_t sysinfo_get_lapic_id(int index) {
    if (index >= 0 && index < core_count) {
        return lapic_ids[index];
    }
    return 0xFF;
}

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
} __attribute__((packed));

void* sysinfo_find_multiboot2_tag(uint32_t multiboot_addr, uint32_t type) {
    struct {
        uint32_t total_size;
        uint32_t reserved;
    }* header = (void*)(uint64_t)multiboot_addr;
    
    if (!header) return NULL;
    
    uint8_t* ptr = (uint8_t*)(uint64_t)multiboot_addr + 8;
    uint8_t* end = (uint8_t*)(uint64_t)multiboot_addr + header->total_size;
    
    while (ptr < end) {
        struct {
            uint32_t type;
            uint32_t size;
        }* tag = (void*)ptr;
        
        if (tag->type == 0 && tag->size == 8) {
            break;
        }
        if (tag->type == type) {
            return tag;
        }
        ptr += ((tag->size + 7) & ~7);
    }
    return NULL;
}

static struct rsdp_descriptor* find_rsdp(uint32_t magic, uint32_t multiboot_addr) {
    if (magic == 0x36d76289) { // Multiboot2
        // 1. Try New ACPI RSDP Tag (Type 15)
        struct {
            uint32_t type;
            uint32_t size;
            uint8_t rsdp[1];
        }* new_acpi = sysinfo_find_multiboot2_tag(multiboot_addr, 15);
        if (new_acpi) {
            serial_printf("ACPI: Found RSDP address via Multiboot2 New ACPI tag\n");
            return (struct rsdp_descriptor*)new_acpi->rsdp;
        }
        // 2. Try Old ACPI RSDP Tag (Type 14)
        struct {
            uint32_t type;
            uint32_t size;
            uint8_t rsdp[1];
        }* old_acpi = sysinfo_find_multiboot2_tag(multiboot_addr, 14);
        if (old_acpi) {
            serial_printf("ACPI: Found RSDP address via Multiboot2 Old ACPI tag\n");
            return (struct rsdp_descriptor*)old_acpi->rsdp;
        }
        // 3. Try command line tag (Type 1)
        struct {
            uint32_t type;
            uint32_t size;
            char cmdline[1];
        }* cmd_tag = sysinfo_find_multiboot2_tag(multiboot_addr, 1);
        if (cmd_tag) {
            const char* p = cmd_tag->cmdline;
            while (*p) {
                if (strncmp(p, "acpi=0x", 7) == 0 || strncmp(p, "acpi=0X", 7) == 0) {
                    uint64_t val = 0;
                    p += 7;
                    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                        val <<= 4;
                        if (*p >= '0' && *p <= '9') val += *p - '0';
                        else if (*p >= 'a' && *p <= 'f') val += *p - 'a' + 10;
                        else if (*p >= 'A' && *p <= 'F') val += *p - 'A' + 10;
                        p++;
                    }
                    if (val) {
                        serial_printf("ACPI: Found RSDP address via Multiboot2 command line: 0x%llx\n", val);
                        return (struct rsdp_descriptor*)val;
                    }
                }
                p++;
            }
        }
    } else { // Multiboot1
        struct multiboot_info* mbi = (struct multiboot_info*)(uint64_t)multiboot_addr;
        if (mbi && (mbi->flags & (1 << 2)) && mbi->cmdline) {
            const char* cmd = (const char*)(uint64_t)mbi->cmdline;
            const char* p = cmd;
            while (*p) {
                if (strncmp(p, "acpi=0x", 7) == 0 || strncmp(p, "acpi=0X", 7) == 0) {
                    uint64_t val = 0;
                    p += 7;
                    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                        val <<= 4;
                        if (*p >= '0' && *p <= '9') val += *p - '0';
                        else if (*p >= 'a' && *p <= 'f') val += *p - 'a' + 10;
                        else if (*p >= 'A' && *p <= 'F') val += *p - 'A' + 10;
                        p++;
                    }
                    if (val) {
                        serial_printf("ACPI: Found RSDP address via Multiboot1 command line: 0x%llx\n", val);
                        return (struct rsdp_descriptor*)val;
                    }
                }
                p++;
            }
        }
    }
    
    // 4. Scan BIOS EBDA pointer
    uint16_t* ptr_40e = (uint16_t*)0x40E;
    __asm__ volatile("" : "+r"(ptr_40e));
    uint16_t ebda_seg = *ptr_40e;
    uint64_t ebda_phys = (uint64_t)ebda_seg << 4;
    if (ebda_phys >= 0x80000 && ebda_phys < 0xA0000) {
        for (uint64_t addr = ebda_phys; addr < ebda_phys + 1024; addr += 16) {
            if (strncmp((const char*)addr, "RSD PTR ", 8) == 0) {
                return (struct rsdp_descriptor*)addr;
            }
        }
    }
    
    // 5. Search BIOS read-only memory space for RSDP signature: "RSD PTR "
    for (uint64_t addr = 0xE0000; addr < 0x100000; addr += 16) {
        if (strncmp((const char*)addr, "RSD PTR ", 8) == 0) {
            return (struct rsdp_descriptor*)addr;
        }
    }
    return NULL;
}

void sysinfo_detect(uint32_t magic, uint32_t multiboot_addr) {
    // Default fallback: 1 core (BSP)
    core_count = 1;
    lapic_ids[0] = 0;
    
    g_rsdp = find_rsdp(magic, multiboot_addr);
    if (!g_rsdp) {
        serial_printf("ACPI: RSDP descriptor not found. Fallback to 1 core.\n");
        return;
    }
    
    serial_printf("ACPI: Found RSDP at %p (revision %d, RSDT address: 0x%x)\n",
                  g_rsdp, g_rsdp->revision, g_rsdp->rsdt_addr);
                  
    struct acpi_header* rsdt = (struct acpi_header*)(uint64_t)g_rsdp->rsdt_addr;
    if (strncmp(rsdt->signature, "RSDT", 4) != 0) {
        serial_printf("ACPI: Invalid RSDT signature!\n");
        return;
    }
    
    // Scan RSDT pointers to find MADT ("APIC")
    int entries = (rsdt->length - sizeof(struct acpi_header)) / 4;
    uint32_t* ptrs = (uint32_t*)(rsdt + 1);
    struct madt_header* madt = NULL;
    
    for (int i = 0; i < entries; i++) {
        struct acpi_header* table = (struct acpi_header*)(uint64_t)ptrs[i];
        if (strncmp(table->signature, "APIC", 4) == 0) {
            madt = (struct madt_header*)table;
            break;
        }
    }
    
    if (!madt) {
        serial_printf("ACPI: MADT (APIC) table not found in RSDT. Fallback to 1 core.\n");
        return;
    }
    
    serial_printf("ACPI: Found MADT at %p, size %d bytes\n", madt, madt->header.length);
    
    // Parse MADT records
    core_count = 0;
    uint8_t* ptr = (uint8_t*)(madt + 1);
    uint8_t* end = (uint8_t*)madt + madt->header.length;
    
    while (ptr < end) {
        struct madt_record* record = (struct madt_record*)ptr;
        if (record->length == 0) break; // Avoid infinite loop on corrupt tables
        
        if (record->type == 0) { // Processor Local APIC
            struct madt_lapic* lapic = (struct madt_lapic*)record;
            // Flags bit 0 = Enabled
            int enabled = lapic->flags & 1;
            int online_capable = lapic->flags & 2;
            if (enabled || online_capable) {
                if (core_count < 32) {
                    lapic_ids[core_count++] = lapic->lapic_id;
                }
            }
        }
        ptr += record->length;
    }
    
    serial_printf("ACPI: Detected %d CPU core(s):\n", core_count);
    for (int i = 0; i < core_count; i++) {
        serial_printf("  Core %d: LAPIC ID %d\n", i, lapic_ids[i]);
    }
}

void sysinfo_print_acpi(void) {
    struct rsdp_descriptor* rsdp = g_rsdp;
    if (!rsdp) {
        serial_printf("ACPI not available.\n");
        return;
    }
    
    struct acpi_header* rsdt = (struct acpi_header*)(uint64_t)rsdp->rsdt_addr;
    serial_printf("ACPI RSDT OEM ID: %6s, Table ID: %8s\n", rsdt->oem_id, rsdt->oem_table_id);
    
    int entries = (rsdt->length - sizeof(struct acpi_header)) / 4;
    uint32_t* ptrs = (uint32_t*)(rsdt + 1);
    for (int i = 0; i < entries; i++) {
        struct acpi_header* table = (struct acpi_header*)(uint64_t)ptrs[i];
        char signature[5] = { table->signature[0], table->signature[1], table->signature[2], table->signature[3], '\0' };
        serial_printf("  Table %d: Signature %s, Size %d bytes\n", i, signature, table->length);
    }
}

void sysinfo_poweroff(void) {
    uint32_t pm1a_cnt = 0;
    uint32_t pm1b_cnt = 0;
    
    struct rsdp_descriptor* rsdp = g_rsdp;
    if (rsdp) {
        struct acpi_header* rsdt = (struct acpi_header*)(uint64_t)rsdp->rsdt_addr;
        if (rsdt && strncmp(rsdt->signature, "RSDT", 4) == 0) {
            int entries = (rsdt->length - sizeof(struct acpi_header)) / 4;
            uint32_t* ptrs = (uint32_t*)(rsdt + 1);
            for (int i = 0; i < entries; i++) {
                struct acpi_header* table = (struct acpi_header*)(uint64_t)ptrs[i];
                if (strncmp(table->signature, "FACP", 4) == 0) {
                    // PM1a_CNT_BLK is at offset 64
                    pm1a_cnt = *(uint32_t*)((uint8_t*)table + 64);
                    // PM1b_CNT_BLK is at offset 68
                    pm1b_cnt = *(uint32_t*)((uint8_t*)table + 68);
                    break;
                }
            }
        }
    }
    
    if (pm1a_cnt) {
        serial_printf("ACPI: Powering off using PM1a_CNT 0x%x, PM1b_CNT 0x%x\n", pm1a_cnt, pm1b_cnt);
        // Try all typical SLP_TYP values for S5 (0 to 7)
        for (int slp_typ = 0; slp_typ < 8; slp_typ++) {
            uint16_t val = 0x2000 | (slp_typ << 10); // SLP_EN (bit 13) | (SLP_TYP << 10)
            outw(pm1a_cnt, val);
            if (pm1b_cnt) {
                outw(pm1b_cnt, val);
            }
        }
    }
    
    // Fallback: standard emulator/hypervisor ports
    serial_printf("ACPI: Trying fallback emulator poweroff ports...\n");
    outw(0x604, 0x2000);   // QEMU pc-i440fx / old Bochs
    outw(0x604, 0x3400);   // QEMU alternative
    outw(0xb004, 0x2000);  // QEMU q35
    outw(0xb004, 0x3400);  // QEMU q35 alternative
    outw(0x4004, 0x3400);  // VirtualBox
    outw(0x501, 0x31);     // QEMU debug exit (alternative)
}

