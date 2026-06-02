#include "shell.h"
#include "keyboard.h"
#include "serial.h"
#include "tar.h"
#include "sysinfo.h"
#include "gguf.h"
#include "malloc.h"
#include "io.h"
#include "igpu.h"
#include "efi.h"

extern uint32_t g_boot_magic;
extern uint32_t g_boot_info_addr;

static char shell_get_char(void);

static void shell_token_callback(const char* token) {
    serial_printf("%s", token);
}

static void get_formatted_prompt(const char* raw_prompt, char* formatted, int max_len) {
    size_t inst_size = 0;
    char inst_name[100];
    const char* inst_data = (const char*)tar_find_by_suffix(".instruct", inst_name, &inst_size);
    
    const char* system_prompt = "You are an AI operating system assistant. Output answers clearly and concisely.";
    char system_buf[512];
    
    if (inst_data && inst_size > 0) {
        int copy_len = (inst_size < 511) ? inst_size : 511;
        memcpy(system_buf, inst_data, copy_len);
        system_buf[copy_len] = '\0';
        
        // Remove trailing newlines/spaces
        while (copy_len > 0 && (system_buf[copy_len - 1] == '\n' || system_buf[copy_len - 1] == '\r' || system_buf[copy_len - 1] == ' ')) {
            system_buf[--copy_len] = '\0';
        }
        
        char* start = system_buf;
        if (strncmp(start, "System: ", 8) == 0) {
            start += 8;
        } else if (strncmp(start, "system: ", 8) == 0) {
            start += 8;
        }
        system_prompt = start;
    }
    
    int len = 0;
    const char* sys_start = "<|im_start|>system\n";
    int sys_start_len = strlen(sys_start);
    memcpy(formatted + len, sys_start, sys_start_len);
    len += sys_start_len;
    
    int sys_prompt_len = strlen(system_prompt);
    if (len + sys_prompt_len < max_len) {
        memcpy(formatted + len, system_prompt, sys_prompt_len);
        len += sys_prompt_len;
    }
    
    const char* sys_end = "\n<|im_end|>\n<|im_start|>user\n";
    int sys_end_len = strlen(sys_end);
    if (len + sys_end_len < max_len) {
        memcpy(formatted + len, sys_end, sys_end_len);
        len += sys_end_len;
    }
    
    int user_prompt_len = strlen(raw_prompt);
    if (len + user_prompt_len < max_len) {
        memcpy(formatted + len, raw_prompt, user_prompt_len);
        len += user_prompt_len;
    }
    
    const char* assist_start = "\n<|im_end|>\n<|im_start|>assistant\n";
    int assist_start_len = strlen(assist_start);
    if (len + assist_start_len < max_len) {
        memcpy(formatted + len, assist_start, assist_start_len);
        len += assist_start_len;
    }
    formatted[len] = '\0';
}

static void run_llm(const char* prompt) {
    char formatted_prompt[2048];
    get_formatted_prompt(prompt, formatted_prompt, 2048);
    
    serial_printf("Running inference...\n");
    gguf_generate(formatted_prompt, 100, shell_token_callback);
    serial_printf("\n");
}

