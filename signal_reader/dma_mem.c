#include "dma_mem.h"
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEVICE_FILE_NAME "/dev/vcio"
#define MAJOR_NUM 100
#define IOCTL_MBOX_PROPERTY _IOWR(MAJOR_NUM, 0, char *)

#define MEM_ALLOC_TAG   0x3000C
#define MEM_LOCK_TAG    0x3000D
#define MEM_FREE_TAG    0x3000F
#define MEM_UNLOCK_TAG  0x3000E

#define MEM_FLAG_DIRECT (1 << 2) | (1 << 3)
#define PAGE_SIZE       4046

static int mbox_fd = -1;

// Internal helper to communicate with the VideoCore GPU Mailbox
static int mbox_property(void *buf) {
    if (mbox_fd < 0) {
        mbox_fd = open(DEVICE_FILE_NAME, O_RDONLY);
        if (mbox_fd < 0) {
            return -1;
        }
    }
    return ioctl(mbox_fd, IOCTL_MBOX_PROPERTY, buf);
}

int allocate_dma_buffer(DmaBuffer* buf, size_t size) {
    uint32_t p[32];
    int i = 0;
    
    // Align allocation size to standard page size boundaries
    buf->size = (size % PAGE_SIZE == 0) ? size : size + (PAGE_SIZE - (size % PAGE_SIZE));

    // 1. Tag: Allocate Memory on GPU
    p[i++] = 0;           // Buffer size placeholder
    p[i++] = 0;           // Request code
    p[i++] = MEM_ALLOC_TAG;
    p[i++] = 12;          // Value buffer size
    p[i++] = 0;           // Request/response indicator
    p[i++] = buf->size;   // Request parameter: size
    p[i++] = PAGE_SIZE;   // Request parameter: alignment
    p[i++] = MEM_FLAG_DIRECT; // Request parameter: uncached flags
    p[i++] = 0;           // End tag
    p[0] = i * sizeof(uint32_t); // Set final buffer size

    if (mbox_property(p) < 0 || p[5] == 0) {
        return -1;
    }
    buf->handle = p[5];

    // 2. Tag: Lock Memory to acquire Bus Address
    i = 0;
    p[i++] = 0; p[i++] = 0;
    p[i++] = MEM_LOCK_TAG;
    p[i++] = 4; p[i++] = 0;
    p[i++] = buf->handle;
    p[i++] = 0;
    p[0] = i * sizeof(uint32_t);

    if (mbox_property(p) < 0 || p[5] == 0) {
        return -1;
    }
    buf->bus_addr = p[5];
    
    // Clear top bits to map Bus Address to Physical Address for mmap
    buf->phys_addr = buf->bus_addr & ~0xC0000000;

    // 3. Map physical memory into user space
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        return -1;
    }
    
    buf->virtual_addr = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, buf->phys_addr);
    close(mem_fd);

    if (buf->virtual_addr == MAP_FAILED) {
        return -1;
    }

    return 0;
}

void free_dma_buffer(DmaBuffer* buf) {
    if (!buf || !buf->virtual_addr) return;
    
    // Unmap virtual memory view
    munmap(buf->virtual_addr, buf->size);
    
    uint32_t p[32];
    // Unlock memory tag
    p[0] = 32; p[1] = 0; p[2] = MEM_UNLOCK_TAG; p[3] = 4; p[4] = 0; p[5] = buf->handle; p[6] = 0;
    mbox_property(p);
    
    // Free memory tag
    p[0] = 32; p[1] = 0; p[2] = MEM_FREE_TAG; p[3] = 4; p[4] = 0; p[5] = buf->handle; p[6] = 0;
    mbox_property(p);
    
    buf->virtual_addr = NULL;
    
    // Close global mailbox file descriptor if it was opened
    if (mbox_fd >= 0) {
        close(mbox_fd);
        mbox_fd = -1;
    }
}