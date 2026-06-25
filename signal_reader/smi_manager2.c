#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>

#include "dma_mem.h"
#include "dma_control.h"
#include "smi_hal.h"

#define FIXED_BUFFER_SAMPLES 1024
#define FIXED_BUFFER_BYTES   (FIXED_BUFFER_SAMPLES * 4) 
#define DMA_TI_SMI_CAPTURE   ((4 << 16) | (1 << 10) | (1 << 4))

volatile int keep_running = 1;

void handle_sigint(int sig) { 
    keep_running = 0; 
}

int main() {
    printf("--- SMI Real-Time Circular Capture Initialized ---\n");
    signal(SIGINT, handle_sigint);

    DmaBuffer cb_buf, sample_buf;
    SmiHardware smi_hw;
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    
    if (mem_fd < 0) {
        printf("Error: Cannot open /dev/mem. Use sudo.\n");
        return -1;
    }

    // --- CONFIGURATION ---
    // Change this variable to control the sample rate dynamically
    uint32_t target_rate_hz = 25000000; 

    // 1. Allocate buffers
    allocate_dma_buffer(&sample_buf, FIXED_BUFFER_BYTES);
    allocate_dma_buffer(&cb_buf, sizeof(struct DmaControlBlock));

    // 2. Setup DMA Control Block for CONTINUOUS LOOP
    struct DmaControlBlock* cb = (struct DmaControlBlock*)cb_buf.virtual_addr;
    setup_dma_control_block(cb, DMA_TI_SMI_CAPTURE, 0x7E60000C, sample_buf.bus_addr, FIXED_BUFFER_BYTES);
    
    // This is the magic line that makes the DMA run infinitely
    cb->nextconbk = cb_buf.bus_addr; 

    // 3. Init Hardware
    smi_init(&smi_hw, mem_fd);
    
    volatile uint32_t *dma_base = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, 0xFE007000);
    volatile uint32_t *dma_chan5 = dma_base + (0x500 / 4);

    // 4. Start Hardware
    start_dma_channel(dma_chan5, cb_buf.bus_addr);
    
    // We send 0xFFFFFFFF so the SMI reads endlessly, using the target frequency
    smi_start_capture(&smi_hw, 0xFFFFFFFF, target_rate_hz); 

    printf("Capturing at %d Hz. Monitoring GPIO8 for low state...\n", target_rate_hz);

    // 5. Processing Loop
    volatile uint32_t* samples = (volatile uint32_t*)sample_buf.virtual_addr;
    uint32_t last_index = 0;

    while (keep_running) {
        uint32_t current_dest = dma_chan5[0x0C / 4]; 
        uint32_t current_index = (current_dest - sample_buf.bus_addr) / 4;

        while (last_index != current_index && keep_running) {
            uint32_t val = samples[last_index];
            
            if (!(val & (1 << 0))) {
                printf("Low signal detected at index %d\n", last_index);
            }

            last_index = (last_index + 1) % FIXED_BUFFER_SAMPLES;
        }
        usleep(100); 
    }

    // 6. Cleanup
    printf("\nShutting down safely...\n");
    smi_stop_capture(&smi_hw);
    smi_cleanup(&smi_hw);
    
    if (dma_base != MAP_FAILED) munmap((void*)dma_base, 4096);
    close(mem_fd);
    free_dma_buffer(&sample_buf);
    free_dma_buffer(&cb_buf);
    
    return 0;
}