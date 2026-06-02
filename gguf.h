#ifndef GGUF_H
#define GGUF_H

#include <stdint.h>
#include <stddef.h>

int gguf_init(void* model_buffer, size_t model_size);
const char* gguf_get_chat_template(void);
void gguf_generate(const char* prompt, int max_tokens, void (*token_callback)(const char* token));

float dot_product_q8_0_f32(const void* row_q8, const float* vec_f32, int num_elements);
float dot_product_q4_0_f32(const void* row_q4, const float* vec_f32, int num_elements);

// Multiprocessing helpers
void ap_main(uint64_t core_id);
extern volatile int total_cores;

#endif
