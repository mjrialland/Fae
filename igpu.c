#include "igpu.h"
#include "serial.h"
#include "io.h"
#include "malloc.h"
#include "gguf.h"

static int has_amd_gpu = 0;
static uint8_t gpu_bus = 0;
static uint8_t gpu_slot = 0;
static uint8_t gpu_func = 0;

extern uint64_t pml4[];

static uint64_t* main_pml4 = NULL;
static uint64_t* igpu_pml4 = NULL;

struct igpu_gate* volatile g_active_gate = NULL;

// PCIe BAR addresses and sizes
static uint64_t gpu_vram_addr = 0;
static uint64_t gpu_vram_size = 0;
static uint64_t gpu_doorbell_addr = 0;
static uint64_t gpu_doorbell_size = 0;
static uint64_t gpu_mmio_addr = 0;
static uint64_t gpu_mmio_size = 0;

// AMDGPU Ring Buffer parameters
static uint32_t* ring_buffer = NULL;
static uint32_t ring_wptr = 0;
static const uint32_t ring_size_dwords = 0x8000; // 32KB * 4 = 128KB ring buffer

// PM4 CP Command Packets
#define PACKET_TYPE3 3
#define PACKET3_WRITE_DATA 0x37
#define PACKET3_INDIRECT_BUFFER 0x3F
#define PACKET3_DISPATCH_DIRECT 0x15

static inline uint32_t pm4_packet3_header(uint8_t opcode, uint32_t count) {
    return ((PACKET_TYPE3 & 3) << 30) |
           (((count - 1) & 0x3FFF) << 16) |
           ((opcode & 0xFF) << 8);
}

static uint16_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = ((uint32_t)1 << 31) | 
                       ((uint32_t)bus << 16) | 
                       ((uint32_t)slot << 11) | 
                       ((uint32_t)func << 8) | 
                       (offset & 0xFC);
    outl(0xCF8, address);
    return (uint16_t)((inl(0xCFC) >> ((offset & 2) * 8)) & 0xFFFF);
}

static uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = ((uint32_t)1 << 31) | 
                       ((uint32_t)bus << 16) | 
                       ((uint32_t)slot << 11) | 
                       ((uint32_t)func << 8) | 
                       (offset & 0xFC);
    outl(0xCF8, address);
    return inl(0xCFC);
}

static void pci_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address = ((uint32_t)1 << 31) | 
                       ((uint32_t)bus << 16) | 
                       ((uint32_t)slot << 11) | 
                       ((uint32_t)func << 8) | 
                       (offset & 0xFC);
    outl(0xCF8, address);
    outl(0xCFC, val);
}

// Probes a PCI BAR and returns its base physical address and size
static uint64_t pci_get_bar_info(uint8_t bus, uint8_t slot, uint8_t func, int bar_idx, uint64_t* out_size) {
    uint8_t offset = 0x10 + bar_idx * 4;
    uint32_t orig_low = pci_read_dword(bus, slot, func, offset);
    
    // Check if 64-bit memory BAR (bit 2 set in type)
    int is_64 = ((orig_low & 0x6) == 0x4);
    uint32_t orig_high = 0;
    if (is_64) {
        orig_high = pci_read_dword(bus, slot, func, offset + 4);
    }
    
    // Probe size: Write all 1s
    pci_write_dword(bus, slot, func, offset, 0xFFFFFFFF);
    uint32_t mask_low = pci_read_dword(bus, slot, func, offset);
    
    uint64_t mask = 0;
    if (is_64) {
        pci_write_dword(bus, slot, func, offset + 4, 0xFFFFFFFF);
        uint32_t mask_high = pci_read_dword(bus, slot, func, offset + 4);
        mask = ((uint64_t)mask_high << 32) | mask_low;
        // Restore original high
        pci_write_dword(bus, slot, func, offset + 4, orig_high);
    } else {
        mask = (0xFFFFFFFFULL << 32) | mask_low;
    }
    
    // Restore original low
    pci_write_dword(bus, slot, func, offset, orig_low);
    
    uint64_t addr = 0;
    if (is_64) {
        addr = ((uint64_t)orig_high << 32) | (orig_low & ~0xF);
    } else {
        addr = orig_low & ~0xF;
    }
    
    uint64_t size_mask = mask & ~0xF;
    if (size_mask == 0) {
        if (out_size) *out_size = 0;
        return 0;
    }
    uint64_t size = ~size_mask + 1;
    if (out_size) *out_size = size;
    
    return addr;
}

