#include "gguf.h"
#include "serial.h"
#include "malloc.h"
#include "igpu.h"
#include <immintrin.h>


// Math helper functions
#define PI 3.14159265358979323846

static double exp(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int i = 1; i < 20; i++) {
        term *= x / i;
        sum += term;
    }
    return sum;
}

static double log(double x) {
    if (x <= 0) return 0.0;
    double y = 0.0;
    while (x > 2.0) { x /= 2.718281828459045; y += 1.0; }
    while (x < 0.5) { x *= 2.718281828459045; y -= 1.0; }
    for (int iter = 0; iter < 10; iter++) {
        double ey = exp(y);
        y = y - 1.0 + x / ey;
    }
    return y;
}

static double pow(double base, double exponent) {
    if (base <= 0) return 0.0;
    return exp(exponent * log(base));
}

static double sin(double x) {
    // Wrap to [-PI, PI]
    while (x > PI) x -= 2.0 * PI;
    while (x < -PI) x += 2.0 * PI;
    double sum = x;
    double term = x;
    double x2 = x * x;
    for (int i = 3; i <= 15; i += 2) {
        term *= -x2 / (i * (i - 1));
        sum += term;
    }
    return sum;
}

static double cos(double x) {
    while (x > PI) x -= 2.0 * PI;
    while (x < -PI) x += 2.0 * PI;
    double sum = 1.0;
    double term = 1.0;
    double x2 = x * x;
    for (int i = 2; i <= 16; i += 2) {
        term *= -x2 / (i * (i - 1));
        sum += term;
    }
    return sum;
}

static double sqrt(double x) {
    if (x <= 0) return 0.0;
    double y = x;
    for (int i = 0; i < 10; i++) {
        y = 0.5 * (y + x / y);
    }
    return y;
}

static int key_match(const char* key, uint32_t key_len, const char* name) {
    if (strncmp(name, "llama.", 6) == 0) {
        const char* suffix = name + 6;
        int suff_len = strlen(suffix);
        if (key_len > (uint32_t)suff_len && key[key_len - suff_len - 1] == '.') {
            if (strncmp(key + key_len - suff_len, suffix, suff_len) == 0) {
                return 1;
            }
        }
    }
    if (strlen(name) != key_len) return 0;
    return strncmp(key, name, key_len) == 0;
}

// GGUF Types
struct gguf_header {
    char magic[4];
    uint32_t version;
    uint64_t tensor_count;
    uint64_t metadata_kv_count;
} __attribute__((packed));

struct gguf_tensor_info {
    const char* name;
    uint32_t name_len;
    uint32_t n_dims;
    uint64_t dims[4];
    uint32_t type;
    uint64_t offset;
    void* data;
};

// Vocab Structure
struct vocab_token {
    const char* str;
    uint32_t len;
    float score;
};

// Global GGUF Variables
static struct gguf_tensor_info tensors[512];
static int tensor_count = 0;

static struct vocab_token* vocab = NULL; // Dynamically allocated
static int vocab_size = 0;

#define HASH_SIZE 32768
struct vocab_hash_entry {
    int token_id;
    struct vocab_hash_entry* next;
};
static struct vocab_hash_entry* vocab_hash[HASH_SIZE];
static struct vocab_hash_entry* hash_entries = NULL;
static void build_vocab_hash_table(void);
static int is_sentencepiece = 0;
static int bos_token_id = 1;
static int eos_token_id = 2;
static int unk_token_id = 0;
static int add_bos_token = -1;

static uint32_t block_count = 0;
static uint32_t context_length = 0;
static uint32_t embedding_length = 0;
static uint32_t feed_forward_length = 0;
static uint32_t head_count = 0;
static uint32_t head_count_kv = 0;
static uint32_t head_dim = 0;        // Detected from tensor dims (Q/K/V head dimension)
static uint32_t head_dim_q = 0;      // Q head dimension (may differ from head_dim for QKNorm models)
static uint32_t q_hidden_dim = 0;    // Q projection output dimension
static uint32_t kv_hidden_dim = 0;   // K/V projection output dimension
static float layer_norm_rms_epsilon = 1e-5f;

// Model Weight Offsets/Pointers
struct layer_weights {
    void* attn_q;
    int type_q;
    void* attn_k;
    int type_k;
    void* attn_v;
    int type_v;
    void* attn_output;
    int type_output;
    float* attn_norm;
    // Qwen3-style Q/K normalization weights
    float* attn_q_norm;
    float* attn_k_norm;
    // Qwen2.5-style biases
    float* attn_q_bias;
    float* attn_k_bias;
    float* attn_v_bias;
    // FFN weights
    void* ffn_gate;
    int type_gate;
    void* ffn_up;
    int type_up;
    void* ffn_down;
    int type_down;
    float* ffn_norm;
};

struct model_weights {
    void* token_embd;
    int type_embd;
    void* output;
    int type_output;
    float* output_norm;
    struct layer_weights layers[64];
};

static struct model_weights model;

// Dynamic Buffers for Forward Pass
static float* d_x = NULL;
static float* d_x_norm = NULL;
static float* d_q = NULL;
static float* d_k = NULL;
static float* d_v = NULL;
static float* d_scores = NULL;
static float* d_attn_out = NULL;
static float* d_gate = NULL;
static float* d_up = NULL;
static float* d_ffn = NULL;
static float* d_logits = NULL;

static float* d_k_cache = NULL; // block_count * context_length * embedding_length
static float* d_v_cache = NULL;

// Quantized Quantization Structs
static inline float fp16_to_fp32(uint16_t h) {
    union { uint32_t u; float f; } w;
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t exp  = (h & 0x7C00) >> 10;
    uint32_t mant = (h & 0x03FF);
    
    if (exp == 0) {
        if (mant == 0) {
            w.u = sign;
            return w.f;
        }
        while ((mant & 0x0400) == 0) {
            mant <<= 1;
            exp--;
        }
        exp++;
        mant &= ~0x0400;
        w.u = sign | (((exp - 15 + 127) & 0xFF) << 23) | (mant << 13);
        return w.f;
    } else if (exp == 31) {
        w.u = sign | 0x7F800000 | (mant << 13);
        return w.f;
    }
    
    w.u = sign | (((exp - 15 + 127) & 0xFF) << 23) | (mant << 13);
    return w.f;
}

struct block_q8_0 {
    uint16_t d;
    int8_t qs[32];
} __attribute__((packed));

struct block_q4_0 {
    uint16_t d;
    uint8_t qs[16];
} __attribute__((packed));

// Quantization Dequantization helpers
static void dequantize_q8_0(const void* src, float* dest, int n) {
    const struct block_q8_0* blocks = (const struct block_q8_0*)src;
    int nb = n / 32;
    for (int b = 0; b < nb; b++) {
        float d = fp16_to_fp32(blocks[b].d);
        for (int i = 0; i < 32; i++) {
            dest[b * 32 + i] = blocks[b].qs[i] * d;
        }
    }
}

