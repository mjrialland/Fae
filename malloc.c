#include "malloc.h"
#include "serial.h"
#include "sysinfo.h"
#include "efi.h"

// Multiboot structures
struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

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
};

#define MAX_FREE_BLOCKS 65536
static uint64_t free_blocks[MAX_FREE_BLOCKS];
static int free_blocks_count = 0;
static uint64_t total_ram_bytes = 0;

void pmm_init(uint64_t kernel_end, uint32_t magic, uint32_t multiboot_addr) {
    uint64_t mod_start = 0;
    uint64_t mod_end = 0;
    
    if (magic == 0xAE105E1F) { // UEFI Bootloader
        struct aios_boot_info* info = (struct aios_boot_info*)(uint64_t)multiboot_addr;
        serial_printf("PMM: Initializing using UEFI Boot Info structure at %p...\n", info);
        
        if (info) {
            mod_start = info->model_addr;
            mod_end = info->model_addr + info->model_size;
            uint64_t inst_start = info->instruct_addr;
            uint64_t inst_end = info->instruct_addr + info->instruct_size;
            
            serial_printf("PMM: UEFI GGUF model: %p to %p, instruct: %p to %p\n",
                          (void*)mod_start, (void*)mod_end, (void*)inst_start, (void*)inst_end);
            
            serial_printf("PMM: Parsing UEFI memory map...\n");
            for (uint32_t i = 0; i < info->mmap_entries_count; i++) {
                struct efi_mmap_entry* entry = &info->mmap_entries[i];
                if (entry->type == 1) { // Usable RAM
                    uint64_t start = entry->phys_addr;
                    uint64_t end = entry->phys_addr + entry->num_bytes;
                    total_ram_bytes += entry->num_bytes;
                    
                    start = (start + PAGE_SIZE_2MB - 1) & ~((uint64_t)PAGE_SIZE_2MB - 1);
                    end = end & ~((uint64_t)PAGE_SIZE_2MB - 1);
                    
                    for (uint64_t block = start; block < end; block += PAGE_SIZE_2MB) {
                        if (block < 0x100000) continue;
                        if (block < kernel_end) continue;
                        if (block >= mod_start && block < mod_end) continue;
                        if (block >= inst_start && block < inst_end) continue;
                        
                        if (free_blocks_count < MAX_FREE_BLOCKS) {
                            free_blocks[free_blocks_count++] = block;
                        } else {
                            break;
                        }
                    }
                }
            }
        }
    } else if (magic == 0x36d76289) { // Multiboot2
        serial_printf("PMM: Initializing using Multiboot2 info structure at %p...\n", (void*)(uint64_t)multiboot_addr);
        
        struct multiboot2_tag_module {
            uint32_t type;
            uint32_t size;
            uint32_t mod_start;
            uint32_t mod_end;
            char cmdline[1];
        } __attribute__((packed))* mod = sysinfo_find_multiboot2_tag(multiboot_addr, 3);
        
        if (mod) {
            mod_start = mod->mod_start;
            mod_end = mod->mod_end;
            serial_printf("PMM: Detected initrd module from %p to %p\n", (void*)mod_start, (void*)mod_end);
        }
        
        struct multiboot2_mmap_entry {
            uint64_t addr;
            uint64_t len;
            uint32_t type;
            uint32_t zero;
        } __attribute__((packed));
        
        struct multiboot2_tag_mmap {
            uint32_t type;
            uint32_t size;
            uint32_t entry_size;
            uint32_t entry_version;
            struct multiboot2_mmap_entry entries[1];
        } __attribute__((packed))* mmap_tag = sysinfo_find_multiboot2_tag(multiboot_addr, 6);
        
        if (!mmap_tag) {
            serial_printf("PMM ERROR: Multiboot2 memory map not provided!\n");
            uint64_t start = (kernel_end > mod_end ? kernel_end : mod_end);
            start = (start + PAGE_SIZE_2MB - 1) & ~((uint64_t)PAGE_SIZE_2MB - 1);
            for (int i = 0; i < 64; i++) {
                free_blocks[free_blocks_count++] = start + i * PAGE_SIZE_2MB;
            }
            total_ram_bytes = 128 * 1024 * 1024;
            return;
        }
        
        uint32_t entry_size = mmap_tag->entry_size;
        uint8_t* ptr = (uint8_t*)mmap_tag->entries;
        uint8_t* end = (uint8_t*)mmap_tag + mmap_tag->size;
        
        serial_printf("PMM: Parsing Multiboot2 memory map...\n");
        while (ptr < end) {
            struct multiboot2_mmap_entry* entry = (struct multiboot2_mmap_entry*)ptr;
            if (entry->type == 1) { // Usable RAM
                uint64_t start = entry->addr;
                uint64_t end = entry->addr + entry->len;
                total_ram_bytes += entry->len;
                
                start = (start + PAGE_SIZE_2MB - 1) & ~((uint64_t)PAGE_SIZE_2MB - 1);
                end = end & ~((uint64_t)PAGE_SIZE_2MB - 1);
                
                for (uint64_t block = start; block < end; block += PAGE_SIZE_2MB) {
                    if (block < 0x100000) continue;
                    if (block < kernel_end) continue;
                    if (block >= mod_start && block < mod_end) continue;
                    
                    if (free_blocks_count < MAX_FREE_BLOCKS) {
                        free_blocks[free_blocks_count++] = block;
                    } else {
                        break;
                    }
                }
            }
            ptr += entry_size;
        }
    } else { // Multiboot1
        struct multiboot_info* mbi = (struct multiboot_info*)(uint64_t)multiboot_addr;
        serial_printf("PMM: Initializing using Multiboot1 info structure at %p (flags: 0x%x)...\n", mbi, mbi ? mbi->flags : 0);
        
        if (mbi && (mbi->flags & (1 << 3)) && mbi->mods_count > 0) {
            struct multiboot_module* mod = (struct multiboot_module*)(uint64_t)mbi->mods_addr;
            mod_start = mod->mod_start;
            mod_end = mod->mod_end;
            serial_printf("PMM: Detected initrd module from %p to %p\n", (void*)mod_start, (void*)mod_end);
        }
        
        if (!mbi || !(mbi->flags & (1 << 6))) {
            serial_printf("PMM ERROR: Multiboot memory map not provided!\n");
            uint64_t start = (kernel_end > mod_end ? kernel_end : mod_end);
            start = (start + PAGE_SIZE_2MB - 1) & ~((uint64_t)PAGE_SIZE_2MB - 1);
            for (int i = 0; i < 64; i++) {
                free_blocks[free_blocks_count++] = start + i * PAGE_SIZE_2MB;
            }
            total_ram_bytes = 128 * 1024 * 1024;
            return;
        }
        
        struct multiboot_mmap_entry* entry = (struct multiboot_mmap_entry*)(uint64_t)mbi->mmap_addr;
        uint64_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
        
        serial_printf("PMM: Parsing memory map (length %d bytes)...\n", mbi->mmap_length);
        while ((uint64_t)entry < mmap_end) {
            if (entry->type == 1) {
                uint64_t start = entry->addr;
                uint64_t end = entry->addr + entry->len;
                total_ram_bytes += entry->len;
                
                start = (start + PAGE_SIZE_2MB - 1) & ~((uint64_t)PAGE_SIZE_2MB - 1);
                end = end & ~((uint64_t)PAGE_SIZE_2MB - 1);
                
                for (uint64_t block = start; block < end; block += PAGE_SIZE_2MB) {
                    if (block < 0x100000) continue;
                    if (block < kernel_end) continue;
                    if (block >= mod_start && block < mod_end) continue;
                    
                    if (free_blocks_count < MAX_FREE_BLOCKS) {
                        free_blocks[free_blocks_count++] = block;
                    } else {
                        serial_printf("PMM WARNING: Exceeded MAX_FREE_BLOCKS limit\n");
                        break;
                    }
                }
            }
            entry = (struct multiboot_mmap_entry*)((uint64_t)entry + entry->size + 4);
        }
    }
    
    serial_printf("PMM: Total RAM detected: %llu MB\n", total_ram_bytes / (1024 * 1024));
    serial_printf("PMM: Managed 2MB blocks: %d (%d MB free)\n", 
                  free_blocks_count, free_blocks_count * 2);
}

