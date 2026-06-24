#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "dma_mem.h"
#include "dma_control.h"

#define BCM2711_PERI_BASE 0xFE000000
#define DMA_BASE          (BCM2711_PERI_BASE + 0x007000)
#define DMA_CH5_BASE      (DMA_BASE + 0x500)

// DMA Transfer Information for simple Memory-to-Memory copy
// Bit 8: Source Address Increment
// Bit 4: Destination Address Increment
#define DMA_TI_MEMCPY     ((1 << 8) | (1 << 4))

#define TEST_BUFFER_SIZE  4095 // Bytes to copy

int main() {
    DmaBuffer src_buf, dest_buf, cb_buf;
    int test_passed = 1;

    printf("--- Starting DMA Infrastructure Test ---\n");

    // 1. Allocate Memory Buffers
    printf("Allocating DMA buffers...\n");
    if (allocate_dma_buffer(&src_buf, TEST_BUFFER_SIZE) != 0) {
        printf("FAIL: Could not allocate source buffer.\n");
        return -1;
    }
    if (allocate_dma_buffer(&dest_buf, TEST_BUFFER_SIZE) != 0) {
        printf("FAIL: Could not allocate destination buffer.\n");
        free_dma_buffer(&src_buf);
        return -1;
    }
    if (allocate_dma_buffer(&cb_buf, sizeof(struct DmaControlBlock)) != 0) {
        printf("FAIL: Could not allocate Control Block buffer.\n");
        free_dma_buffer(&src_buf);
        free_dma_buffer(&dest_buf);
        return -1;
    }

    // 2. Populate Source Buffer and Clear Destination
    uint32_t* src_ptr = (uint32_t*)src_buf.virtual_addr;
    uint32_t* dest_ptr = (uint32_t*)dest_buf.virtual_addr;

    for (int i = 0; i < TEST_BUFFER_SIZE / 4; i++) {
        src_ptr[i] = 0xAABBCC00 + i; // Fill with dummy pattern
        dest_ptr[i] = 0x00000000;    // Ensure dest is strictly 0
    }

    // 3. Setup DMA Control Block for Memory-to-Memory
    printf("Configuring Control Block...\n");
    struct DmaControlBlock* cb = (struct DmaControlBlock*)cb_buf.virtual_addr;
    setup_dma_control_block(cb, DMA_TI_MEMCPY, src_buf.bus_addr, dest_buf.bus_addr, TEST_BUFFER_SIZE);

    // 4. Map DMA Peripheral Registers
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        printf("FAIL: Cannot open /dev/mem. Are you running with sudo?\n");
        free_dma_buffer(&src_buf);
        free_dma_buffer(&dest_buf);
        free_dma_buffer(&cb_buf);
        return -1;
    }

    // CRITICAL FIX: mmap requires a page-aligned physical address (multiple of 0x1000).
    // DMA_BASE is 0xFE007000 (Aligned). DMA_CH5_BASE is 0xFE007500 (Not aligned).
    volatile uint32_t* dma_base_map = mmap(
        NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, DMA_BASE
    );

    if (dma_base_map == MAP_FAILED) {
        printf("FAIL: Failed to map DMA registers.\n");
        free_dma_buffer(&src_buf);
        free_dma_buffer(&dest_buf);
        free_dma_buffer(&cb_buf);
        close(mem_fd);
        return -1;
    }

    // Offset the pointer manually by 0x500 bytes to reach Channel 5.
    // Since dma_base_map is a 32-bit pointer (4 bytes), we divide the offset by 4.
    volatile uint32_t* dma_chan5 = dma_base_map + (0x500 / 4);

    // 5. Execute DMA Transfer
    printf("Starting DMA engine...\n");
    start_dma_channel(dma_chan5, cb_buf.bus_addr);

    // 6. Wait for Completion
    int timeout = 100000;
    while (!is_dma_transfer_complete(dma_chan5)) {
        timeout--;
        if (timeout == 0) {
            printf("FAIL: DMA Timeout! Engine hung.\n");
            stop_dma_channel(dma_chan5);
            test_passed = 0;
            break;
        }
    }

    // 7. Validate Results
    if (test_passed) {
        printf("DMA Transfer finished. Validating data...\n");
        for (int i = 0; i < TEST_BUFFER_SIZE / 4; i++) {
            if (dest_ptr[i] != src_ptr[i]) {
                printf("FAIL: Data mismatch at index %d. Expected 0x%08X, got 0x%08X\n", i, src_ptr[i], dest_ptr[i]);
                test_passed = 0;
                break;
            }
        }
    }

    if (test_passed) {
        printf("\n==========================================\n");
        printf("  SUCCESS! DMA Infrastructure is working. \n");
        printf("==========================================\n");
    }

    // 8. Cleanup Everything
    munmap((void*)dma_chan5, 4096);
    close(mem_fd);
    free_dma_buffer(&src_buf);
    free_dma_buffer(&dest_buf);
    free_dma_buffer(&cb_buf);

    return test_passed ? 0 : -1;
}