__attribute__((unused)) static void dequantize_q4_0(const void* src, float* dest, int n) {
    const struct block_q4_0* blocks = (const struct block_q4_0*)src;
    int nb = n / 32;
    for (int b = 0; b < nb; b++) {
        float d = fp16_to_fp32(blocks[b].d);
        for (int i = 0; i < 16; i++) {
            uint8_t byte = blocks[b].qs[i];
            dest[b * 32 + i] = (((byte & 0x0F) - 8)) * d;
            dest[b * 32 + i + 16] = (((byte >> 4) - 8)) * d;
        }
    }
}


float dot_product_q8_0_f32(const void* row_q8, const float* vec_f32, int num_elements) {
    const struct block_q8_0* blocks = (const struct block_q8_0*)row_q8;
    int num_blocks = num_elements / 32;
    float total_sum = 0.0f;
    
    for (int b = 0; b < num_blocks; b++) {
        float d = fp16_to_fp32(blocks[b].d);
        
        // Load 32 bytes (256 bits) of int8 values
        __m256i raw = _mm256_loadu_si256((const __m256i*)blocks[b].qs);
        
        // Split into lower and upper 128-bit halves
        __m128i raw_lo = _mm256_castsi256_si128(raw);
        __m128i raw_hi = _mm256_extracti128_si256(raw, 1);
        
        // Split each 128-bit half into two 64-bit halves for sign extension
        __m128i raw_0_7 = raw_lo;
        __m128i raw_8_15 = _mm_srli_si128(raw_lo, 8);
        __m128i raw_16_23 = raw_hi;
        __m128i raw_24_31 = _mm_srli_si128(raw_hi, 8);
        
        // Convert int8 to int32 (4 registers of 8 elements)
        __m256i i32_0 = _mm256_cvtepi8_epi32(raw_0_7);
        __m256i i32_1 = _mm256_cvtepi8_epi32(raw_8_15);
        __m256i i32_2 = _mm256_cvtepi8_epi32(raw_16_23);
        __m256i i32_3 = _mm256_cvtepi8_epi32(raw_24_31);
        
        // Convert int32 to float
        __m256 f_0 = _mm256_cvtepi32_ps(i32_0);
        __m256 f_1 = _mm256_cvtepi32_ps(i32_1);
        __m256 f_2 = _mm256_cvtepi32_ps(i32_2);
        __m256 f_3 = _mm256_cvtepi32_ps(i32_3);
        
        // Load floats from vec_f32
        __m256 v_0 = _mm256_loadu_ps(vec_f32 + b * 32 + 0);
        __m256 v_1 = _mm256_loadu_ps(vec_f32 + b * 32 + 8);
        __m256 v_2 = _mm256_loadu_ps(vec_f32 + b * 32 + 16);
        __m256 v_3 = _mm256_loadu_ps(vec_f32 + b * 32 + 24);
        
        // FMA: Multiply f_i * v_i and accumulate
        __m256 acc = _mm256_mul_ps(f_0, v_0);
        acc = _mm256_fmadd_ps(f_1, v_1, acc);
        acc = _mm256_fmadd_ps(f_2, v_2, acc);
        acc = _mm256_fmadd_ps(f_3, v_3, acc);
        
        // Horizontal sum of acc
        float block_sum;
        __m128 lo = _mm256_castps256_ps128(acc);
        __m128 hi = _mm256_extractf128_ps(acc, 1);
        __m128 sum128 = _mm_add_ps(lo, hi);
        sum128 = _mm_add_ps(sum128, _mm_shuffle_ps(sum128, sum128, _MM_SHUFFLE(1, 0, 3, 2)));
        sum128 = _mm_add_ps(sum128, _mm_shuffle_ps(sum128, sum128, _MM_SHUFFLE(0, 1, 0, 1)));
        _mm_store_ss(&block_sum, sum128);
        
        total_sum += block_sum * d;
    }
    return total_sum;
}

float dot_product_q4_0_f32(const void* row_q4, const float* vec_f32, int num_elements) {
    const struct block_q4_0* blocks = (const struct block_q4_0*)row_q4;
    int num_blocks = num_elements / 32;
    float total_sum = 0.0f;
    
    __m128i mask = _mm_set1_epi8(0x0F);
    __m128i offset = _mm_set1_epi8(8);
    
    for (int b = 0; b < num_blocks; b++) {
        float d = fp16_to_fp32(blocks[b].d);
        
        // Load 16 bytes (128 bits) of Q4_0 packed weights
        __m128i raw = _mm_loadu_si128((const __m128i*)blocks[b].qs);
        
        // Extract low and high nibbles
        __m128i low_nibbles = _mm_and_si128(raw, mask);
        __m128i high_nibbles = _mm_and_si128(_mm_srli_epi16(raw, 4), mask);
        
        // Subtract offset 8
        low_nibbles = _mm_sub_epi8(low_nibbles, offset);
        high_nibbles = _mm_sub_epi8(high_nibbles, offset);
        
        // Split for sign extension (sequential layout: low nibbles are 0-15, high nibbles are 16-31)
        __m128i raw_0_7 = low_nibbles;
        __m128i raw_8_15 = _mm_srli_si128(low_nibbles, 8);
        __m128i raw_16_23 = high_nibbles;
        __m128i raw_24_31 = _mm_srli_si128(high_nibbles, 8);
        
        // Convert to epi32 (256-bit registers)
        __m256i i32_0 = _mm256_cvtepi8_epi32(raw_0_7);
        __m256i i32_1 = _mm256_cvtepi8_epi32(raw_8_15);
        __m256i i32_2 = _mm256_cvtepi8_epi32(raw_16_23);
        __m256i i32_3 = _mm256_cvtepi8_epi32(raw_24_31);
        
        // Convert to float
        __m256 f_0 = _mm256_cvtepi32_ps(i32_0);
        __m256 f_1 = _mm256_cvtepi32_ps(i32_1);
        __m256 f_2 = _mm256_cvtepi32_ps(i32_2);
        __m256 f_3 = _mm256_cvtepi32_ps(i32_3);
        
        // Load floats from vec_f32
        __m256 v_0 = _mm256_loadu_ps(vec_f32 + b * 32 + 0);
        __m256 v_1 = _mm256_loadu_ps(vec_f32 + b * 32 + 8);
        __m256 v_2 = _mm256_loadu_ps(vec_f32 + b * 32 + 16);
        __m256 v_3 = _mm256_loadu_ps(vec_f32 + b * 32 + 24);
        
        // Multiply and accumulate
        __m256 acc = _mm256_mul_ps(f_0, v_0);
        acc = _mm256_fmadd_ps(f_1, v_1, acc);
        acc = _mm256_fmadd_ps(f_2, v_2, acc);
        acc = _mm256_fmadd_ps(f_3, v_3, acc);
        
        // Horizontal sum of acc
        float block_sum;
        __m128 lo = _mm256_castps256_ps128(acc);
        __m128 hi = _mm256_extractf128_ps(acc, 1);
        __m128 sum128 = _mm_add_ps(lo, hi);
        sum128 = _mm_add_ps(sum128, _mm_shuffle_ps(sum128, sum128, _MM_SHUFFLE(1, 0, 3, 2)));
        sum128 = _mm_add_ps(sum128, _mm_shuffle_ps(sum128, sum128, _MM_SHUFFLE(0, 1, 0, 1)));
        _mm_store_ss(&block_sum, sum128);
        
        total_sum += block_sum * d;
    }
    return total_sum;
}

