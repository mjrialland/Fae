#include "serial.h"
#include "io.h"
#include <stdarg.h>

#define PORT 0x3F8          // COM1

// VGA console parameters
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
static uint16_t* vga_buffer = (uint16_t*)0xB8000;
static int vga_col = 0;
static int vga_row = 0;
static uint8_t vga_color = 0x0F; // White on black

#include "font8x8.h"

static uint32_t* fb_addr = NULL;
static uint32_t fb_pitch = 0;
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint8_t fb_bpp = 0;
static uint32_t fb_char_col = 0;
static uint32_t fb_char_row = 0;

void serial_init_framebuffer(uint64_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp) {
    fb_addr = (uint32_t*)addr;
    fb_pitch = pitch;
    fb_width = width;
    fb_height = height;
    fb_bpp = bpp;
    
    fb_char_col = 0;
    fb_char_row = 0;
    
    if (fb_addr) {
        vga_buffer = (uint16_t*)addr;
    }
    
    // Clear screen
    if (fb_addr && fb_bpp == 32) {
        uint32_t stride = fb_pitch / 4;
        for (uint32_t y = 0; y < fb_height; y++) {
            for (uint32_t x = 0; x < fb_width; x++) {
                fb_addr[y * stride + x] = 0x00000000;
            }
        }
    }
}

static void fb_scroll(void) {
    if (!fb_addr) return;
    
    uint64_t* dest = (uint64_t*)fb_addr;
    uint64_t* src = (uint64_t*)((uint8_t*)fb_addr + 8 * fb_pitch);
    uint64_t qwords_to_move = ((fb_height - 8) * (uint64_t)fb_pitch) / 8;
    
    for (uint64_t i = 0; i < qwords_to_move; i++) {
        dest[i] = src[i];
    }
    
    uint64_t* clear_dest = (uint64_t*)((uint8_t*)fb_addr + (fb_height - 8) * (uint64_t)fb_pitch);
    uint64_t clear_qwords = fb_pitch; // (8 * fb_pitch) / 8
    for (uint64_t i = 0; i < clear_qwords; i++) {
        clear_dest[i] = 0;
    }
    
    fb_char_row = (fb_height / 8) - 1;
}

static void fb_put_char(char c) {
    if (!fb_addr) return;
    
    if (c == '\n') {
        fb_char_col = 0;
        fb_char_row++;
        if (fb_char_row >= fb_height / 8) {
            fb_scroll();
        }
        return;
    } else if (c == '\r') {
        fb_char_col = 0;
        return;
    } else if (c == '\b') {
        if (fb_char_col > 0) {
            fb_char_col--;
            uint32_t px = fb_char_col * 8;
            uint32_t py = fb_char_row * 8;
            uint32_t stride = fb_pitch / 4;
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    fb_addr[(py + y) * stride + (px + x)] = 0x00000000;
                }
            }
        }
        return;
    }
    
    uint32_t px = fb_char_col * 8;
    uint32_t py = fb_char_row * 8;
    uint32_t stride = fb_pitch / 4;
    
    const uint8_t* glyph = font8x8_basic[(unsigned char)c & 0x7F];
    for (int y = 0; y < 8; y++) {
        uint8_t row_data = glyph[y];
        for (int x = 0; x < 8; x++) {
            if (row_data & (1 << x)) {
                fb_addr[(py + y) * stride + (px + x)] = 0x00FFFFFF; // White
            } else {
                fb_addr[(py + y) * stride + (px + x)] = 0x00000000; // Black
            }
        }
    }
    
    fb_char_col++;
    if (fb_char_col >= fb_width / 8) {
        fb_char_col = 0;
        fb_char_row++;
        if (fb_char_row >= fb_height / 8) {
            fb_scroll();
        }
    }
}

static void vga_scroll(void) {
    for (int y = 0; y < VGA_HEIGHT - 1; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[y * VGA_WIDTH + x] = vga_buffer[(y + 1) * VGA_WIDTH + x];
        }
    }
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = (uint16_t)vga_color << 8 | ' ';
    }
    vga_row = VGA_HEIGHT - 1;
}

void vga_clear(void) {
    if (fb_addr && fb_bpp == 32) {
        uint32_t stride = fb_pitch / 4;
        for (uint32_t y = 0; y < fb_height; y++) {
            for (uint32_t x = 0; x < fb_width; x++) {
                fb_addr[y * stride + x] = 0x00000000;
            }
        }
        fb_char_col = 0;
        fb_char_row = 0;
        return;
    }
    
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[y * VGA_WIDTH + x] = (uint16_t)vga_color << 8 | ' ';
        }
    }
    vga_col = 0;
    vga_row = 0;
}

