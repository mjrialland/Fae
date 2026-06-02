#ifndef IGPU_H
#define IGPU_H

#include <stdint.h>
#include <stddef.h>

#define IGPU_OWNER_CPU 0
#define IGPU_OWNER_GPU 1

#define IGPU_STATUS_IDLE       0
#define IGPU_STATUS_PROCESSING 1
#define IGPU_STATUS_COMPLETE   2
#define IGPU_STATUS_ERROR      3

struct igpu_gate {
    volatile uint32_t owner;        // IGPU_OWNER_CPU or IGPU_OWNER_GPU
    volatile uint32_t status;       // Idle, Processing, Complete, Error
    
    void* weights_buffer;           // Shared weights address
    size_t weights_size;            // Size of weights buffer
    
    void* input_buffer;             // Input vector (F32)
    size_t input_size;              // Size of input buffer
    
    void* output_buffer;            // Output vector (F32)
    size_t output_size;             // Size of output buffer
    
    // Matrix dimensions
    int rows;
    int cols;
    int quant_type;                 // 8 = Q8_0, 4 = Q4_0
};

void igpu_init(void);
int igpu_has_hardware(void);
void igpu_map_bars(void);

// AMDGPU Ring Buffer management
void amd_gpu_ring_init(void);
void amd_gpu_ring_write(uint32_t val);
void amd_gpu_ring_commit(void);

void igpu_submit_math(struct igpu_gate* gate);
void igpu_wait_math(struct igpu_gate* gate);

// Emulated iGPU thread loop for AP core
void igpu_run_simulated(int core_id);

// Function to control CPU page tables Present bit
void set_memory_cpu_access(void* addr, size_t size, int allowed);

#endif