// Multicore Parallelized Work Scheduler
struct thread_job {
    volatile int active;
    const void* weight_matrix;
    const float* input_vector;
    float* output_vector;
    int rows;
    int cols;
    int type; // 8 = Q8_0, 2 = Q4_0
    int start_row;
    int end_row;
};

volatile struct thread_job jobs[32];
volatile int total_cores = 1;

void ap_main(uint64_t core_id) {
    if (core_id == 3) {
        igpu_run_simulated(3);
        return;
    }
    serial_printf("AP: Core %lld booted and waiting for job...\n", core_id);
    while (1) {
        if (jobs[core_id].active) {
            const void* mat = jobs[core_id].weight_matrix;
            const float* vec = jobs[core_id].input_vector;
            float* out = jobs[core_id].output_vector;
            int cols = jobs[core_id].cols;
            int type = jobs[core_id].type;
            
            if (type == 8) { // Q8_0
                int row_size = (cols / 32) * sizeof(struct block_q8_0);
                for (int r = jobs[core_id].start_row; r < jobs[core_id].end_row; r++) {
                    out[r] = dot_product_q8_0_f32((const char*)mat + r * row_size, vec, cols);
                }
            } else if (type == 2) { // Q4_0
                int row_size = (cols / 32) * sizeof(struct block_q4_0);
                for (int r = jobs[core_id].start_row; r < jobs[core_id].end_row; r++) {
                    out[r] = dot_product_q4_0_f32((const char*)mat + r * row_size, vec, cols);
                }
            }
            
            __asm__ volatile ("" : : : "memory");
            jobs[core_id].active = 0;
        }
        __asm__ volatile ("pause");
    }
}

static struct igpu_gate matmul_gate;

void mat_vec_mul(float* out, const void* weight_matrix, const float* vec, int rows, int cols, int type) {
    size_t w_size = 0;
    int q_type = 0;
    
    if (type == 8) {
        w_size = rows * (cols / 32) * sizeof(struct block_q8_0);
        q_type = 8;
    } else if (type == 2) {
        w_size = rows * (cols / 32) * sizeof(struct block_q4_0);
        q_type = 4;
    } else {
        w_size = rows * cols * sizeof(float);
        q_type = 0;
    }
    
    matmul_gate.owner = IGPU_OWNER_CPU;
    matmul_gate.status = IGPU_STATUS_IDLE;
    matmul_gate.weights_buffer = (void*)weight_matrix;
    matmul_gate.weights_size = w_size;
    matmul_gate.input_buffer = (void*)vec;
    matmul_gate.input_size = cols * sizeof(float);
    matmul_gate.output_buffer = out;
    matmul_gate.output_size = rows * sizeof(float);
    matmul_gate.rows = rows;
    matmul_gate.cols = cols;
    matmul_gate.quant_type = q_type;
    
    igpu_submit_math(&matmul_gate);
    igpu_wait_math(&matmul_gate);
}

// GGUF Parser
static uint8_t* parse_string(uint8_t* ptr, const char** out_str, uint32_t* out_len) {
    uint64_t len = *(uint64_t*)ptr;
    ptr += 8;
    *out_str = (const char*)ptr;
    *out_len = (uint32_t)len;
    ptr += len;
    return ptr;
}

static uint8_t* parse_value(uint8_t* ptr, uint32_t type, const char* key, uint32_t key_len) {
    if (type == 0) { // UINT8
        if (key_match(key, key_len, "tokenizer.ggml.add_bos_token")) {
            // Can check if add_bos_token
        }
        ptr += 1;
    } else if (type == 1) { // INT8
        ptr += 1;
    } else if (type == 2) { // UINT16
        ptr += 2;
    } else if (type == 3) { // INT16
        ptr += 2;
    } else if (type == 4) { // UINT32
        uint32_t val = *(uint32_t*)ptr;
        if (key_match(key, key_len, "llama.block_count")) block_count = val;
        else if (key_match(key, key_len, "llama.context_length")) context_length = val;
        else if (key_match(key, key_len, "llama.embedding_length")) embedding_length = val;
        else if (key_match(key, key_len, "llama.feed_forward_length")) feed_forward_length = val;
        else if (key_match(key, key_len, "llama.attention.head_count")) head_count = val;
        else if (key_match(key, key_len, "llama.attention.head_count_kv")) head_count_kv = val;
        else if (key_match(key, key_len, "llama.vocab_size")) vocab_size = val;
        else if (key_match(key, key_len, "tokenizer.ggml.bos_token_id")) bos_token_id = val;
        else if (key_match(key, key_len, "tokenizer.ggml.eos_token_id")) eos_token_id = val;
        else if (key_match(key, key_len, "tokenizer.ggml.unknown_token_id")) unk_token_id = val;
        else if (key_match(key, key_len, "tokenizer.ggml.unk_token_id")) unk_token_id = val;
        ptr += 4;
    } else if (type == 5) { // INT32
        int32_t val = *(int32_t*)ptr;
        if (key_match(key, key_len, "tokenizer.ggml.bos_token_id")) bos_token_id = val;
        else if (key_match(key, key_len, "tokenizer.ggml.eos_token_id")) eos_token_id = val;
        else if (key_match(key, key_len, "tokenizer.ggml.unknown_token_id")) unk_token_id = val;
        else if (key_match(key, key_len, "tokenizer.ggml.unk_token_id")) unk_token_id = val;
        ptr += 4;
    } else if (type == 6) { // FLOAT32
        float val = *(float*)ptr;
        if (key_match(key, key_len, "llama.attention.layer_norm_rms_epsilon")) {
            layer_norm_rms_epsilon = val;
        }
        ptr += 4;
    } else if (type == 7) { // BOOL
        uint8_t val = *(uint8_t*)ptr;
        if (key_match(key, key_len, "tokenizer.ggml.add_bos_token")) {
            add_bos_token = val;
        }
        ptr += 1;
    } else if (type == 8) { // STRING
        uint64_t len = *(uint64_t*)ptr;
        ptr += 8 + len;
    } else if (type == 9) { // ARRAY
        uint32_t elem_type = *(uint32_t*)ptr;
        ptr += 4;
        uint64_t arr_len = *(uint64_t*)ptr;
        ptr += 8;
        if (elem_type == 8) { // STRING array
            if (key_match(key, key_len, "tokenizer.ggml.tokens")) {
                vocab = (struct vocab_token*)malloc(arr_len * sizeof(struct vocab_token));
                vocab_size = arr_len;
            }
            for (uint64_t i = 0; i < arr_len; i++) {
                uint64_t len = *(uint64_t*)ptr;
                ptr += 8;
                if (key_match(key, key_len, "tokenizer.ggml.tokens")) {
                    if (vocab) {
                        vocab[i].str = (const char*)ptr;
                        vocab[i].len = (uint32_t)len;
                    }
                }
                ptr += len;
            }
        } else if (elem_type == 6) { // FLOAT32 array
            float* scores = (float*)ptr;
            ptr += arr_len * 4;
            if (key_match(key, key_len, "tokenizer.ggml.scores")) {
                for (uint64_t i = 0; i < arr_len; i++) {
                    if (vocab) {
                        vocab[i].score = scores[i];
                    }
                }
            }
        } else {
            uint32_t sizes[] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
            uint32_t elem_size = (elem_type < 13) ? sizes[elem_type] : 1;
            ptr += arr_len * elem_size;
        }
    } else if (type == 10) { // UINT64
        ptr += 8;
    } else if (type == 11) { // INT64
        ptr += 8;
    } else if (type == 12) { // FLOAT64
        ptr += 8;
    }
    return ptr;
}