static int vga_hidden = 0;

void serial_set_vga_visible(int visible) {
    vga_hidden = !visible;
}

void vga_put_char(char c) {
    if (vga_hidden) return;
    
    if (fb_addr) {
        fb_put_char(c);
        return;
    }
    
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_HEIGHT) {
            vga_scroll();
        }
        return;
    } else if (c == '\r') {
        vga_col = 0;
        return;
    } else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            vga_buffer[vga_row * VGA_WIDTH + vga_col] = (uint16_t)vga_color << 8 | ' ';
        }
        return;
    }
    
    vga_buffer[vga_row * VGA_WIDTH + vga_col] = (uint16_t)vga_color << 8 | c;
    vga_col++;
    if (vga_col >= VGA_WIDTH) {
        vga_col = 0;
        vga_row++;
        if (vga_row >= VGA_HEIGHT) {
            vga_scroll();
        }
    }
}

void serial_init(void) {
    outb(PORT + 1, 0x00);    // Disable all interrupts
    outb(PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(PORT + 1, 0x00);    //                  (hi byte)
    outb(PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
    vga_clear();
}

int serial_received(void) {
    return inb(PORT + 5) & 1;
}

char serial_read(void) {
    while (serial_received() == 0);
    return inb(PORT);
}

static int is_transmit_empty(void) {
    return inb(PORT + 5) & 0x20;
}

void serial_write_char(char a) {
    volatile uint32_t timeout = 50000;
    
    while (is_transmit_empty() == 0) {
        if (--timeout == 0) {
            // Fallback for real hardware: bypass UART write but still print to screen
            vga_put_char(a); 
            return;
        }
        __asm__ volatile ("pause");
    }
    
    outb(PORT, a);
    vga_put_char(a); 
}

void serial_write(const char* str) {
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\n') {
            serial_write_char('\r');
        }
        serial_write_char(str[i]);
    }
}

static void print_uint(unsigned long long val, int base, int width, char pad) {
    char buf[64];
    int idx = 0;
    const char* chars = "0123456789abcdef";
    do {
        buf[idx++] = chars[val % base];
        val /= base;
    } while (val > 0);

    while (idx < width) {
        serial_write_char(pad);
        width--;
    }

    while (idx > 0) {
        serial_write_char(buf[--idx]);
    }
}

static void print_int(long long val, int base) {
    if (val < 0) {
        serial_write_char('-');
        val = -val;
    }
    print_uint(val, base, 0, ' ');
}

void serial_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    for (int i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%') {
            i++;
            int width = 0;
            char pad = ' ';
            if (format[i] == '0') {
                pad = '0';
                i++;
            }
            while (format[i] >= '0' && format[i] <= '9') {
                width = width * 10 + (format[i] - '0');
                i++;
            }

            char next = format[i];
            if (next == 'c') {
                char val = (char)va_arg(args, int);
                serial_write_char(val);
            } else if (next == 's') {
                const char* val = va_arg(args, const char*);
                if (!val) val = "(null)";
                serial_write(val);
            } else if (next == 'd') {
                int val = va_arg(args, int);
                print_int(val, 10);
            } else if (next == 'u') {
                unsigned int val = va_arg(args, unsigned int);
                print_uint(val, 10, width, pad);
            } else if (next == 'x') {
                unsigned int val = va_arg(args, unsigned int);
                print_uint(val, 16, width, pad);
            } else if (next == 'p') {
                void* val = va_arg(args, void*);
                serial_write("0x");
                print_uint((unsigned long long)val, 16, width, pad);
            } else if (next == 'l') {
                i++;
                if (format[i] == 'l') {
                    i++;
                    if (format[i] == 'd') {
                        long long val = va_arg(args, long long);
                        print_int(val, 10);
                    } else if (format[i] == 'u') {
                        unsigned long long val = va_arg(args, unsigned long long);
                        print_uint(val, 10, width, pad);
                    } else if (format[i] == 'x') {
                        unsigned long long val = va_arg(args, unsigned long long);
                        print_uint(val, 16, width, pad);
                    }
                } else if (format[i] == 'd') {
                    long val = va_arg(args, long);
                    print_int(val, 10);
                } else if (format[i] == 'u') {
                    unsigned long val = va_arg(args, unsigned long);
                    print_uint(val, 10, width, pad);
                } else if (format[i] == 'x') {
                    unsigned long val = va_arg(args, unsigned long);
                    print_uint(val, 16, width, pad);
                }
            } else {
                serial_write_char('%');
                serial_write_char(next);
            }
        } else {
            if (format[i] == '\n') {
                serial_write_char('\r');
            }
            serial_write_char(format[i]);
        }
    }

    va_end(args);
}