void* pmm_alloc_page(void) {
    if (free_blocks_count <= 0) {
        serial_printf("PMM ERROR: Out of physical memory!\n");
        return NULL;
    }
    return (void*)free_blocks[--free_blocks_count];
}

void* pmm_alloc_contiguous(size_t count) {
    if (count == 0) return NULL;
    if (count == 1) return pmm_alloc_page();
    
    for (int i = 0; i <= free_blocks_count - (int)count; i++) {
        int contiguous = 1;
        for (size_t j = 0; j < count - 1; j++) {
            if (free_blocks[i + j + 1] != free_blocks[i + j] + PAGE_SIZE_2MB) {
                contiguous = 0;
                break;
            }
        }
        if (contiguous) {
            void* start_addr = (void*)free_blocks[i];
            // Shift remaining blocks left
            for (int k = i + count; k < free_blocks_count; k++) {
                free_blocks[k - count] = free_blocks[k];
            }
            free_blocks_count -= count;
            return start_addr;
        }
    }
    
    serial_printf("PMM ERROR: Failed to find %d contiguous 2MB pages!\n", (int)count);
    return NULL;
}

void pmm_free_page(void* ptr) {
    uint64_t addr = (uint64_t)ptr;
    if (addr & (PAGE_SIZE_2MB - 1)) {
        serial_printf("PMM ERROR: Attempted to free unaligned address %p\n", ptr);
        return;
    }
    if (free_blocks_count < MAX_FREE_BLOCKS) {
        free_blocks[free_blocks_count++] = addr;
    }
}

