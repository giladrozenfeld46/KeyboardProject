#ifndef DMA_MEM_H
#define DMA_MEM_H

#include <stddef.h>
#include <stdint.h>

// Structure to encapsulate all attributes of a contiguous DMA buffer
typedef struct {
    uint32_t handle;       // GPU memory handle reference
    uint32_t bus_addr;     // Bus address as seen by the DMA controller
    uint32_t phys_addr;    // Physical address used for memory mapping
    void* virtual_addr;    // Virtual address pointer used by the CPU
    size_t size;           // Total allocated size (aligned to page size)
} DmaBuffer;

// Allocates contiguous, uncached memory via VideoCore Mailbox
// Returns 0 on success, -1 on failure
int allocate_dma_buffer(DmaBuffer* buf, size_t size);

// Unmaps and releases the allocated memory back to the GPU
void free_dma_buffer(DmaBuffer* buf);

#endif // DMA_MEM_H