// Maps a 2MB page into the PML4
static void map_page_2mb(uint64_t phys, uint64_t virt, uint64_t flags) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pdt_idx  = (virt >> 21) & 0x1FF;
    
    if (!(pml4[pml4_idx] & 1)) {
        void* page = pmm_alloc_page();
        if (!page) {
            serial_printf("iGPU ERROR: Out of physical memory for PDPT!\n");
            return;
        }
        memset(page, 0, 4096);
        pml4[pml4_idx] = (uint64_t)page | 3;
    }
    
    uint64_t* pdpt = (uint64_t*)(pml4[pml4_idx] & ~0xFFF);
    
    if (!(pdpt[pdpt_idx] & 1)) {
        void* page = pmm_alloc_page();
        if (!page) {
            serial_printf("iGPU ERROR: Out of physical memory for PDT!\n");
            return;
        }
        memset(page, 0, 4096);
        pdpt[pdpt_idx] = (uint64_t)page | 3;
    }
    
    uint64_t* pdt = (uint64_t*)(pdpt[pdpt_idx] & ~0xFFF);
    pdt[pdt_idx] = phys | flags | 0x80; // 0x80 = Page Size (2MB)
}

static void flush_tlb(void) {
    __asm__ volatile (
        "mov %%cr3, %%rax\n\t"
        "mov %%rax, %%cr3"
        : : : "rax", "memory"
    );
}

// Identity maps a memory range using 2MB pages
static void map_range_2mb(uint64_t start_addr, uint64_t size, int is_mmio) {
    if (size == 0) return;
    
    uint64_t flags = 3; // Present, R/W
    if (is_mmio) {
        flags |= 0x18; // PCD (Cache Disable) | PWT (Write-Through)
    }
    
    uint64_t start_page = start_addr / PAGE_SIZE_2MB;
    uint64_t end_page = (start_addr + size - 1) / PAGE_SIZE_2MB;
    
    serial_printf("iGPU: Mapping memory range %p - %p (size: %llu MB) with flags 0x%llx\n",
                  (void*)start_addr, (void*)(start_addr + size), size / (1024 * 1024), flags);
                  
    for (uint64_t page = start_page; page <= end_page; page++) {
        map_page_2mb(page * PAGE_SIZE_2MB, page * PAGE_SIZE_2MB, flags);
    }
    flush_tlb();
}

void igpu_map_bars(void) {
    if (!has_amd_gpu) return;
    
    serial_printf("iGPU: Probing PCIe BARs for AMD Display Controller...\n");
    
    // BAR0 (VRAM Framebuffer)
    gpu_vram_addr = pci_get_bar_info(gpu_bus, gpu_slot, gpu_func, 0, &gpu_vram_size);
    if (gpu_vram_addr) {
        serial_printf("iGPU: Found BAR0 (VRAM) at %p (size: %llu MB)\n",
                      (void*)gpu_vram_addr, gpu_vram_size / (1024 * 1024));
        map_range_2mb(gpu_vram_addr, gpu_vram_size, 0); // VRAM allows caching
    }
    
    // BAR2 (Doorbells)
    gpu_doorbell_addr = pci_get_bar_info(gpu_bus, gpu_slot, gpu_func, 2, &gpu_doorbell_size);
    if (gpu_doorbell_addr) {
        serial_printf("iGPU: Found BAR2 (Doorbells) at %p (size: %llu MB)\n",
                      (void*)gpu_doorbell_addr, gpu_doorbell_size / (1024 * 1024));
        map_range_2mb(gpu_doorbell_addr, gpu_doorbell_size, 1); // Doorbells: MMIO (uncached)
    }
    
    // BAR5 (MMIO Registers)
    gpu_mmio_addr = pci_get_bar_info(gpu_bus, gpu_slot, gpu_func, 5, &gpu_mmio_size);
    if (gpu_mmio_addr) {
        serial_printf("iGPU: Found BAR5 (MMIO) at %p (size: %llu KB)\n",
                      (void*)gpu_mmio_addr, gpu_mmio_size / 1024);
        map_range_2mb(gpu_mmio_addr, gpu_mmio_size, 1); // MMIO Registers: uncached
    }
}

