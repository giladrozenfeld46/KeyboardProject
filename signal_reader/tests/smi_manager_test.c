#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dma_mem.h"
#include "dma_control.h"
#include "smi_hal.h"

// Hardware addresses needed by the Manager
#define BCM2711_PERI_BASE    0xFE000000 
#define DMA_BASE             (BCM2711_PERI_BASE + 0x007000)
#define SMI_D_BUS_ADDR       0x7E60000C 

// DMA configuration: Map to SMI DREQ, wait for DREQ, increment destination
#define DMA_TI_SMI_CAPTURE   ((4 << 16) | (1 << 10) | (1 << 4))

// Fixed Buffer Size Configuration
#define FIXED_BUFFER_SAMPLES 1024
#define FIXED_BUFFER_BYTES   (FIXED_BUFFER_SAMPLES * 4) // 4096 bytes (4KB)

#define SMI_SAMPLE_RATE_HZ   2500000 // 2.5 MHz


int main() {
    printf("--- SMI & DMA Manager initialized ---\n");

    DmaBuffer cb_buf, sample_buf;
    SmiHardware smi_hw;
    int mem_fd = -1;
    volatile uint32_t *dma_base_map = MAP_FAILED;

    // 1. Allocate strictly fixed DMA buffers
    printf("Allocating a fixed %d byte buffer...\n", FIXED_BUFFER_BYTES);
    if (allocate_dma_buffer(&sample_buf, FIXED_BUFFER_BYTES) != 0) {
        printf("Error: Failed to allocate sample buffer.\n");
        return -1;
    }
    
    if (allocate_dma_buffer(&cb_buf, sizeof(struct DmaControlBlock)) != 0) {
        printf("Error: Failed to allocate Control Block.\n");
        free_dma_buffer(&sample_buf);
        return -1;
    }

    // 2. Setup DMA Control Block for a single linear capture
    struct DmaControlBlock* cb = (struct DmaControlBlock*)cb_buf.virtual_addr;
    setup_dma_control_block(cb, DMA_TI_SMI_CAPTURE, SMI_D_BUS_ADDR, sample_buf.bus_addr, FIXED_BUFFER_BYTES);

    // 3. Open System Memory
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        printf("Error: Cannot open /dev/mem. Use sudo.\n");
        goto cleanup;
    }

    // 4. Initialize SMI Hardware via HAL
    if (smi_init(&smi_hw, mem_fd) != 0) {
        printf("Error: SMI HAL initialization failed.\n");
        goto cleanup;
    }

    // 5. Map DMA Registers (Remembering the page alignment rule)
    dma_base_map = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, DMA_BASE);
    if (dma_base_map == MAP_FAILED) {
        printf("Error: DMA register mapping failed.\n");
        goto cleanup;
    }
    volatile uint32_t *dma_chan5 = dma_base_map + (0x500 / 4);

    // 6. Execute coordinated capture
    printf("Arming DMA and triggering SMI at %d Hz...\n", SMI_SAMPLE_RATE_HZ);
    
    start_dma_channel(dma_chan5, cb_buf.bus_addr);
    
    // Pass the sample rate directly to the HAL
    smi_start_capture(&smi_hw, FIXED_BUFFER_SAMPLES, SMI_SAMPLE_RATE_HZ);

    // 7. Wait for completion (Simple blocking wait for this iteration)
    int timeout = 500000;
    while (!is_dma_transfer_complete(dma_chan5)) {
        timeout--;
        if (timeout == 0) {
            printf("Error: DMA Timeout! Engine hung.\n");
            stop_dma_channel(dma_chan5);
            break;
        }
    }

    // 8. Output Results
    if (timeout > 0) {
        printf("Capture successful. Processing data...\n");
        uint32_t* samples = (uint32_t*)sample_buf.virtual_addr;
        
        for (int i = 0; i < 5; i++) {
            printf("Sample %04d: GPIO8=%d, GPIO9=%d\n", 
                i, (samples[i] & 1) ? 1 : 0, (samples[i] & 2) ? 1 : 0);
        }
    }

cleanup:
    // 9. Graceful System Teardown
    printf("Tearing down infrastructure...\n");
    smi_cleanup(&smi_hw);
    
    if (dma_base_map != MAP_FAILED) munmap((void*)dma_base_map, 4096);
    if (mem_fd >= 0) close(mem_fd);
    
    free_dma_buffer(&sample_buf);
    free_dma_buffer(&cb_buf);

    printf("Exiting safely.\n");
    return 0;
}