static void* find_tensor_data(const char* name) {
    for (int i = 0; i < tensor_count; i++) {
        if (key_match(tensors[i].name, tensors[i].name_len, name)) {
            return tensors[i].data;
        }
    }
    serial_printf("GGUF WARNING: Tensor not found: %s\n", name);
    return NULL;
}

static int find_tensor_type(const char* name) {
    for (int i = 0; i < tensor_count; i++) {
        if (key_match(tensors[i].name, tensors[i].name_len, name)) {
            return tensors[i].type;
        }
    }
    return 8; // Default to Q8_0
}

static void make_layer_tensor_name(char* buf, int layer, const char* suffix) {
    int idx = 4;
    buf[0] = 'b';
    buf[1] = 'l';
    buf[2] = 'k';
    buf[3] = '.';
    if (layer >= 10) {
        buf[idx++] = '0' + (layer / 10);
        buf[idx++] = '0' + (layer % 10);
    } else {
        buf[idx++] = '0' + layer;
    }
    buf[idx++] = '.';
    int i = 0;
    while (suffix[i] != '\0') {
        buf[idx++] = suffix[i];
        i++;
    }
    buf[idx] = '\0';
}

int gguf_init(void* model_buffer, size_t model_size) {
    (void)model_size;
    uint8_t* ptr = (uint8_t*)model_buffer;
    struct gguf_header* header = (struct gguf_header*)ptr;
    
    if (strncmp(header->magic, "GGUF", 4) != 0) {
        serial_printf("GGUF ERROR: Invalid magic bytes!\n");
        return 0;
    }
    
    ptr += sizeof(struct gguf_header);
    
    serial_printf("GGUF: Parsing %lld metadata key-value pairs...\n", header->metadata_kv_count);
    
    uint64_t alignment = 32; // Default GGUF alignment
    
    for (uint64_t i = 0; i < header->metadata_kv_count; i++) {
        const char* key;
        uint32_t key_len;
        ptr = parse_string(ptr, &key, &key_len);
        uint32_t val_type = *(uint32_t*)ptr;
        ptr += 4;
        
        // Look for alignment value if present
        if (key_match(key, key_len, "general.alignment")) {
            alignment = *(uint32_t*)ptr;
        }
        
        ptr = parse_value(ptr, val_type, key, key_len);
    }
    
    if (head_count_kv == 0) {
        head_count_kv = head_count;
    }
    
    // Allocate dynamic buffers for forward pass
    // Note: Q, K, V may have different dimensions than embedding_length for models like Qwen3
    // Default: head_dim = embedding_length / head_count
    head_dim = embedding_length / head_count;
    head_dim_q = head_dim;  // Same by default
    q_hidden_dim = head_count * head_dim;
    kv_hidden_dim = head_count_kv * head_dim;
    
    serial_printf("GGUF Params:\n");
    serial_printf("  - Block Count: %d\n", block_count);
    serial_printf("  - Context Length: %d\n", context_length);
    serial_printf("  - Embedding Length: %d\n", embedding_length);
    serial_printf("  - Feed Forward Length: %d\n", feed_forward_length);
    serial_printf("  - Head Count: %d\n", head_count);
    serial_printf("  - Head Count KV: %d\n", head_count_kv);
    serial_printf("  - Vocab Size: %d\n", vocab_size);
    serial_printf("  - Alignment: %lld\n", alignment);
    
    tensor_count = (int)header->tensor_count;
    serial_printf("GGUF: Parsing %d tensor infos...\n", tensor_count);
    
    for (int i = 0; i < tensor_count; i++) {
        ptr = parse_string(ptr, &tensors[i].name, &tensors[i].name_len);
        tensors[i].n_dims = *(uint32_t*)ptr;
        ptr += 4;
        for (uint32_t d = 0; d < tensors[i].n_dims; d++) {
            tensors[i].dims[d] = *(uint64_t*)ptr;
            ptr += 8;
        }
        tensors[i].type = *(uint32_t*)ptr;
        ptr += 4;
        tensors[i].offset = *(uint64_t*)ptr;
        ptr += 8;
    }
    
    // Aligned tensor data block start
    uint64_t current_offset = ptr - (uint8_t*)model_buffer;
    uint64_t aligned_offset = (current_offset + alignment - 1) & ~(alignment - 1);
    uint8_t* tensor_data_block = (uint8_t*)model_buffer + aligned_offset;
    
    for (int i = 0; i < tensor_count; i++) {
        tensors[i].data = tensor_data_block + tensors[i].offset;
    }
    
    // Detect head dimensions from tensor shapes (for Qwen3-style models with Q/K norm)
    // Check layer 0's Q/K norm weight dimensions
    char test_name[64];
    make_layer_tensor_name(test_name, 0, "attn_q_norm.weight");
    for (int i = 0; i < tensor_count; i++) {
        if (key_match(tensors[i].name, tensors[i].name_len, test_name)) {
            if (tensors[i].n_dims >= 1) {
                head_dim_q = tensors[i].dims[0];
                // If it's head_count * head_dim, divide
                if (head_dim_q > embedding_length) {
                    head_dim_q = head_dim_q / head_count;
                }
            }
            break;
        }
    }
    make_layer_tensor_name(test_name, 0, "attn_k_norm.weight");
    for (int i = 0; i < tensor_count; i++) {
        if (key_match(tensors[i].name, tensors[i].name_len, test_name)) {
            if (tensors[i].n_dims >= 1) {
                head_dim = tensors[i].dims[0];
                if (head_dim > embedding_length) {
                    head_dim = head_dim / head_count_kv;
                }
            }
            break;
        }
    }
    
    // Recalculate projection dimensions
    q_hidden_dim = head_count * head_dim_q;
    kv_hidden_dim = head_count_kv * head_dim;
    
    serial_printf("GGUF: Detected Head Dim: %d (Q: %d)\n", head_dim, head_dim_q);
    serial_printf("GGUF: Q Hidden Dim: %d, KV Hidden Dim: %d\n", q_hidden_dim, kv_hidden_dim);
    
    // Link weight structures
    model.token_embd = find_tensor_data("token_embd.weight");
    model.type_embd = find_tensor_type("token_embd.weight");
    model.output = find_tensor_data("output.weight");
    if (!model.output) {
        serial_printf("GGUF: output.weight is tied to token_embd.weight\n");
        model.output = model.token_embd;
        model.type_output = model.type_embd;
    } else {
        model.type_output = find_tensor_type("output.weight");
    }
    model.output_norm = (float*)find_tensor_data("output_norm.weight");
    
    char name_buf[64];
    for (uint32_t l = 0; l < block_count; l++) {
        make_layer_tensor_name(name_buf, l, "attn_q.weight");
        model.layers[l].attn_q = find_tensor_data(name_buf);
        model.layers[l].type_q = find_tensor_type(name_buf);
        
        make_layer_tensor_name(name_buf, l, "attn_k.weight");
        model.layers[l].attn_k = find_tensor_data(name_buf);
        model.layers[l].type_k = find_tensor_type(name_buf);
        
        make_layer_tensor_name(name_buf, l, "attn_v.weight");
        model.layers[l].attn_v = find_tensor_data(name_buf);
        model.layers[l].type_v = find_tensor_type(name_buf);
        
        make_layer_tensor_name(name_buf, l, "attn_output.weight");
        model.layers[l].attn_output = find_tensor_data(name_buf);
        model.layers[l].type_output = find_tensor_type(name_buf);
        
        make_layer_tensor_name(name_buf, l, "attn_norm.weight");
        model.layers[l].attn_norm = (float*)find_tensor_data(name_buf);
        
        // Qwen3-style Q/K normalization
        make_layer_tensor_name(name_buf, l, "attn_q_norm.weight");
        model.layers[l].attn_q_norm = (float*)find_tensor_data(name_buf);
        
        make_layer_tensor_name(name_buf, l, "attn_k_norm.weight");
        model.layers[l].attn_k_norm = (float*)find_tensor_data(name_buf);
        
        // Qwen2.5-style biases
        make_layer_tensor_name(name_buf, l, "attn_q.bias");
        model.layers[l].attn_q_bias = (float*)find_tensor_data(name_buf);
        
        make_layer_tensor_name(name_buf, l, "attn_k.bias");
        model.layers[l].attn_k_bias = (float*)find_tensor_data(name_buf);
        
        make_layer_tensor_name(name_buf, l, "attn_v.bias");
        model.layers[l].attn_v_bias = (float*)find_tensor_data(name_buf);
        
        make_layer_tensor_name(name_buf, l, "ffn_gate.weight");
        model.layers[l].ffn_gate = find_tensor_data(name_buf);
        model.layers[l].type_gate = find_tensor_type(name_buf);
        
        make_layer_tensor_name(name_buf, l, "ffn_up.weight");
        model.layers[l].ffn_up = find_tensor_data(name_buf);
        model.layers[l].type_up = find_tensor_type(name_buf);
        
        make_layer_tensor_name(name_buf, l, "ffn_down.weight");
        model.layers[l].ffn_down = find_tensor_data(name_buf);
        model.layers[l].type_down = find_tensor_type(name_buf);
        
        make_layer_tensor_name(name_buf, l, "ffn_norm.weight");
        model.layers[l].ffn_norm = (float*)find_tensor_data(name_buf);
    }
    
    // Allocate dynamic buffers for forward pass
    // Note: Q, K, V may have different dimensions than embedding_length for models like Qwen3
    
    // Limit context length to fit in available memory
    // KV cache = block_count * context_length * kv_hidden_dim * 4 bytes * 2 (K+V)
    // We want to keep total memory under ~2GB for KV cache
    uint64_t kv_cache_size = (uint64_t)block_count * context_length * kv_hidden_dim * sizeof(float) * 2;
    uint64_t max_kv_cache = 2ULL * 1024 * 1024 * 1024;  // 2GB max for KV cache
    if (kv_cache_size > max_kv_cache) {
        uint64_t new_ctx = max_kv_cache / (block_count * kv_hidden_dim * sizeof(float) * 2);
        // Round down to power of 2 for cleaner arithmetic
        uint64_t pow2 = 1;
        while (pow2 * 2 <= new_ctx) pow2 *= 2;
        new_ctx = pow2;
        if (new_ctx < 256) new_ctx = 256;  // Minimum context
        serial_printf("GGUF: Reducing context_length from %d to %llu (memory limit)\n", context_length, (unsigned long long)new_ctx);
        context_length = (uint32_t)new_ctx;
    }
    
    d_x = (float*)malloc(embedding_length * sizeof(float));
    d_x_norm = (float*)malloc(embedding_length * sizeof(float));
    d_q = (float*)malloc(q_hidden_dim * sizeof(float));       // Q projects to q_hidden_dim
    d_k = (float*)malloc(kv_hidden_dim * sizeof(float));       // K projects to kv_hidden_dim
    d_v = (float*)malloc(kv_hidden_dim * sizeof(float));       // V projects to kv_hidden_dim
    d_scores = (float*)malloc(context_length * sizeof(float));
    d_attn_out = (float*)malloc(head_count * head_dim * sizeof(float)); // Concatenated attention outputs
    d_gate = (float*)malloc(feed_forward_length * sizeof(float));
    d_up = (float*)malloc(feed_forward_length * sizeof(float));
    d_ffn = (float*)malloc(feed_forward_length * sizeof(float));
    d_logits = (float*)malloc(vocab_size * sizeof(float));
    
    // KV cache uses kv_hidden_dim
    d_k_cache = (float*)malloc((size_t)block_count * context_length * kv_hidden_dim * sizeof(float));
    d_v_cache = (float*)malloc((size_t)block_count * context_length * kv_hidden_dim * sizeof(float));
    
    if (!d_x || !d_logits || !d_k_cache || !d_v_cache) {
        serial_printf("GGUF ERROR: Failed to allocate model execution memory buffers!\n");
        return 0;
    }
    
    serial_printf("GGUF: Execution buffers successfully allocated.\n");
    
    // Build vocabulary hash table for O(1) lookup
    build_vocab_hash_table();
    
    return 1;
}

