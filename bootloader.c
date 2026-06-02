#include "efi.h"

// Define custom jump function from efi_stub.asm
extern void __attribute__((ms_abi)) efi_jmp_to_kernel(uint32_t entry, uint32_t magic, uint32_t boot_info);

// Global boot info structure, guaranteed to be below 4GB as part of the loader image
static struct aios_boot_info boot_info;

// String helpers
static void print_str(EFI_SYSTEM_TABLE* st, CHAR16* str) {
    st->ConOut->OutputString(st->ConOut, str);
}

static void print_uint(EFI_SYSTEM_TABLE* st, uint64_t val) {
    CHAR16 buf[24];
    int i = 0;
    if (val == 0) {
        buf[i++] = '0';
    } else {
        uint64_t tmp = val;
        while (tmp > 0) {
            buf[i++] = '0' + (tmp % 10);
            tmp /= 10;
        }
    }
    buf[i] = '\0';
    // Reverse
    for (int j = 0; j < i / 2; j++) {
        CHAR16 c = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = c;
    }
    print_str(st, buf);
}

static int ends_with_gguf(CHAR16* name) {
    UINTN len = 0;
    while (name[len]) len++;
    if (len < 5) return 0;
    if (name[len-5] == '.' && 
        (name[len-4] == 'g' || name[len-4] == 'G') &&
        (name[len-3] == 'g' || name[len-3] == 'G') &&
        (name[len-2] == 'u' || name[len-2] == 'U') &&
        (name[len-1] == 'f' || name[len-1] == 'F')) {
        return 1;
    }
    return 0;
}

