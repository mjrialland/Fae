#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

void serial_init(void);
int serial_received(void);
char serial_read(void);
void serial_write_char(char a);
void serial_write(const char* str);
void serial_printf(const char* format, ...);
void serial_set_vga_visible(int visible);
void serial_init_framebuffer(uint64_t addr, uint32_t pitch, uint32_t width, uint32_t height, uint8_t bpp);

#endif
