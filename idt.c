#include "idt.h"
#include "io.h"
#include "serial.h"
#include "igpu.h"


struct idt_entry idt[256];
struct idt_ptr idtp;

// Declared in boot.asm
extern void isr33(void);
extern void isr13(void);
extern void isr14(void);

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low = (uint16_t)(base & 0xFFFF);
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].type_attr = flags;
    idt[num].offset_mid = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt[num].zero = 0;
}

#define PIC1          0x20
#define PIC2          0xA0
#define PIC1_COMMAND  PIC1
#define PIC1_DATA     (PIC1+1)
#define PIC2_COMMAND  PIC2
#define PIC2_DATA     (PIC2+1)

static void pic_remap(void) {
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();
    
    outb(PIC1_DATA, 0x20); // Master PIC remapped to 0x20 (32)
    io_wait();
    outb(PIC2_DATA, 0x28); // Slave PIC remapped to 0x28 (40)
    io_wait();
    
    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();
    
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();
    
    // Mask all interrupts initially except keyboard (IRQ 1)
    // IRQ 0 is timer, IRQ 1 is keyboard
    outb(PIC1_DATA, 0xFD); // 0xFD = 11111101b (enable IRQ 1)
    io_wait();
    outb(PIC2_DATA, 0xFF);
    io_wait();
}

void idt_init(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (uint64_t)&idt;

    // Clear IDT
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    pic_remap();

    // 0x08 is kernel code segment
    // 0x8E: Present, Ring 0, 32-bit/64-bit Interrupt Gate
    idt_set_gate(13, (uint64_t)isr13, 0x08, 0x8E); // GPF
    idt_set_gate(14, (uint64_t)isr14, 0x08, 0x8E); // Page Fault
    idt_set_gate(33, (uint64_t)isr33, 0x08, 0x8E); // Keyboard IRQ 1

    // Load IDT
    __asm__ volatile ("lidt %0" : : "m"(idtp));
    
    // Enable interrupts
    __asm__ volatile ("sti");
}

void gpf_handler(uint64_t error_code, void* rsp) {
    serial_printf("\n!!! GENERAL PROTECTION FAULT !!!\n");
    serial_printf("Error Code: 0x%llx\n", error_code);
    serial_printf("RSP Frame: %p\n", rsp);
    while (1) {
        __asm__ volatile ("hlt");
    }
}

void pf_handler(uint64_t error_code, void* rsp) {
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    
    extern uint64_t pml4[];
    extern struct igpu_gate* volatile g_active_gate;
    
    uint64_t page = cr2 / (2 * 1024 * 1024);
    uint64_t pdt_idx = page / 512;
    uint64_t entry_idx = page % 512;
    uint64_t* pdpt = (uint64_t*)(pml4[0] & ~0xFFF);
    uint64_t* pdt = (uint64_t*)(pdpt[pdt_idx] & ~0xFFF);
    int is_locked = !(pdt[entry_idx] & 1);
    
    if (is_locked && g_active_gate != NULL) {
        uint64_t w_start = (uint64_t)g_active_gate->weights_buffer;
        uint64_t w_end = w_start + g_active_gate->weights_size;
        
        if (cr2 >= w_start && cr2 < w_end) {
            serial_printf("\n!!! iGPU HARDWARE ACCESS CONFLICT FAULT !!!\n");
            serial_printf("CPU attempted to access shared UMA memory at %p while locked by iGPU!\n", (void*)cr2);
            serial_printf("Error details: code=0x%llx, gate_status=%d, weights=%p (size=%d)\n", 
                          error_code, g_active_gate->status, g_active_gate->weights_buffer, g_active_gate->weights_size);
            while (1) {
                __asm__ volatile ("hlt");
            }
        }
    }
    
    serial_printf("\n!!! PAGE FAULT !!!\n");
    serial_printf("Accessed Address: %p\n", (void*)cr2);
    serial_printf("Error Code: 0x%llx (", error_code);
    if (error_code & 1) serial_printf("Present ");
    else serial_printf("Non-present ");
    if (error_code & 2) serial_printf("Write ");
    else serial_printf("Read ");
    if (error_code & 4) serial_printf("User ");
    else serial_printf("Supervisor ");
    if (error_code & 8) serial_printf("Reserved-write ");
    if (error_code & 16) serial_printf("Instruction-fetch ");
    serial_printf(")\n");
    serial_printf("RSP Frame: %p\n", rsp);
    while (1) {
        __asm__ volatile ("hlt");
    }
}