const char* gguf_get_chat_template(void) {
    return NULL; // Template parsed dynamically or instruct file used
}

static uint32_t hash_str(const char* str, int len) {
    uint32_t hash = 5381;
    for (int i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)str[i];
    }
    return hash % HASH_SIZE;
}

static void build_vocab_hash_table(void) {
    hash_entries = malloc(vocab_size * sizeof(struct vocab_hash_entry));
    if (!hash_entries) {
        serial_printf("GGUF WARNING: Failed to allocate vocab hash table! Falling back to linear scan.\n");
        return;
    }
    
    for (int i = 0; i < HASH_SIZE; i++) {
        vocab_hash[i] = NULL;
    }
    
    // Auto-detect tokenizer type by scanning vocabulary for U+2581 (0xE2 0x96 0x81)
    is_sentencepiece = 0;
    for (int i = 0; i < vocab_size; i++) {
        if (vocab[i].len >= 3) {
            for (uint32_t j = 0; j <= vocab[i].len - 3; j++) {
                if ((unsigned char)vocab[i].str[j] == 0xE2 &&
                    (unsigned char)vocab[i].str[j+1] == 0x96 &&
                    (unsigned char)vocab[i].str[j+2] == 0x81) {
                    is_sentencepiece = 1;
                    break;
                }
            }
        }
        if (is_sentencepiece) break;
    }
    
    if (add_bos_token == -1) {
        add_bos_token = is_sentencepiece ? 1 : 0;
    }
    
    serial_printf("GGUF: Tokenizer type detected: %s (add_bos_token: %d, bos_token_id: %d, eos_token_id: %d, unk_token_id: %d)\n", 
                  is_sentencepiece ? "SentencePiece" : "BPE", add_bos_token, bos_token_id, eos_token_id, unk_token_id);
    
    for (int i = 0; i < vocab_size; i++) {
        uint32_t h = hash_str(vocab[i].str, vocab[i].len);
        hash_entries[i].token_id = i;
        hash_entries[i].next = vocab_hash[h];
        vocab_hash[h] = &hash_entries[i];
    }
    serial_printf("GGUF: Vocab hash table built successfully.\n");
}