// --- Heap Allocator (malloc/free) ---

struct malloc_header {
    size_t size;
    int is_free;
    struct malloc_header* next;
};

static struct malloc_header* heap_start = NULL;

void malloc_init(void) {
    heap_start = NULL;
}

static struct malloc_header* request_space(size_t size) {
    // Determine how many 2MB pages we need
    size_t total_needed = size + sizeof(struct malloc_header);
    size_t pages_needed = (total_needed + PAGE_SIZE_2MB - 1) / PAGE_SIZE_2MB;
    
    void* page = pmm_alloc_contiguous(pages_needed);
    if (!page) return NULL;

    struct malloc_header* block = (struct malloc_header*)page;
    block->size = (pages_needed * PAGE_SIZE_2MB) - sizeof(struct malloc_header);
    block->is_free = 0;
    block->next = NULL;
    return block;
}

void* malloc(size_t size) {
    if (size == 0) return NULL;

    // Align size to 16 bytes
    size = (size + 15) & ~15;

    if (!heap_start) {
        heap_start = request_space(size);
        if (!heap_start) return NULL;
        return (void*)(heap_start + 1);
    }

    struct malloc_header* curr = heap_start;
    struct malloc_header* prev = NULL;

    while (curr) {
        if (curr->is_free && curr->size >= size) {
            // Can we split?
            if (curr->size >= size + sizeof(struct malloc_header) + 16) {
                struct malloc_header* split = (struct malloc_header*)((char*)(curr + 1) + size);
                split->size = curr->size - size - sizeof(struct malloc_header);
                split->is_free = 1;
                split->next = curr->next;
                curr->size = size;
                curr->next = split;
            }
            curr->is_free = 0;
            return (void*)(curr + 1);
        }
        prev = curr;
        curr = curr->next;
    }

    // No free block found, request more space
    struct malloc_header* block = request_space(size);
    if (!block) return NULL;
    prev->next = block;
    return (void*)(block + 1);
}

void free(void* ptr) {
    if (!ptr) return;

    struct malloc_header* block = (struct malloc_header*)ptr - 1;
    block->is_free = 1;

    // Coalesce free blocks
    struct curr {
        struct malloc_header* block;
    };
    struct malloc_header* curr = heap_start;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free) {
            curr->size += sizeof(struct malloc_header) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

void* calloc(size_t num, size_t size) {
    size_t total = num * size;
    void* ptr = malloc(total);
    if (ptr) {
        char* p = (char*)ptr;
        for (size_t i = 0; i < total; i++) {
            p[i] = 0;
        }
    }
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    struct malloc_header* block = (struct malloc_header*)ptr - 1;
    if (block->size >= size) {
        return ptr;
    }

    void* new_ptr = malloc(size);
    if (new_ptr) {
        char* src = (char*)ptr;
        char* dest = (char*)new_ptr;
        for (size_t i = 0; i < block->size; i++) {
            dest[i] = src[i];
        }
        free(ptr);
    }
    return new_ptr;
}

// Basic memset and memcpy needed for C runtime
void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

void* memcpy(void* dest, const void* src, size_t n) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}