void amd_gpu_ring_init(void) {
    if (!has_amd_gpu) return;
    
    void* page = pmm_alloc_page();
    if (!page) {
        serial_printf("iGPU ERROR: Failed to allocate physical page for GPU compute ring!\n");
        return;
    }
    
    memset(page, 0, 4096);
    ring_buffer = (uint32_t*)page;
    ring_wptr = 0;
    
    serial_printf("iGPU: AMDGPU Ring Buffer successfully allocated at physical address %p\n", ring_buffer);
}

void amd_gpu_ring_write(uint32_t val) {
    if (!ring_buffer) return;
    ring_buffer[ring_wptr] = val;
    ring_wptr = (ring_wptr + 1) % ring_size_dwords;
}

void amd_gpu_ring_commit(void) {
    if (!ring_buffer || !gpu_mmio_addr) return;
    
    // Write new wptr value to GFX compute ring wptr (mmCP_HQD_PQ_WPTR, offset 0x2C04)
    volatile uint32_t* mmio = (volatile uint32_t*)gpu_mmio_addr;
    mmio[0x2C04] = ring_wptr;
    
    // Write to doorbell if present
    if (gpu_doorbell_addr) {
        volatile uint32_t* doorbell = (volatile uint32_t*)gpu_doorbell_addr;
        doorbell[0] = ring_wptr;
    }
    
    __asm__ volatile ("sfence" : : : "memory");
}

// Clone the entire first 64GB of page tables to a private iGPU page directory
// so that the AP core executing the simulated GPU has full access to RAM
// while the CPU cores will trigger Page Faults when memory is locked.
static void igpu_init_page_tables(void) {
    main_pml4 = pml4;
    
    // Allocate 2MB page from PMM for iGPU page tables
    void* page = pmm_alloc_page();
    if (!page) {
        serial_printf("iGPU ERROR: Failed to allocate memory for iGPU page tables!\n");
        return;
    }
    
    // Offset layout within the 2MB block:
    //   0       : PML4 (4KB)
    //   4096    : PDPT (4KB)
    //   8192    : 64 PDTs (64 * 4KB = 256KB)
    igpu_pml4 = (uint64_t*)page;
    uint64_t* igpu_pdpt = (uint64_t*)((uint8_t*)page + 4096);
    uint64_t* igpu_pdts = (uint64_t*)((uint8_t*)page + 8192);
    
    // 1. Setup PML4
    memset(igpu_pml4, 0, 4096);
    igpu_pml4[0] = (uint64_t)igpu_pdpt | 3; // Present, R/W, Supervisor
    
    // 2. Setup PDPT & duplicate main PDTs
    memset(igpu_pdpt, 0, 4096);
    uint64_t* main_pdpt = (uint64_t*)(main_pml4[0] & ~0xFFF);
    
    for (int i = 0; i < 64; i++) {
        uint64_t* target_pdt = (uint64_t*)((uint8_t*)igpu_pdts + i * 4096);
        igpu_pdpt[i] = (uint64_t)target_pdt | 3; // Present, R/W, Supervisor
        
        uint64_t* main_pdt = (uint64_t*)(main_pdpt[i] & ~0xFFF);
        memcpy(target_pdt, main_pdt, 4096);
    }
    
    serial_printf("iGPU: Cloned page tables for emulator core at %p\n", igpu_pml4);
}

