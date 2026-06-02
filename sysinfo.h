#ifndef SYSINFO_H
#define SYSINFO_H

#include <stdint.h>

void sysinfo_detect(uint32_t magic, uint32_t multiboot_addr);
void* sysinfo_find_multiboot2_tag(uint32_t multiboot_addr, uint32_t type);
void sysinfo_print_cpu(void);
void sysinfo_print_pci(void);
void sysinfo_print_acpi(void);

int sysinfo_get_core_count(void);
uint8_t sysinfo_get_lapic_id(int index);
void sysinfo_poweroff(void);

#endif