// ELF structures
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_BOOT_SERVICES* bs = SystemTable->BootServices;
    EFI_STATUS status;

    SystemTable->ConOut->ClearScreen(SystemTable->ConOut);
    print_str(SystemTable, L"====================================================\r\n");
    print_str(SystemTable, L"           aiOS UEFI Bootloader & Manager           \r\n");
    print_str(SystemTable, L"====================================================\r\n\r\n");

    // 1. Locate all filesystems (partitions)
    UINTN num_handles = 0;
    EFI_HANDLE* handles = NULL;
    status = bs->LocateHandleBuffer(
        ByProtocol,
        (EFI_GUID*)&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
        NULL,
        &num_handles,
        &handles
    );

    if (status != EFI_SUCCESS || num_handles == 0) {
        print_str(SystemTable, L"Error: No FAT32 filesystems found!\r\n");
        return status;
    }

    // Scan up to 10 files
    static CHAR16 gguf_files[10][256];
    static EFI_HANDLE gguf_fs_handles[10];
    int gguf_count = 0;

    print_str(SystemTable, L"Scanning partitions for GGUF models...\r\n");
    for (UINTN i = 0; i < num_handles && gguf_count < 10; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = NULL;
        status = bs->HandleProtocol(handles[i], (EFI_GUID*)&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void**)&fs);
        if (status != EFI_SUCCESS) continue;

        EFI_FILE_PROTOCOL* root = NULL;
        status = fs->OpenVolume(fs, &root);
        if (status != EFI_SUCCESS) continue;

        // Read directory entries
        UINTN buf_size = 1024;
        static uint8_t info_buf[1024];
        
        while (gguf_count < 10) {
            buf_size = 1024;
            status = root->Read(root, &buf_size, info_buf);
            if (status != EFI_SUCCESS || buf_size == 0) break;

            EFI_FILE_INFO* file_info = (EFI_FILE_INFO*)info_buf;
            // Ignore directories and volume labels
            if (!(file_info->Attribute & 0x10) && !(file_info->Attribute & 0x08)) {
                if (ends_with_gguf(file_info->FileName)) {
                    // Save file name
                    UINTN n_idx = 0;
                    while (file_info->FileName[n_idx] && n_idx < 255) {
                        gguf_files[gguf_count][n_idx] = file_info->FileName[n_idx];
                        n_idx++;
                    }
                    gguf_files[gguf_count][n_idx] = L'\0';
                    gguf_fs_handles[gguf_count] = handles[i];
                    gguf_count++;
                }
            }
        }
        root->Close(root);
    }

    if (gguf_count == 0) {
        print_str(SystemTable, L"Error: No GGUF models (.gguf) found on any drive!\r\n");
        return EFI_NOT_FOUND;
    }

    // 2. Present Model Selection Menu
    print_str(SystemTable, L"\r\nSelect GGUF model to load:\r\n");
    for (int i = 0; i < gguf_count; i++) {
        print_str(SystemTable, L"  ");
        CHAR16 num_str[4];
        num_str[0] = '1' + i;
        num_str[1] = ')';
        num_str[2] = ' ';
        num_str[3] = '\0';
        print_str(SystemTable, num_str);
        print_str(SystemTable, gguf_files[i]);
        print_str(SystemTable, L"\r\n");
    }

    int selected_idx = -1;
    print_str(SystemTable, L"\r\nEnter selection (1-9): ");
    while (selected_idx == -1) {
        EFI_INPUT_KEY key;
        status = SystemTable->ConIn->ReadKeyStroke(SystemTable->ConIn, &key);
        if (status == EFI_SUCCESS) {
            if (key.UnicodeChar >= '1' && key.UnicodeChar < '1' + gguf_count) {
                selected_idx = key.UnicodeChar - '1';
                CHAR16 selection_char[2];
                selection_char[0] = key.UnicodeChar;
                selection_char[1] = '\0';
                print_str(SystemTable, selection_char);
                print_str(SystemTable, L"\r\n");
            }
        }
        bs->Stall(10000); // 10ms delay to avoid CPU burning
    }

    // 3. Load Selected GGUF Model
    print_str(SystemTable, L"\r\nLoading model: ");
    print_str(SystemTable, gguf_files[selected_idx]);
    print_str(SystemTable, L"\r\n");

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* model_fs = NULL;
    bs->HandleProtocol(gguf_fs_handles[selected_idx], (EFI_GUID*)&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void**)&model_fs);
    
    EFI_FILE_PROTOCOL* model_root = NULL;
    model_fs->OpenVolume(model_fs, &model_root);

    EFI_FILE_PROTOCOL* model_file = NULL;
    status = model_root->Open(model_root, &model_file, gguf_files[selected_idx], EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        print_str(SystemTable, L"Error: Failed to open model file!\r\n");
        return status;
    }

    // Get model file size
    model_file->SetPosition(model_file, 0xFFFFFFFFFFFFFFFFULL);
    uint64_t model_size = 0;
    model_file->GetPosition(model_file, &model_size);
    model_file->SetPosition(model_file, 0);

    print_str(SystemTable, L"Size: ");
    print_uint(SystemTable, model_size);
    print_str(SystemTable, L" bytes\r\n");

    UINTN model_pages = (model_size + 4095) / 4096;
    uint64_t model_phys_addr = 0;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, model_pages, &model_phys_addr);
    if (status != EFI_SUCCESS) {
        print_str(SystemTable, L"Error: Failed to allocate pages for model!\r\n");
        return status;
    }

    // Read model file in 32MB chunks
    print_str(SystemTable, L"Reading model data [");
    uint64_t bytes_left = model_size;
    uint8_t* write_ptr = (uint8_t*)model_phys_addr;
    while (bytes_left > 0) {
        UINTN chunk_size = (bytes_left > 32 * 1024 * 1024) ? 32 * 1024 * 1024 : bytes_left;
        status = model_file->Read(model_file, &chunk_size, write_ptr);
        if (status != EFI_SUCCESS || chunk_size == 0) {
            print_str(SystemTable, L"x Error reading file!\r\n");
            return status;
        }
        bytes_left -= chunk_size;
        write_ptr += chunk_size;
        print_str(SystemTable, L".");
    }
    print_str(SystemTable, L"] Done!\r\n");
    model_file->Close(model_file);

    boot_info.model_addr = model_phys_addr;
    boot_info.model_size = model_size;

    // 4. Try loading companion .instruct file
    static CHAR16 instruct_name[256];
    UINTN name_len = 0;
    while (gguf_files[selected_idx][name_len]) {
        instruct_name[name_len] = gguf_files[selected_idx][name_len];
        name_len++;
    }
    instruct_name[name_len] = L'\0';
    // Replace .gguf with .instruct (length is 4 for .gguf, length is 9 for .instruct)
    if (name_len > 5) {
        instruct_name[name_len-4] = 'i';
        instruct_name[name_len-3] = 'n';
        instruct_name[name_len-2] = 's';
        instruct_name[name_len-1] = 't';
        instruct_name[name_len]   = 'r';
        instruct_name[name_len+1] = 'u';
        instruct_name[name_len+2] = 'c';
        instruct_name[name_len+3] = 't';
        instruct_name[name_len+4] = '\0';
    }

    EFI_FILE_PROTOCOL* inst_file = NULL;
    status = model_root->Open(model_root, &inst_file, instruct_name, EFI_FILE_MODE_READ, 0);
    if (status == EFI_SUCCESS) {
        print_str(SystemTable, L"Loading companion instructions...\r\n");
        inst_file->SetPosition(inst_file, 0xFFFFFFFFFFFFFFFFULL);
        uint64_t inst_size = 0;
        inst_file->GetPosition(inst_file, &inst_size);
        inst_file->SetPosition(inst_file, 0);

        UINTN inst_pages = (inst_size + 4095) / 4096;
        uint64_t inst_phys_addr = 0;
        status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, inst_pages, &inst_phys_addr);
        if (status == EFI_SUCCESS) {
            UINTN read_size = inst_size;
            status = inst_file->Read(inst_file, &read_size, (void*)inst_phys_addr);
            if (status == EFI_SUCCESS) {
                boot_info.instruct_addr = inst_phys_addr;
                boot_info.instruct_size = inst_size;
                print_str(SystemTable, L"Instructions loaded successfully.\r\n");
            }
        }
        inst_file->Close(inst_file);
    }
    model_root->Close(model_root);

    // 5. Load Kernel from Boot Partition
    print_str(SystemTable, L"\r\nLoading kernel: /boot/kernel.bin\r\n");
    
    // Find the boot filesystem device handle
    EFI_LOADED_IMAGE_PROTOCOL* loaded_image = NULL;
    status = bs->HandleProtocol(ImageHandle, (EFI_GUID*)&EFI_LOADED_IMAGE_PROTOCOL_GUID, (void**)&loaded_image);
    if (status != EFI_SUCCESS) {
        print_str(SystemTable, L"Error: Failed to locate LoadedImageProtocol!\r\n");
        return status;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* boot_fs = NULL;
    status = bs->HandleProtocol(loaded_image->DeviceHandle, (EFI_GUID*)&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void**)&boot_fs);
    if (status != EFI_SUCCESS) {
        print_str(SystemTable, L"Error: Failed to locate SimpleFileSystem on boot device!\r\n");
        return status;
    }

    EFI_FILE_PROTOCOL* boot_root = NULL;
    status = boot_fs->OpenVolume(boot_fs, &boot_root);
    if (status != EFI_SUCCESS) {
        print_str(SystemTable, L"Error: Failed to open volume on boot device!\r\n");
        return status;
    }

    EFI_FILE_PROTOCOL* kern_file = NULL;
    status = boot_root->Open(boot_root, &kern_file, L"\\boot\\kernel.bin", EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS) {
        // Try relative path
        status = boot_root->Open(boot_root, &kern_file, L"boot\\kernel.bin", EFI_FILE_MODE_READ, 0);
    }
    if (status != EFI_SUCCESS) {
        // Try direct root path
        status = boot_root->Open(boot_root, &kern_file, L"kernel.bin", EFI_FILE_MODE_READ, 0);
    }

    if (status != EFI_SUCCESS) {
        print_str(SystemTable, L"Error: Failed to open /boot/kernel.bin!\r\n");
        return status;
    }

    // Read ELF header
    Elf32_Ehdr ehdr;
    UINTN ehdr_size = sizeof(Elf32_Ehdr);
    status = kern_file->Read(kern_file, &ehdr_size, &ehdr);
    if (status != EFI_SUCCESS) {
        print_str(SystemTable, L"Error: Failed to read kernel ELF header!\r\n");
        return status;
    }

    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
        print_str(SystemTable, L"Error: /boot/kernel.bin is not a valid ELF binary!\r\n");
        return EFI_NOT_FOUND;
    }

    // Read Program Headers
    static Elf32_Phdr phdr[32];
    UINTN phdr_bytes = ehdr.e_phnum * sizeof(Elf32_Phdr);
    status = kern_file->SetPosition(kern_file, ehdr.e_phoff);
    status = kern_file->Read(kern_file, &phdr_bytes, phdr);
    if (status != EFI_SUCCESS) {
        print_str(SystemTable, L"Error: Failed to read kernel program headers!\r\n");
        return status;
    }

    // Copy ELF segments to physical addresses
    // We pre-allocate pages at physical address 0x100000 (1MB) to 0x1100000 (17MB) to hold the kernel code
    uint64_t reserve_addr = 0x100000;
    UINTN reserve_pages = 0x1000; // 16MB
    bs->AllocatePages(AllocateAddress, EfiLoaderCode, reserve_pages, &reserve_addr);

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdr[i].p_type == 1) { // PT_LOAD
            print_str(SystemTable, L"  PT_LOAD: Segment to physical address: ");
            print_uint(SystemTable, phdr[i].p_paddr);
            print_str(SystemTable, L"\r\n");

            status = kern_file->SetPosition(kern_file, phdr[i].p_offset);
            UINTN segment_size = phdr[i].p_filesz;
            status = kern_file->Read(kern_file, &segment_size, (void*)(uint64_t)phdr[i].p_paddr);
            if (status != EFI_SUCCESS) {
                print_str(SystemTable, L"Error copying ELF segment!\r\n");
                return status;
            }

            // Zero out BSS
            if (phdr[i].p_memsz > phdr[i].p_filesz) {
                uint8_t* bss_ptr = (uint8_t*)(uint64_t)(phdr[i].p_paddr + phdr[i].p_filesz);
                UINTN bss_len = phdr[i].p_memsz - phdr[i].p_filesz;
                for (UINTN j = 0; j < bss_len; j++) {
                    bss_ptr[j] = 0;
                }
            }
        }
    }
    kern_file->Close(kern_file);
    boot_root->Close(boot_root);

    // 6. Graphics Output Protocol (GOP)
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    status = bs->LocateProtocol((EFI_GUID*)&EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, NULL, (void**)&gop);
    if (status == EFI_SUCCESS && gop->Mode && gop->Mode->Info) {
        boot_info.framebuffer_addr = gop->Mode->FrameBufferBase;
        boot_info.framebuffer_width = gop->Mode->Info->HorizontalResolution;
        boot_info.framebuffer_height = gop->Mode->Info->VerticalResolution;
        boot_info.framebuffer_pitch = gop->Mode->Info->PixelsPerScanLine * 4; // Assuming 32-bpp (4 bytes per pixel)
        boot_info.framebuffer_bpp = 32;
        
        print_str(SystemTable, L"GOP: Framebuffer mapped at ");
        print_uint(SystemTable, boot_info.framebuffer_addr);
        print_str(SystemTable, L" (");
        print_uint(SystemTable, boot_info.framebuffer_width);
        print_str(SystemTable, L"x");
        print_uint(SystemTable, boot_info.framebuffer_height);
        print_str(SystemTable, L")\r\n");
    } else {
        print_str(SystemTable, L"Warning: GOP graphics not available!\r\n");
    }

    // 7. Get Memory Map
    UINTN map_size = 0;
    EFI_MEMORY_DESCRIPTOR* mmap = NULL;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    uint32_t desc_ver = 0;

    // Get size first
    bs->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_ver);
    map_size += 4096; // add buffer padding

    uint64_t mmap_phys_addr = 0;
    UINTN mmap_pages = (map_size + 4095) / 4096;
    status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, mmap_pages, &mmap_phys_addr);
    if (status != EFI_SUCCESS) {
        print_str(SystemTable, L"Error: Failed to allocate memory for UEFI memory map!\r\n");
        return status;
    }

    mmap = (EFI_MEMORY_DESCRIPTOR*)mmap_phys_addr;
    status = bs->GetMemoryMap(&map_size, mmap, &map_key, &desc_size, &desc_ver);
    if (status != EFI_SUCCESS) {
        print_str(SystemTable, L"Error: Failed to retrieve UEFI memory map!\r\n");
        return status;
    }

    // Parse memory map
    int entry_count = 0;
    UINTN num_entries = map_size / desc_size;
    for (UINTN i = 0; i < num_entries && entry_count < 128; i++) {
        EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)((uint8_t*)mmap + i * desc_size);
        
        uint32_t type = 0; // standard: 0 = Reserved
        if (desc->Type == EfiConventionalMemory || 
            desc->Type == EfiLoaderCode || 
            desc->Type == EfiLoaderData ||
            desc->Type == EfiBootServicesCode || 
            desc->Type == EfiBootServicesData) {
            type = 1; // Usable RAM
        }
        
        boot_info.mmap_entries[entry_count].phys_addr = desc->PhysicalStart;
        boot_info.mmap_entries[entry_count].num_bytes = desc->NumberOfPages * 4096;
        boot_info.mmap_entries[entry_count].type = type;
        entry_count++;
    }
    boot_info.mmap_entries_count = entry_count;

    print_str(SystemTable, L"Memory Map: Parsed ");
    print_uint(SystemTable, entry_count);
    print_str(SystemTable, L" entries.\r\n");

    // 8. Exit Boot Services & Boot Kernel!
    print_str(SystemTable, L"Exiting Boot Services...\r\n");
    
    // We retry ExitBootServices if it fails (due to memory map changes from print/allocations)
    int retries = 3;
    while (retries > 0) {
        status = bs->ExitBootServices(ImageHandle, map_key);
        if (status == EFI_SUCCESS) {
            break;
        }
        
        // Re-fetch memory map on failure
        map_size = mmap_pages * 4096;
        bs->GetMemoryMap(&map_size, mmap, &map_key, &desc_size, &desc_ver);
        retries--;
    }

    if (status != EFI_SUCCESS) {
        // This is back in EFI console since ExitBootServices failed
        print_str(SystemTable, L"Error: ExitBootServices failed!\r\n");
        return status;
    }

    // 9. Jump to the Kernel
    // Pass custom magic 0xAE105E1F and physical address of boot_info structure
    // e_entry corresponds to 0x100000 (_start)
    efi_jmp_to_kernel(ehdr.e_entry, 0xAE105E1F, (uint32_t)(uint64_t)&boot_info);

    // Halt if we return (which should be impossible)
    while(1) {
        __asm__ volatile ("hlt");
    }

    return EFI_SUCCESS;
}
