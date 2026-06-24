#include "dma_memory_alocator.h"

// Global file descriptor for the mailbox
int mbox_fd = -1;

// Internal function to communicate with the GPU
static int mbox_property(void *buf) {
    if (mbox_fd < 0) {
        mbox_fd = open(DEVICE_FILE_NAME, 0);
        if (mbox_fd < 0) {
            printf("Error: Cannot open %s. Run as root.\n", DEVICE_FILE_NAME);
            return -1;
        }
    }
    return ioctl(mbox_fd, IOCTL_MBOX_PROPERTY, buf);
}

// Function to allocate contiguous, uncached RAM
int allocate_dma_buffer(DmaBuffer* buf, size_t size) {
    uint32_t p[32];
    int i = 0;
    
    // Ensure size is a multiple of page size
    buf->size = (size % PAGE_SIZE == 0) ? size : size + (PAGE_SIZE - (size % PAGE_SIZE));

    // 1. Request Allocation from GPU
    p[i++] = 0;          // Total size (will be updated)
    p[i++] = 0;          // Process Request
    p[i++] = MEM_ALLOC_TAG;
    p[i++] = 12;         // Tag size
    p[i++] = 0;          // Request code
    p[i++] = buf->size;  // Parameter 1: Size
    p[i++] = PAGE_SIZE;  // Parameter 2: Alignment
    p[i++] = MEM_FLAG_DIRECT; // Parameter 3: Flags
    p[i++] = 0;          // End tag
    p[0] = i * sizeof(uint32_t); // Fill in total size

    if (mbox_property(p) < 0 || p[5] == 0) {
        printf("Error: Failed to allocate memory via Mailbox.\n");
        return -1;
    }
    buf->handle = p[5];

    // 2. Lock the memory to get the Bus Address
    i = 0;
    p[i++] = 0;
    p[i++] = 0;
    p[i++] = MEM_LOCK_TAG;
    p[i++] = 4;
    p[i++] = 0;
    p[i++] = buf->handle;
    p[i++] = 0;
    p[0] = i * sizeof(uint32_t);

    if (mbox_property(p) < 0 || p[5] == 0) {
        printf("Error: Failed to lock memory.\n");
        return -1;
    }
    
    buf->bus_addr = p[5];
    
    // Pi 4 converts bus addresses to physical addresses by clearing the top bits
    buf->phys_addr = buf->bus_addr & ~0xC0000000;

    // 3. Map the physical address into user-space Virtual Memory
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) return -1;

    buf->virtual_addr = mmap(
        NULL, 
        buf->size, 
        PROT_READ | PROT_WRITE, 
        MAP_SHARED, 
        mem_fd, 
        buf->phys_addr
    );

    close(mem_fd);

    if (buf->virtual_addr == MAP_FAILED) return -1;
    
    return 0; // Success
}