// Tokenizer Greedy Longest Match prefix matching (O(1) average lookup)
static int find_token_by_str(const char* str, int len) {
    if (!hash_entries) {
        // Fallback to sequential scan
        for (int i = 0; i < vocab_size; i++) {
            if (vocab[i].len == (uint32_t)len && strncmp(vocab[i].str, str, len) == 0) {
                return i;
            }
        }
        return -1;
    }
    
    uint32_t h = hash_str(str, len);
    struct vocab_hash_entry* entry = vocab_hash[h];
    while (entry) {
        int tok = entry->token_id;
        if (vocab[tok].len == (uint32_t)len && strncmp(vocab[tok].str, str, len) == 0) {
            return tok;
        }
        entry = entry->next;
    }
    return -1;
}

static uint32_t byte_to_unicode_cp(uint8_t b) {
    if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174)) {
        return b;
    }
    if (b <= 32) {
        return 256 + b;
    }
    if (b >= 127 && b <= 160) {
        return 256 + 33 + (b - 127);
    }
    if (b == 173) {
        return 256 + 33 + 34;
    }
    return b;
}

static int tokenize(const char* text, int* tokens) {
    int n_tokens = 0;
    int len = strlen(text);
    int i = 0;
    
    // Conditionally add BOS token based on add_bos_token
    if (add_bos_token) {
        tokens[n_tokens++] = bos_token_id;
    }
    
    while (i < len) {
        int longest_match = -1;
        int longest_len = 0;
        
        int max_l = len - i;
        if (max_l > 64) max_l = 64;
        for (int l = 1; l <= max_l; l++) {
            int tok = find_token_by_str(text + i, l);
            if (tok != -1) {
                longest_match = tok;
                longest_len = l;
            }
        }
        
        if (longest_match != -1) {
            tokens[n_tokens++] = longest_match;
            i += longest_len;
        } else {
            if (is_sentencepiece) {
                // Byte fallback representation: <0xXX>
                unsigned char c = (unsigned char)text[i];
                char hex_str[8];
                hex_str[0] = '<';
                hex_str[1] = '0';
                hex_str[2] = 'x';
                const char* hex_digits = "0123456789ABCDEF";
                hex_str[3] = hex_digits[c >> 4];
                hex_str[4] = hex_digits[c & 0x0F];
                hex_str[5] = '>';
                hex_str[6] = '\0';
                
                int tok = find_token_by_str(hex_str, 6);
                if (tok != -1) {
                    tokens[n_tokens++] = tok;
                } else {
                    tokens[n_tokens++] = unk_token_id;
                }
                i++;
            } else {
                // In BPE mode, if a byte somehow fails to match, map to unk_token_id directly
                tokens[n_tokens++] = unk_token_id;
                i++;
            }
        }
    }
    return n_tokens;
}

// Helper to preprocess spaces to U+2581 block character for SentencePiece tokenizer, or BPE unicode mapping
static void preprocess_text(const char* src, char* dest, int max_len) {
    if (!is_sentencepiece) {
        int j = 0;
        for (int i = 0; src[i] != '\0' && j < max_len - 3; i++) {
            uint8_t b = (uint8_t)src[i];
            uint32_t u = byte_to_unicode_cp(b);
            if (u <= 127) {
                dest[j++] = (char)u;
            } else {
                dest[j++] = (char)(0xC0 | (u >> 6));
                dest[j++] = (char)(0x80 | (u & 0x3F));
            }
        }
        dest[j] = '\0';
        return;
    }
    
    int j = 0;
    // Add prefix space if not present (SentencePiece word-start behavior)
    if (src[0] != ' ' && src[0] != '\0' && j < max_len - 3) {
        dest[j++] = (char)0xE2;
        dest[j++] = (char)0x96;
        dest[j++] = (char)0x81;
    }
    for (int i = 0; src[i] != '\0' && j < max_len - 4; i++) {
        if (src[i] == ' ') {
            dest[j++] = (char)0xE2;
            dest[j++] = (char)0x96;
            dest[j++] = (char)0x81;
        } else {
            dest[j++] = src[i];
        }
    }
    dest[j] = '\0';
}

static void rmsnorm(float* o, const float* x, const float* weight, int size) {
    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        sum += x[i] * x[i];
    }
    float scale = 1.0f / (float)sqrt((double)(sum / size) + layer_norm_rms_epsilon);
    for (int i = 0; i < size; i++) {
        o[i] = x[i] * scale * weight[i];
    }
}

