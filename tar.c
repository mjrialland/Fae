#include "tar.h"
#include "serial.h"

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed));

static void* tar_start = NULL;
static size_t tar_len = 0;

void tar_init(void* tar_address, size_t tar_size) {
    tar_start = tar_address;
    tar_len = tar_size;
    serial_printf("TAR: Mounted initrd at %p, size %d bytes\n", tar_address, tar_size);
}

static size_t parse_octal(const char* str, int max_len) {
    size_t val = 0;
    for (int i = 0; i < max_len; i++) {
        if (str[i] == '\0' || str[i] == ' ') break;
        if (str[i] >= '0' && str[i] <= '7') {
            val = val * 8 + (str[i] - '0');
        }
    }
    return val;
}

// Check if string comparison helpers are needed - wait, they are in malloc.c (strlen, strcmp)
extern size_t strlen(const char* s);
extern int strcmp(const char* s1, const char* s2);

void tar_list(void) {
    if (!tar_start) return;
    
    char* ptr = (char*)tar_start;
    char* end = ptr + tar_len;
    
    serial_printf("Listing files in initrd:\n");
    
    while (ptr < end) {
        struct tar_header* header = (struct tar_header*)ptr;
        
        // If name is empty, we reached end of archive
        if (header->name[0] == '\0') {
            break;
        }
        
        size_t size = parse_octal(header->size, 12);
        serial_printf("  - %s (size: %llu bytes, type: %c)\n", header->name, size, header->typeflag);
        
        // Advance ptr: header block (512 bytes) + file data (aligned to 512)
        ptr += 512 + ((size + 511) & ~511);
    }
}

void* tar_find_file(const char* filename, size_t* out_size) {
    if (!tar_start) return NULL;
    
    char* ptr = (char*)tar_start;
    char* end = ptr + tar_len;
    
    while (ptr < end) {
        struct tar_header* header = (struct tar_header*)ptr;
        
        if (header->name[0] == '\0') {
            break;
        }
        
        size_t size = parse_octal(header->size, 12);
        
        if (strcmp(header->name, filename) == 0) {
            if (out_size) {
                *out_size = size;
            }
            return (void*)(ptr + 512); // File contents start after header
        }
        
        ptr += 512 + ((size + 511) & ~511);
    }
    
    return NULL;
}

static void* uefi_instruct_addr = NULL;
static size_t uefi_instruct_size = 0;

void tar_set_uefi_instruct(void* addr, size_t size) {
    uefi_instruct_addr = addr;
    uefi_instruct_size = size;
}

void* tar_find_by_suffix(const char* suffix, char* out_name, size_t* out_size) {
    if (!tar_start) {
        if (uefi_instruct_addr && strcmp(suffix, ".instruct") == 0) {
            if (out_name) {
                const char* mock_name = "uefi_companion.instruct";
                int i = 0;
                while (mock_name[i] && i < 99) {
                    out_name[i] = mock_name[i];
                    i++;
                }
                out_name[i] = '\0';
            }
            if (out_size) {
                *out_size = uefi_instruct_size;
            }
            return uefi_instruct_addr;
        }
        return NULL;
    }
    
    char* ptr = (char*)tar_start;
    char* end = ptr + tar_len;
    size_t suff_len = strlen(suffix);
    
    while (ptr < end) {
        struct tar_header* header = (struct tar_header*)ptr;
        
        if (header->name[0] == '\0') {
            break;
        }
        
        size_t size = parse_octal(header->size, 12);
        size_t name_len = strlen(header->name);
        
        if (name_len >= suff_len) {
            if (strcmp(header->name + name_len - suff_len, suffix) == 0) {
                if (out_name) {
                    for (size_t i = 0; i < name_len && i < 99; i++) {
                        out_name[i] = header->name[i];
                    }
                    out_name[name_len < 99 ? name_len : 99] = '\0';
                }
                if (out_size) {
                    *out_size = size;
                }
                return (void*)(ptr + 512);
            }
        }
        
        ptr += 512 + ((size + 511) & ~511);
    }
    
    return NULL;
}
