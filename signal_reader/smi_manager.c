#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h> // Required for memcpy and memset

#include "dma_mem.h"
#include "dma_control.h"
#include "smi_hal.h"
#include "smi_manager.h"

// Configuration for multiple separate buffers
#define BUFFER_SAMPLES       1024 
#define BUFFER_BYTES         (BUFFER_SAMPLES * 4) 
#define NUM_BUFFERS          5

// Optimization: Number of samples to fetch in a single burst (must be a divisor of BUFFER_SAMPLES)
#define CHUNK_SIZE           8

// DMA Configuration: SMI DREQ, Wait for DREQ, Increment Dest, Interrupt Enable
#define DMA_TI_SMI_CIRCULAR  ((4 << 16) | (1 << 10) | (1 << 4) | (1 << 2))

// --- GLOBAL MODULE STATE ---
static DmaBuffer cb_buf;
static DmaBuffer sample_bufs[NUM_BUFFERS]; 
static SmiHardware smi_hw;
static volatile uint32_t *dma_base = NULL;
static volatile uint32_t *dma_chan5 = NULL;
static int mem_fd = -1;

// --- GLOBAL READ POINTERS ---
// These point to the exact location of the next chunk to be fetched
uint32_t g_current_read_buffer = 0;
uint32_t g_current_read_index = 0;

/**
 * Initializes the DMA and SMI hardware, allocates buffers, 
 * pre-fills them with 0xFFFFFFFF, and starts the capture in the background.
 * Returns 0 on success, -1 on failure.
 */
int smi_manager_init(uint32_t target_rate_hz) {
    printf("Initializing SMI Manager Module (%d Buffers, Chunk Size: %d)...\n", NUM_BUFFERS, CHUNK_SIZE);
    
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        printf("Error: Cannot open /dev/mem. Use sudo.\n");
        return -1;
    }

    // 1. Allocate separate memory blocks for each buffer
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (allocate_dma_buffer(&sample_bufs[i], BUFFER_BYTES) != 0) {
            printf("Failed to allocate sample buffer %d.\n", i);
            return -1;
        }

        // PRE-FILL the buffer with 0xFFFFFFFF so we can detect when DMA writes to it
        volatile uint32_t* buf_ptr = (volatile uint32_t*)sample_bufs[i].virtual_addr;
        for (int j = 0; j < BUFFER_SAMPLES; j++) {
            buf_ptr[j] = 0xFFFFFFFF;
        }
    }
    
    // 2. Allocate memory for all Control Blocks
    if (allocate_dma_buffer(&cb_buf, sizeof(struct DmaControlBlock) * NUM_BUFFERS) != 0) {
        printf("Failed to allocate control blocks.\n");
        return -1;
    }
    
    struct DmaControlBlock* cbs = (struct DmaControlBlock*)cb_buf.virtual_addr;

    // 3. Chain the blocks together circularly
    for (int i = 0; i < NUM_BUFFERS; i++) {
        setup_dma_control_block(&cbs[i], DMA_TI_SMI_CIRCULAR, 0x7E60000C, 
                                sample_bufs[i].bus_addr, BUFFER_BYTES);
        
        if (i == NUM_BUFFERS - 1) {
            cbs[i].nextconbk = cb_buf.bus_addr; 
        } else {
            cbs[i].nextconbk = cb_buf.bus_addr + ((i + 1) * sizeof(struct DmaControlBlock));
        }
    }

    smi_init(&smi_hw, mem_fd);
    dma_base = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, 0xFE007000);
    dma_chan5 = dma_base + (0x500 / 4);

    // 4. Hardware Reset for DMA 
    dma_chan5[0] = (1 << 31); 
    usleep(1000); 
    dma_chan5[0] = 0;

    // Reset read pointers
    g_current_read_buffer = 0;
    g_current_read_index = 0;

    // 5. Start DMA and SMI continuous capture
    start_dma_channel((volatile uint32_t*)dma_chan5, cb_buf.bus_addr);
    smi_start_capture(&smi_hw, 0xFFFFFFFF, target_rate_hz); 

    printf("SMI Background Capture Started.\n");
    return 0;
}

/**
 * Attempts to read a chunk of 8 samples from the buffer using fast memory copying.
 * Expects an array of at least 8 uint32_t elements as out_samples.
 * Returns 0 if the chunk is not ready yet.
 * Returns 1 if 8 samples were successfully copied into out_samples.
 */
int smi_manager_read_chunk(uint32_t *out_samples) {
    if (!out_samples) return 0;

    // Access the exact location the global pointer is targeting
    volatile uint32_t* current_buffer_ptr = (volatile uint32_t*)sample_bufs[g_current_read_buffer].virtual_addr;

    // Check if the LAST sample in our chunk has been overwritten by the DMA.
    // Since DMA writes sequentially, if the 8th sample is ready, all 8 are ready.
    if (current_buffer_ptr[g_current_read_index + (CHUNK_SIZE - 1)] == 0xFFFFFFFF) {
        return 0; // Chunk not fully ready yet
    }

    // FAST READ: Burst copy 8 samples (32 bytes) from Uncached RAM to CPU Cache
    memcpy(out_samples, (void*)&current_buffer_ptr[g_current_read_index], CHUNK_SIZE * sizeof(uint32_t));

    // FAST WIPE: Reset the 8 samples to 0xFFFFFFFF in Uncached RAM
    // 0xFF byte pattern results in 0xFFFFFFFF for a 32-bit integer
    memset((void*)&current_buffer_ptr[g_current_read_index], 0xFF, CHUNK_SIZE * sizeof(uint32_t));

    // Advance the read pointer by the chunk size
    g_current_read_index += CHUNK_SIZE;
    
    // Wrap around logic for the buffers
    if (g_current_read_index >= BUFFER_SAMPLES) {
        g_current_read_index = 0; // Reset index
        g_current_read_buffer++;  // Move to next buffer
        
        // Wrap around to the first buffer if we reached the end
        if (g_current_read_buffer >= NUM_BUFFERS) {
            g_current_read_buffer = 0;
        }
    }

    return 1; // Successfully read a chunk
}

/**
 * Stops the hardware, unmaps memory, and frees the allocated DMA buffers.
 */
void smi_manager_cleanup() {
    printf("Stopping SMI and DMA hardware...\n");
    
    if (dma_chan5) {
        // HARD RESET: Instantly freeze the DMA channel
        dma_chan5[0] = (1 << 31);
    }
    
    smi_stop_capture(&smi_hw);
    smi_cleanup(&smi_hw);
    
    if (dma_base && dma_base != MAP_FAILED) {
        munmap((void*)dma_base, 4096);
    }
    
    if (mem_fd >= 0) {
        close(mem_fd);
    }
    
    // Free all scattered buffers
    for (int i = 0; i < NUM_BUFFERS; i++) {
        free_dma_buffer(&sample_bufs[i]);
    }
    free_dma_buffer(&cb_buf);
    
    printf("SMI Manager cleaned up successfully.\n");
}