// Transformer layer forward pass
static void transformer_forward(int token_id, int pos) {
    int dim = embedding_length;
    int group_size = head_count / head_count_kv;
    
    // 1. Token Embeddings
    if (model.type_embd == 8) {
        dequantize_q8_0((const char*)model.token_embd + token_id * (dim / 32 * sizeof(struct block_q8_0)), d_x, dim);
    } else if (model.type_embd == 2) {
        dequantize_q4_0((const char*)model.token_embd + token_id * (dim / 32 * sizeof(struct block_q4_0)), d_x, dim);
    } else {
        serial_printf("GGUF ERROR: Unsupported embedding tensor type: %d\n", model.type_embd);
    }
    
    // Loop layers
    for (uint32_t l = 0; l < block_count; l++) {
        // RMSNorm input
        rmsnorm(d_x_norm, d_x, model.layers[l].attn_norm, dim);
        
        // Q, K, V Projections
        // Q: embedding_length -> q_hidden_dim
        // K,V: embedding_length -> kv_hidden_dim
        mat_vec_mul(d_q, model.layers[l].attn_q, d_x_norm, q_hidden_dim, dim, model.layers[l].type_q);
        mat_vec_mul(d_k, model.layers[l].attn_k, d_x_norm, kv_hidden_dim, dim, model.layers[l].type_k);
        mat_vec_mul(d_v, model.layers[l].attn_v, d_x_norm, kv_hidden_dim, dim, model.layers[l].type_v);
        
        // Add biases if present (Qwen2.5-style)
        if (model.layers[l].attn_q_bias) {
            for (uint32_t i = 0; i < q_hidden_dim; i++) {
                d_q[i] += model.layers[l].attn_q_bias[i];
            }
        }
        if (model.layers[l].attn_k_bias) {
            for (uint32_t i = 0; i < kv_hidden_dim; i++) {
                d_k[i] += model.layers[l].attn_k_bias[i];
            }
        }
        if (model.layers[l].attn_v_bias) {
            for (uint32_t i = 0; i < kv_hidden_dim; i++) {
                d_v[i] += model.layers[l].attn_v_bias[i];
            }
        }
        
        // Apply Q/K normalization if present (Qwen3-style)
        if (model.layers[l].attn_q_norm) {
            for (uint32_t h = 0; h < head_count; h++) {
                float* q_head = d_q + h * head_dim_q;
                float sum = 0.0f;
                for (uint32_t i = 0; i < head_dim_q; i++) {
                    sum += q_head[i] * q_head[i];
                }
                float scale = 1.0f / (float)sqrt((double)(sum / head_dim_q) + layer_norm_rms_epsilon);
                for (uint32_t i = 0; i < head_dim_q; i++) {
                    q_head[i] = q_head[i] * scale * model.layers[l].attn_q_norm[i];
                }
            }
        }
        if (model.layers[l].attn_k_norm) {
            for (uint32_t h = 0; h < head_count_kv; h++) {
                float* k_head = d_k + h * head_dim;
                float sum = 0.0f;
                for (uint32_t i = 0; i < head_dim; i++) {
                    sum += k_head[i] * k_head[i];
                }
                float scale = 1.0f / (float)sqrt((double)(sum / head_dim) + layer_norm_rms_epsilon);
                for (uint32_t i = 0; i < head_dim; i++) {
                    k_head[i] = k_head[i] * scale * model.layers[l].attn_k_norm[i];
                }
            }
        }
        
        // Rotary Position Embeddings (RoPE)
        // Q RoPE - uses head_dim_q for each head
        for (uint32_t h = 0; h < head_count; h++) {
            for (uint32_t i = 0; i < head_dim_q; i += 2) {
                double freq = 1.0 / pow(10000.0, (double)i / (double)head_dim_q);
                double theta = (double)pos * freq;
                float cos_theta = cos(theta);
                float sin_theta = sin(theta);
                
                int idx1 = h * head_dim_q + i;
                int idx2 = h * head_dim_q + i + 1;
                
                float q1 = d_q[idx1];
                float q2 = d_q[idx2];
                d_q[idx1] = q1 * cos_theta - q2 * sin_theta;
                d_q[idx2] = q1 * sin_theta + q2 * cos_theta;
            }
        }
        // K RoPE - uses head_dim for KV heads
        for (uint32_t h = 0; h < head_count_kv; h++) {
            for (uint32_t i = 0; i < head_dim; i += 2) {
                double freq = 1.0 / pow(10000.0, (double)i / (double)head_dim);
                double theta = (double)pos * freq;
                float cos_theta = cos(theta);
                float sin_theta = sin(theta);
                
                int idx1 = h * head_dim + i;
                int idx2 = h * head_dim + i + 1;
                
                float k1 = d_k[idx1];
                float k2 = d_k[idx2];
                d_k[idx1] = k1 * cos_theta - k2 * sin_theta;
                d_k[idx2] = k1 * sin_theta + k2 * cos_theta;
            }
        }
        
        // Write K, V cache for this token
        uint64_t cache_offset = (l * context_length * kv_hidden_dim) + (pos * kv_hidden_dim);
        memcpy(d_k_cache + cache_offset, d_k, kv_hidden_dim * sizeof(float));
        memcpy(d_v_cache + cache_offset, d_v, kv_hidden_dim * sizeof(float));
        
        // Multi-head Attention
        // For GQA: group_size = head_count / head_count_kv
        float scale = 1.0f / (float)sqrt((double)head_dim_q);  // Scale by Q head dim
        for (uint32_t h = 0; h < head_count; h++) {
            float* q_head = d_q + h * head_dim_q;
            uint32_t kv_h = h / group_size;  // Which KV head this Q head uses
            
            // Calculate attention scores for all previous positions
            for (int p = 0; p <= pos; p++) {
                float* k_cached = d_k_cache + (l * context_length * kv_hidden_dim) + (p * kv_hidden_dim) + kv_h * head_dim;
                float score = 0.0f;
                for (uint32_t i = 0; i < head_dim; i++) {
                    score += q_head[i] * k_cached[i];
                }
                d_scores[p] = score * scale;
            }
            
            // Softmax over scores [0, pos]
            float max_score = d_scores[0];
            for (int p = 1; p <= pos; p++) {
                if (d_scores[p] > max_score) max_score = d_scores[p];
            }
            
            float sum = 0.0f;
            for (int p = 0; p <= pos; p++) {
                d_scores[p] = (float)exp(d_scores[p] - max_score);
                sum += d_scores[p];
            }
            for (int p = 0; p <= pos; p++) {
                d_scores[p] /= sum;
            }
            
            // Compute head output vector
            float* out_head = d_attn_out + h * head_dim;
            for (uint32_t i = 0; i < head_dim; i++) {
                out_head[i] = 0.0f;
            }
            
            for (int p = 0; p <= pos; p++) {
                float* v_cached = d_v_cache + (l * context_length * kv_hidden_dim) + (p * kv_hidden_dim) + kv_h * head_dim;
                float attn_weight = d_scores[p];
                for (uint32_t i = 0; i < head_dim; i++) {
                    out_head[i] += attn_weight * v_cached[i];
                }
            }
        }
        
        // Attention Output projection: (head_count * head_dim) -> embedding_length
        float* x_attn = d_q;  // reuse buffer (large enough to hold embedding_length)
        int attn_out_dim = head_count * head_dim;
        mat_vec_mul(x_attn, model.layers[l].attn_output, d_attn_out, dim, attn_out_dim, model.layers[l].type_output);
        
        // Residual addition
        for (int i = 0; i < dim; i++) {
            d_x[i] += x_attn[i];
        }
        
        // Feed-Forward Network
        rmsnorm(d_x_norm, d_x, model.layers[l].ffn_norm, dim);
        
        // Gate and Up projections
        mat_vec_mul(d_gate, model.layers[l].ffn_gate, d_x_norm, feed_forward_length, dim, model.layers[l].type_gate);
        mat_vec_mul(d_up, model.layers[l].ffn_up, d_x_norm, feed_forward_length, dim, model.layers[l].type_up);
        
        // SwiGLU: SiLU(gate) * up
        for (uint32_t i = 0; i < feed_forward_length; i++) {
            float g = d_gate[i];
            float silu = g / (1.0f + (float)exp(-g));
            d_ffn[i] = silu * d_up[i];
        }
        
        // Down projection
        float* x_ffn = d_x_norm; // reuse buffer
        mat_vec_mul(x_ffn, model.layers[l].ffn_down, d_ffn, dim, feed_forward_length, model.layers[l].type_down);
        
        // Residual addition
        for (int i = 0; i < dim; i++) {
            d_x[i] += x_ffn[i];
        }
    }
    
    // Final layer norm
    rmsnorm(d_x_norm, d_x, model.output_norm, dim);
    
    // Logits projection
    mat_vec_mul(d_logits, model.output, d_x_norm, vocab_size, dim, model.type_output);
}

