#ifndef DMA_MEMORY_ALOCATOR_H
#define DMA_MEMORY_ALOCATOR_H

/* includes */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* macros */
// VideoCore Mailbox interface path
#define DEVICE_FILE_NAME "/dev/vcio"
#define MAJOR_NUM 100
#define IOCTL_MBOX_PROPERTY _IOWR(MAJOR_NUM, 0, char *)

// Mailbox tags for memory allocation
#define MEM_ALLOC_TAG 0x3000C
#define MEM_LOCK_TAG  0x3000D
#define MEM_FREE_TAG  0x3000F
#define MEM_UNLOCK_TAG 0x3000E

// Direct Uncached memory flag (Required for DMA on Pi 4)
#define MEM_FLAG_DIRECT (1 << 2) | (1 << 3)
#define PAGE_SIZE 4096


/* types */
// Struct to hold the allocated memory details
typedef struct {
    uint32_t handle;       // GPU reference handle
    uint32_t bus_addr;     // Address to give to the DMA
    uint32_t phys_addr;    // Address to map in /dev/mem
    void* virtual_addr;    // Pointer for your C code to read/write
    size_t size;
} DmaBuffer;

/* function declarations */
int allocate_dma_buffer(DmaBuffer* buf, size_t size);


#endif /* DMA_MEMORY_ALOCATOR_H */