static void run_igpu_test(void) {
    serial_printf("iGPU: Starting diagnostic tests...\n");
    serial_printf("iGPU: Hardware present: %s\n", igpu_has_hardware() ? "YES" : "NO (AP Emulation active)");
    
    void* w_page = pmm_alloc_page();
    void* control_page = pmm_alloc_page();
    if (!w_page || !control_page) {
        serial_printf("iGPU Test Error: Failed to allocate pages for buffers!\n");
        if (w_page) pmm_free_page(w_page);
        if (control_page) pmm_free_page(control_page);
        return;
    }
    
    void* w_buf = w_page;
    float* i_buf = (float*)control_page;
    float* o_buf = (float*)((uint8_t*)control_page + 4096);
    struct igpu_gate* gate = (struct igpu_gate*)((uint8_t*)control_page + 8192);
    
    struct test_block_q8_0 {
        uint16_t d;
        int8_t qs[32];
    } __attribute__((packed))* blocks = (struct test_block_q8_0*)w_buf;
    
    for (int r = 0; r < 4; r++) {
        blocks[r].d = 0x3C00; // 1.0f in FP16
        for (int c = 0; c < 32; c++) {
            blocks[r].qs[c] = (int8_t)(c + 1); // 1, 2, ..., 32
        }
    }
    
    for (int c = 0; c < 32; c++) {
        i_buf[c] = 1.0f;
    }
    
    for (int r = 0; r < 4; r++) {
        o_buf[r] = 0.0f;
    }
    
    gate->owner = IGPU_OWNER_CPU;
    gate->status = IGPU_STATUS_IDLE;
    gate->weights_buffer = w_buf;
    gate->weights_size = PAGE_SIZE_2MB; // Lock the entire 2MB weights page
    gate->input_buffer = i_buf;
    gate->input_size = 0;              // Do not lock the control page containing the input
    gate->output_buffer = o_buf;
    gate->output_size = 4096;
    gate->rows = 4;
    gate->cols = 32;
    gate->quant_type = 8; // Q8_0
    
    serial_printf("iGPU: Launching computation (4x32 matrix * 32 vector)...\n");
    
    igpu_submit_math(gate);
    
    serial_printf("\n--- Safety Lock Test ---\n");
    serial_printf("The UMA memory pages are now LOCKED in the CPU page tables.\n");
    serial_printf("Do you want to inject a CPU access conflict to test page-table protection? (y/n): ");
    char answer = shell_get_char();
    serial_printf("%c\n", answer);
    
    if (answer == 'y' || answer == 'Y') {
        serial_printf("Injecting fault: Reading from locked weights buffer at %p...\n", w_buf);
        volatile int8_t val = blocks[0].qs[0];
        (void)val;
        serial_printf("ERROR: Fault injection failed! CPU was able to read locked memory.\n");
    } else {
        serial_printf("Skipping page fault safety test. Waiting for computation completion...\n");
    }
    
    igpu_wait_math(gate);
    
    serial_printf("iGPU: Computation completed. Validating results:\n");
    int success = 1;
    for (int r = 0; r < 4; r++) {
        serial_printf("  Row %d: Output = %d.0 (Expected: 528.0)\n", r, (int)o_buf[r]);
        if (o_buf[r] != 528.0f) {
            success = 0;
        }
    }
    
    if (success) {
        serial_printf("iGPU Diagnostic Result: SUCCESS!\n");
    } else {
        serial_printf("iGPU Diagnostic Result: FAILURE (Output math incorrect).\n");
    }
    
    pmm_free_page(w_page);
    pmm_free_page(control_page);
}


static char shell_get_char(void) {
    while (1) {
        if (keyboard_has_char()) {
            return keyboard_get_char();
        }
        if (serial_received()) {
            char c = serial_read();
            if (c == (char)0xFF) {
                // Drop phantom serial reads on laptops without COM ports
                __asm__ volatile ("pause");
                continue;
            }
            if (c == 127 || c == '\b') return '\b'; // handle backspace / delete
            if (c == '\r') return '\n';
            return c;
        }
        __asm__ volatile ("pause");
    }
}