void igpu_init(void) {
    has_amd_gpu = 0;
    
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                uint16_t vendor = pci_read_word(bus, slot, func, 0);
                if (vendor == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }
                
                uint16_t class_reg = pci_read_word(bus, slot, func, 0x0A);
                uint8_t base_class = (class_reg >> 8) & 0xFF;
                
                if (vendor == 0x1002 && base_class == 0x03) { // AMD Display Controller
                    has_amd_gpu = 1;
                    gpu_bus = bus;
                    gpu_slot = slot;
                    gpu_func = func;
                    uint16_t device = pci_read_word(bus, slot, func, 2);
                    serial_printf("iGPU: Detected AMD Radeon Display Controller at %02x:%02x.%d (Device ID: 0x%04x)\n",
                                  bus, slot, func, device);
                    break;
                }
                
                if (func == 0) {
                    uint16_t header = pci_read_word(bus, slot, 0, 0x0E);
                    if (!(header & 0x80)) break;
                }
            }
            if (has_amd_gpu) break;
        }
        if (has_amd_gpu) break;
    }
    
    if (!has_amd_gpu) {
        serial_printf("iGPU: No AMD GPU detected on PCI bus. Activating CPU AP Emulation Fallback Pathway.\n");
    } else {
        // Map PCI BAR registers/framebuffer and allocate ring queue
        igpu_map_bars();
        amd_gpu_ring_init();
    }
    
    // Initialize the private page tables for the emulator core
    igpu_init_page_tables();
}

int igpu_has_hardware(void) {
    return has_amd_gpu;
}

void set_memory_cpu_access(void* addr, size_t size, int allowed) {
    uint64_t start_addr = (uint64_t)addr;
    uint64_t end_addr = start_addr + size;
    
    uint64_t start_page = start_addr / PAGE_SIZE_2MB;
    uint64_t end_page = (end_addr - 1) / PAGE_SIZE_2MB;
    
    for (uint64_t page = start_page; page <= end_page; page++) {
        if (page == 0) continue; // NEVER lock the first 2MB page containing kernel/stack/GDT/IDT
        
        uint64_t pdt_idx = page / 512;
        uint64_t entry_idx = page % 512;
        
        uint64_t pml4_entry = pml4[0];
        if (!(pml4_entry & 1)) continue;
        
        uint64_t* pdpt = (uint64_t*)(pml4_entry & ~0xFFF);
        uint64_t pdpt_entry = pdpt[pdt_idx];
        if (!(pdpt_entry & 1)) continue;
        
        uint64_t* pdt = (uint64_t*)(pdpt_entry & ~0xFFF);
        
        if (allowed) {
            pdt[entry_idx] |= 1; // Set Present
        } else {
            pdt[entry_idx] &= ~1; // Clear Present
        }
    }
    
    // Reload CR3 to flush TLB
    __asm__ volatile (
        "mov %%cr3, %%rax\n\t"
        "mov %%rax, %%cr3"
        :
        :
        : "rax", "memory"
    );
}