static void print_token(int token_id, void (*token_callback)(const char* token)) {
    if (token_id < 0 || token_id >= vocab_size) return;
    
    const char* str = vocab[token_id].str;
    uint32_t len = vocab[token_id].len;
    
    // Check for byte fallback e.g. <0xXX>
    if (len == 6 && str[0] == '<' && str[1] == '0' && str[2] == 'x' && str[5] == '>') {
        char hex[3];
        hex[0] = str[3];
        hex[1] = str[4];
        hex[2] = '\0';
        
        unsigned char c = 0;
        for (int i = 0; i < 2; i++) {
            c <<= 4;
            if (hex[i] >= '0' && hex[i] <= '9') c += hex[i] - '0';
            else if (hex[i] >= 'A' && hex[i] <= 'F') c += hex[i] - 'A' + 10;
            else if (hex[i] >= 'a' && hex[i] <= 'f') c += hex[i] - 'a' + 10;
        }
        char out[2] = { (char)c, '\0' };
        token_callback(out);
        return;
    }
    
    // Remove SentencePiece space symbol U+2581
    char out_buf[256];
    uint32_t out_idx = 0;
    for (uint32_t i = 0; i < len && out_idx < sizeof(out_buf) - 1; i++) {
        if (i + 2 < len && 
            (unsigned char)str[i] == 0xE2 && 
            (unsigned char)str[i+1] == 0x96 && 
            (unsigned char)str[i+2] == 0x81) {
            out_buf[out_idx++] = ' ';
            i += 2;
        } else {
            out_buf[out_idx++] = str[i];
        }
    }
    out_buf[out_idx] = '\0';
    token_callback(out_buf);
}

static uint32_t rng_state = 123456789;
static uint32_t xorshift32(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}
static float random_float(void) {
    return (float)(xorshift32() & 0xFFFFFF) / 16777216.0f;
}
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static void quicksort_logits(float* arr, int* indices, int left, int right) {
    if (left >= right) return;
    int i = left, j = right;
    float pivot = arr[(left + right) / 2];
    while (i <= j) {
        while (arr[i] > pivot) i++;
        while (arr[j] < pivot) j--;
        if (i <= j) {
            float tmp_a = arr[i]; arr[i] = arr[j]; arr[j] = tmp_a;
            int tmp_i = indices[i]; indices[i] = indices[j]; indices[j] = tmp_i;
            i++; j--;
        }
    }
    quicksort_logits(arr, indices, left, j);
    quicksort_logits(arr, indices, i, right);
}

void gguf_generate(const char* prompt, int max_tokens, void (*token_callback)(const char* token)) {
    int tokens[256];
    char preprocessed[1024];
    preprocess_text(prompt, preprocessed, 1024);
    
    int n_tokens = tokenize(preprocessed, tokens);
    if (n_tokens <= 0) return;
    
    serial_set_vga_visible(0); // Hide debug prints from VGA screen
    
    serial_printf("GGUF: Prompt tokenized into %d tokens.\n", n_tokens);
    serial_printf("Tokens: ");
    for (int i = 0; i < (n_tokens < 15 ? n_tokens : 15); i++) {
        serial_printf("%d ", tokens[i]);
    }
    serial_printf("\n");
    
    // Seed the RNG dynamically using the TSC
    rng_state = (uint32_t)rdtsc();
    if (rng_state == 0) rng_state = 123456789;
    
    // Prefill with progress every token for first 10 positions, then every 10 tokens
    int last_token = tokens[0];
    serial_printf("\nGGUF: Prefill %d tokens\n", n_tokens - 1);
    serial_printf("GGUF Debug: layers=%d, heads=%d/%d, dim=%d/%d/%d\n",
                 block_count, head_count, head_count_kv, head_dim, head_dim_q, kv_hidden_dim);
    for (int i = 0; i < n_tokens - 1; i++) {
        transformer_forward(tokens[i], i);
        last_token = tokens[i + 1];
        if (i < 10 || (i + 1) % 10 == 0) {
            serial_printf("%d%s", i + 1, (i + 1) % 50 == 0 ? "\n" : " ");
        }
    }
    serial_printf("\nGGUF: Prefill complete\n");
    
    // 2. Generate new tokens
    int pos = n_tokens - 1;
    for (int step = 0; step < max_tokens; step++) {
        // Run forward pass for the last token
        transformer_forward(last_token, pos);
        
        serial_printf("\nLogits[0..4]: %d %d %d %d %d\n", 
                      (int)(d_logits[0] * 100), (int)(d_logits[1] * 100), 
                      (int)(d_logits[2] * 100), (int)(d_logits[3] * 100), 
                      (int)(d_logits[4] * 100));
                      
        // Sampling parameters
        float temperature = 0.7f;
        float top_p = 0.9f;
        int next_token = 0;
        
        if (temperature <= 0.0f) {
            // Greedy sampling
            float max_logit = d_logits[0];
            for (int i = 1; i < vocab_size; i++) {
                if (d_logits[i] > max_logit) {
                    max_logit = d_logits[i];
                    next_token = i;
                }
            }
        } else {
            float* probs = malloc(vocab_size * sizeof(float));
            int* indices = malloc(vocab_size * sizeof(int));
            if (!probs || !indices) {
                // Fallback to greedy if malloc fails
                float max_logit = d_logits[0];
                for (int i = 1; i < vocab_size; i++) {
                    if (d_logits[i] > max_logit) {
                        max_logit = d_logits[i];
                        next_token = i;
                    }
                }
                if (probs) free(probs);
                if (indices) free(indices);
            } else {
                float max_logit = d_logits[0] / temperature;
                for (int i = 1; i < vocab_size; i++) {
                    float scaled = d_logits[i] / temperature;
                    if (scaled > max_logit) max_logit = scaled;
                }
                
                float sum_exp = 0.0f;
                for (int i = 0; i < vocab_size; i++) {
                    probs[i] = (float)exp(d_logits[i] / temperature - max_logit);
                    sum_exp += probs[i];
                    indices[i] = i;
                }
                for (int i = 0; i < vocab_size; i++) {
                    probs[i] /= sum_exp;
                }
                
                quicksort_logits(probs, indices, 0, vocab_size - 1);
                
                float cumsum = 0.0f;
                int n_candidates = 0;
                for (int i = 0; i < vocab_size; i++) {
                    cumsum += probs[i];
                    n_candidates++;
                    if (cumsum >= top_p) {
                        break;
                    }
                }
                
                float r = random_float() * cumsum;
                float sum = 0.0f;
                next_token = indices[n_candidates - 1];
                for (int i = 0; i < n_candidates; i++) {
                    sum += probs[i];
                    if (r <= sum) {
                        next_token = indices[i];
                        break;
                    }
                }
                
                free(probs);
                free(indices);
            }
        }
        
        // Stop if EOS is generated
        if (next_token == eos_token_id) {
            break;
        }
        
        // Output token (temporarily show to VGA screen)
        serial_set_vga_visible(1);
        print_token(next_token, token_callback);
        serial_set_vga_visible(0);
        
        // Prepare next iteration
        last_token = next_token;
        pos++;
        
        if (pos >= (int)context_length) {
            break; // Context window limit reached
        }
    }
    
    serial_set_vga_visible(1); // Restore VGA visibility
}
