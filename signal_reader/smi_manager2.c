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

// Configuration for 4 buffers, each 4096 bytes (1024 samples)
#define BUFFER_SAMPLES       1024 
#define BUFFER_BYTES         (BUFFER_SAMPLES * 4) 
#define NUM_BUFFERS          4
#define TOTAL_BUFFER_BYTES   (BUFFER_BYTES * NUM_BUFFERS)

// DMA Configuration: SMI DREQ, Wait for DREQ, Increment Dest, Interrupt Enable
#define DMA_TI_SMI_CIRCULAR  ((4 << 16) | (1 << 10) | (1 << 4) | (1 << 2))

volatile int keep_running = 1;

void handle_sigint(int sig) { 
    keep_running = 0; 
}

void export_and_plot(uint32_t* safe_buffer, uint32_t trigger_index) {
    printf("Exporting triggered buffer data...\n");
    FILE *fp = fopen("waveform.csv", "w");
    if (!fp) {
        printf("Error: Cannot create waveform.csv\n");
        return;
    }

    fprintf(fp, "Index,GPIO8,GPIO9\n");
    
    // Dump the 1024 samples of the safe buffer to the CSV file
    for (int i = 0; i < BUFFER_SAMPLES; i++) {
        uint32_t val = safe_buffer[i];
        int gpio8 = (val & (1 << 0)) ? 1 : 0;
        int gpio9 = (val & (1 << 1)) ? 1 : 0;
        
        fprintf(fp, "%d,%d,%d\n", i - (int)trigger_index, gpio8, gpio9);
    }
    fclose(fp);

    printf("Plotting graph...\n");
    system("python3 plot_waveform.py");
}

int main() {
    printf("--- SMI 4-Stage Circular Logic Analyzer ---\n");
    signal(SIGINT, handle_sigint);

    DmaBuffer cb_buf, sample_buf;
    SmiHardware smi_hw;
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        printf("Error: Cannot open /dev/mem. Use sudo.\n");
        return -1;
    }

    uint32_t target_rate_hz = 2000000; 

    // 1. Allocate contiguous memory for all 4 buffers combined
    if (allocate_dma_buffer(&sample_buf, TOTAL_BUFFER_BYTES) != 0) {
        printf("Failed to allocate sample buffer.\n");
        return -1;
    }
    
    // 2. Allocate memory for 4 Control Blocks
    if (allocate_dma_buffer(&cb_buf, sizeof(struct DmaControlBlock) * NUM_BUFFERS) != 0) {
        printf("Failed to allocate control blocks.\n");
        return -1;
    }
    
    struct DmaControlBlock* cbs = (struct DmaControlBlock*)cb_buf.virtual_addr;

    // 3. Chain the 4 blocks together (0->1->2->3->0)
    for (int i = 0; i < NUM_BUFFERS; i++) {
        setup_dma_control_block(&cbs[i], DMA_TI_SMI_CIRCULAR, 0x7E60000C, 
                                sample_buf.bus_addr + (i * BUFFER_BYTES), BUFFER_BYTES);
        
        if (i == NUM_BUFFERS - 1) {
            // The last block points back to the very first block
            cbs[i].nextconbk = cb_buf.bus_addr; 
        } else {
            // Point to the next contiguous block in the array
            cbs[i].nextconbk = cb_buf.bus_addr + ((i + 1) * sizeof(struct DmaControlBlock));
        }
    }

    smi_init(&smi_hw, mem_fd);
    volatile uint32_t *dma_base = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, 0xFE007000);
    volatile uint32_t *dma_chan5 = dma_base + (0x500 / 4);

    // 4. Hardware Reset for DMA to prevent ghosting from previous runs
    dma_chan5[0] = (1 << 31); 
    usleep(1000); 
    dma_chan5[0] = 0;

    // 5. Start DMA using the first control block
    start_dma_channel(dma_chan5, cb_buf.bus_addr);
    
    // Start SMI at continuous mode
    smi_start_capture(&smi_hw, 0xFFFFFFFF, target_rate_hz); 

    printf("Armed (4 buffers chained). Waiting for falling edge on GPIO8...\n");

    volatile uint32_t* samples = (volatile uint32_t*)sample_buf.virtual_addr;
    uint32_t* safe_storage = malloc(BUFFER_BYTES);
    
    int triggered = 0;
    uint32_t current_cb_index = 0; 

    while (keep_running) {
        // Read the physical address of the currently active control block
        uint32_t active_cb_addr = dma_chan5[0x04 / 4]; 
        
        // Calculate the index (0 to 3) of the active block
        uint32_t active_cb_index = (active_cb_addr - cb_buf.bus_addr) / sizeof(struct DmaControlBlock);

        // Safety check to ensure the index is valid and see if it moved
        if (active_cb_index < NUM_BUFFERS && active_cb_index != current_cb_index) {
            
            // The DMA moved to a new block, meaning it just finished the previous block
            uint32_t finished_cb = current_cb_index;
            uint32_t start_idx = finished_cb * BUFFER_SAMPLES;
            
            // Scan only the newly completed buffer safely
            for (int i = 0; i < BUFFER_SAMPLES; i++) {
                uint32_t val = samples[start_idx + i];
                
                // Trigger condition: GPIO8 goes low
                if (!(val & (1 << 0))) {
                    printf("Triggered in buffer %d at local index %d!\n", finished_cb, i);
                    
                    // Copy this safe, completed buffer to our permanent storage
                    for(int j = 0; j < BUFFER_SAMPLES; j++) {
                        safe_storage[j] = samples[start_idx + j];
                    }
                    
                    triggered = 1;
                    keep_running = 0; // Break main loop
                    break;
                }
            }
            
            // Update the tracker
            current_cb_index = active_cb_index;
        }
        
        usleep(10); 
    }

    // Stop hardware completely
    smi_stop_capture(&smi_hw);
    stop_dma_channel(dma_chan5);

    if (triggered) {
        export_and_plot(safe_storage, 0); 
    } else {
        printf("\nCapture aborted manually.\n");
    }

    // Cleanup resources
    smi_cleanup(&smi_hw);
    if (dma_base != MAP_FAILED) munmap((void*)dma_base, 4096);
    close(mem_fd);
    free_dma_buffer(&sample_buf);
    free_dma_buffer(&cb_buf);
    free(safe_storage);
    
    return 0;
}