void shell_start(void) {
    char line[256];
    int idx = 0;
    
    serial_printf("\nWelcome to aiOS! (Type 'help' for command list)\n");
    
    while (1) {
        serial_printf("aiOS> ");
        idx = 0;
        
        while (1) {
            char c = shell_get_char();
            
            if (c == '\n') {
                line[idx] = '\0';
                serial_printf("\n");
                break;
            } else if (c == '\b') {
                if (idx > 0) {
                    idx--;
                    serial_printf("\b \b");
                }
            } else if (idx < 255 && c >= 32 && c <= 126) {
                line[idx++] = c;
                serial_write_char(c); // prints to serial and VGA
            }
        }
        
        if (idx == 0) continue;
        
        // Command parsing
        if (strcmp(line, "help") == 0) {
            serial_printf("Available Commands:\n");
            serial_printf("  help                      - Show this menu\n");
            serial_printf("  ls                        - List files in initrd RAM disk\n");
            serial_printf("  cat <filename>            - Print contents of file\n");
            serial_printf("  sysinfo                   - Run CPUID, PCI scan, and ACPI table listing\n");
            serial_printf("  llm <prompt>              - Execute LLM locally on the CPU\n");
            serial_printf("  llmstrap                  - Show details of loaded LLM model\n");
            serial_printf("  drivergen <vendor> <dev>  - Generate PCI device driver template\n");
            serial_printf("  shutdown                  - Shutdown the operating system\n");
            serial_printf("  igputest                  - Run iGPU shared memory gate diagnostics\n");
        } else if (strcmp(line, "ls") == 0) {
            tar_list();
        } else if (strncmp(line, "cat ", 4) == 0) {
            const char* filename = line + 4;
            size_t size = 0;
            const char* data = (const char*)tar_find_file(filename, &size);
            if (data) {
                for (size_t i = 0; i < size; i++) {
                    serial_write_char(data[i]);
                }
                serial_printf("\n");
            } else {
                serial_printf("File not found: %s\n", filename);
            }
        } else if (strcmp(line, "sysinfo") == 0) {
            sysinfo_print_cpu();
            sysinfo_print_acpi();
            sysinfo_print_pci();
        } else if (strncmp(line, "llm ", 4) == 0) {
            run_llm(line + 4);
        } else if (strncmp(line, "drivergen ", 10) == 0) {
            char prompt[512];
            // Format prompt: "Write a short PCI driver template in C for Vendor ID [vendor], Device ID [device]"
            // E.g. drivergen 0x1002 0x15e3
            // We just parse the args
            const char* args = line + 10;
            // Let's create the prompt directly
            int p_idx = 0;
            const char* prefix = "Write a short C driver skeleton for a PCI device: ";
            int prefix_len = strlen(prefix);
            memcpy(prompt, prefix, prefix_len);
            p_idx += prefix_len;
            
            int args_len = strlen(args);
            if (p_idx + args_len + 32 < 512) {
                memcpy(prompt + p_idx, args, args_len);
                p_idx += args_len;
                const char* suffix = ". Include read/write helpers.";
                memcpy(prompt + p_idx, suffix, strlen(suffix));
                p_idx += strlen(suffix);
                prompt[p_idx] = '\0';
                
                run_llm(prompt);
            }
        } else if (strcmp(line, "igputest") == 0) {
            run_igpu_test();
        } else if (strcmp(line, "llmstrap") == 0) {
            if (g_boot_magic == 0xAE105E1F) {
                struct aios_boot_info* info = (struct aios_boot_info*)(uint64_t)g_boot_info_addr;
                serial_printf("llmstrap: Operating under UEFI Bootloader Mode.\n");
                if (info && info->model_addr && info->model_size > 0) {
                    serial_printf("  - Active Model Address: %p\n", (void*)info->model_addr);
                    serial_printf("  - Active Model Size: %llu bytes\n", info->model_size);
                    if (info->instruct_addr) {
                        serial_printf("  - Active Companion Instructions Address: %p (%llu bytes)\n",
                                      (void*)info->instruct_addr, info->instruct_size);
                    } else {
                        serial_printf("  - Active Companion Instructions: None (Default loaded)\n");
                    }
                    serial_printf("\nNote: Models are loaded and locked at boot time using UEFI services.\n");
                    serial_printf("To change the active model, reboot and select a different option from the UEFI menu.\n");
                } else {
                    serial_printf("  - No model was loaded by UEFI at boot time.\n");
                }
            } else {
                serial_printf("llmstrap: Operating under Legacy GRUB/Multiboot Mode.\n");
                serial_printf("  - Models are loaded contiguous via initrd.tar RAM disk.\n");
                serial_printf("  - Size limit: ~1GB (below 4GB limit).\n");
                serial_printf("  - Active GGUF: parsed from initrd.\n");
            }
        } else if (strcmp(line, "shutdown") == 0) {
            serial_printf("Are you sure you want to shutdown? (y/n): ");
            char answer = shell_get_char();
            serial_printf("%c\n", answer);
            if (answer == 'y' || answer == 'Y') {
                serial_printf("Shutting down aiOS...\n");
                sysinfo_poweroff();
                
                // If it fails (running on real hardware without supported ACPI), halt/reboot
                serial_printf("Shutdown signal sent. You can safely power off your PC.\n");
                serial_printf("Press 'r' to reboot, or any other key to halt.\n");
                char choice = shell_get_char();
                if (choice == 'r' || choice == 'R') {
                    serial_printf("Rebooting...\n");
                    // Trigger a triple fault to reboot
                    struct {
                        uint16_t limit;
                        uint64_t base;
                    } __attribute__((packed)) null_idtr = { 0, 0 };
                    __asm__ volatile ("lidt %0; int $3" : : "m"(null_idtr));
                }
                while (1) {
                    __asm__ volatile ("hlt");
                }
            } else {
                serial_printf("Shutdown aborted.\n");
            }
        } else {
            serial_printf("Unknown command: %s (Type 'help' for options)\n", line);
        }
    }
}
