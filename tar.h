#ifndef TAR_H
#define TAR_H

#include <stdint.h>
#include <stddef.h>

void tar_init(void* tar_address, size_t tar_size);
void tar_list(void);
void* tar_find_file(const char* filename, size_t* out_size);
void* tar_find_by_suffix(const char* suffix, char* out_name, size_t* out_size);

#endif
