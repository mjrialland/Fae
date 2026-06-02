#ifndef MALLOC_H
#define MALLOC_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE_2MB (2 * 1024 * 1024)

void pmm_init(uint64_t kernel_end, uint32_t magic, uint32_t multiboot_addr);
void* pmm_alloc_page(void);
void* pmm_alloc_contiguous(size_t count);
void pmm_free_page(void* ptr);

void malloc_init(void);
void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t num, size_t size);
void* realloc(void* ptr, size_t size);

void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
size_t strlen(const char* s);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);

#endif