void igpu_submit_math(struct igpu_gate* gate) {
    if (!gate) return;
    
    if (has_amd_gpu && ring_buffer) {
        // PM4 CP Submission to Compute Queue (for diagnostic testing of submission queues)
        amd_gpu_ring_write(pm4_packet3_header(PACKET3_WRITE_DATA, 6));
        amd_gpu_ring_write(0x00000001); // Memory space target
        amd_gpu_ring_write((uint32_t)(uint64_t)gate->weights_buffer);
        amd_gpu_ring_write((uint32_t)((uint64_t)gate->weights_buffer >> 32));
        amd_gpu_ring_write((uint32_t)(uint64_t)gate->input_buffer);
        amd_gpu_ring_write((uint32_t)((uint64_t)gate->input_buffer >> 32));
        amd_gpu_ring_write((uint32_t)(uint64_t)gate->output_buffer);
        
        // Dispatch compute direct execution command packet
        amd_gpu_ring_write(pm4_packet3_header(PACKET3_DISPATCH_DIRECT, 3));
        amd_gpu_ring_write(gate->rows); // thread groups X
        amd_gpu_ring_write(1);          // thread groups Y
        amd_gpu_ring_write(1);          // thread groups Z
        
        // Commit queue pointers and submit command stream to CP
        amd_gpu_ring_commit();
    }
    
    // Always fall back to the software GPU emulator core (AP Core 3)
    // for actual matrix mathematics to produce correct and validated outputs
    // until GFX microcode and shader compiling are fully integrated.
    g_active_gate = gate;
    gate->status = IGPU_STATUS_PROCESSING;
    gate->owner = IGPU_OWNER_GPU;
}

void igpu_wait_math(struct igpu_gate* gate) {
    if (!gate) return;
    
    // Spin until GPU finishes
    while (gate->owner == IGPU_OWNER_GPU) {
        __asm__ volatile ("pause");
    }
}

// Emulation Helper Functions

static inline float fp16_to_fp32(uint16_t h) {
    union { uint32_t u; float f; } w;
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t exp  = (h & 0x7C00) >> 10;
    uint32_t mant = (h & 0x03FF);
    
    if (exp == 0) {
        if (mant == 0) {
            w.u = sign;
            return w.f;
        }
        while ((mant & 0x0400) == 0) {
            mant <<= 1;
            exp--;
        }
        exp++;
        mant &= ~0x0400;
        w.u = sign | (((exp - 15 + 127) & 0xFF) << 23) | (mant << 13);
        return w.f;
    } else if (exp == 31) {
        w.u = sign | 0x7F800000 | (mant << 13);
        return w.f;
    }
    
    w.u = sign | (((exp - 15 + 127) & 0xFF) << 23) | (mant << 13);
    return w.f;
}

struct block_q8_0 {
    uint16_t d;
    int8_t qs[32];
} __attribute__((packed));

struct block_q4_0 {
    uint16_t d;
    uint8_t qs[16];
} __attribute__((packed));

void igpu_run_simulated(int core_id) {
    // Switch this core to the private iGPU page tables so it retains access
    if (igpu_pml4) {
        __asm__ volatile (
            "mov %0, %%cr3"
            :
            : "r"(igpu_pml4)
            : "memory"
        );
    }
    
    serial_printf("iGPU Emulator: AP Core %d booted and switched to private page tables.\n", core_id);
    
    while (1) {
        if (g_active_gate != NULL && g_active_gate->owner == IGPU_OWNER_GPU) {
            struct igpu_gate* gate = g_active_gate;
            
            int rows = gate->rows;
            int cols = gate->cols;
            const float* vec = (const float*)gate->input_buffer;
            float* out = (float*)gate->output_buffer;
            const void* weights = gate->weights_buffer;
            
            if (gate->quant_type == 8) {
                int row_size = (cols / 32) * sizeof(struct block_q8_0);
                for (int r = 0; r < rows; r++) {
                    out[r] = dot_product_q8_0_f32((const char*)weights + r * row_size, vec, cols);
                }
            } else if (gate->quant_type == 4) {
                int row_size = (cols / 32) * sizeof(struct block_q4_0);
                for (int r = 0; r < rows; r++) {
                    out[r] = dot_product_q4_0_f32((const char*)weights + r * row_size, vec, cols);
                }
            } else {
                const float* w_float = (const float*)weights;
                for (int r = 0; r < rows; r++) {
                    float sum = 0.0f;
                    for (int c = 0; c < cols; c++) {
                        sum += w_float[r * cols + c] * vec[c];
                    }
                    out[r] = sum;
                }
            }
            
            gate->status = IGPU_STATUS_COMPLETE;
            gate->owner = IGPU_OWNER_CPU;
        }
        __asm__ volatile ("pause");
    }
}
