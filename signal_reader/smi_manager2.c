#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <time.h> // Added for high-resolution timing

#include "dma_mem.h"
#include "dma_control.h"
#include "smi_hal.h"

// Configuration for multiple separate buffers, each 4096 bytes (1024 samples)
#define BUFFER_SAMPLES       1024 
#define BUFFER_BYTES         (BUFFER_SAMPLES * 4) 
#define NUM_BUFFERS          16

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
        
        // Align the actual trigger event to x=0 on the graph
        fprintf(fp, "%d,%d,%d\n", i - (int)trigger_index, gpio8, gpio9);
    }
    fclose(fp);

    printf("Plotting graph...\n");
    system("python3 plot_waveform.py");
}

int main() {
    printf("--- SMI Scatter-Gather Logic Analyzer (%d Buffers) ---\n", NUM_BUFFERS);
    signal(SIGINT, handle_sigint);

    DmaBuffer cb_buf;
    DmaBuffer sample_bufs[NUM_BUFFERS]; // Array of separate DMA buffers
    SmiHardware smi_hw;
    
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        printf("Error: Cannot open /dev/mem. Use sudo.\n");
        return -1;
    }

    uint32_t target_rate_hz = 2000000; 

    // 1. Allocate separate memory blocks for each buffer (Scatter-Gather approach)
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (allocate_dma_buffer(&sample_bufs[i], BUFFER_BYTES) != 0) {
            printf("Failed to allocate sample buffer %d.\n", i);
            for (int j = 0; j < i; j++) {
                free_dma_buffer(&sample_bufs[j]);
            }
            return -1;
        }
    }
    
    // 2. Allocate memory for all Control Blocks in one contiguous array
    if (allocate_dma_buffer(&cb_buf, sizeof(struct DmaControlBlock) * NUM_BUFFERS) != 0) {
        printf("Failed to allocate control blocks.\n");
        for (int i = 0; i < NUM_BUFFERS; i++) {
            free_dma_buffer(&sample_bufs[i]);
        }
        return -1;
    }
    
    struct DmaControlBlock* cbs = (struct DmaControlBlock*)cb_buf.virtual_addr;

    // 3. Chain the blocks together, pointing each to its respective physical buffer
    for (int i = 0; i < NUM_BUFFERS; i++) {
        setup_dma_control_block(&cbs[i], DMA_TI_SMI_CIRCULAR, 0x7E60000C, 
                                sample_bufs[i].bus_addr, BUFFER_BYTES);
        
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

    // Variables for performance metrics
    struct timespec start_scan, end_scan;
    uint64_t total_scan_time_ns = 0;
    uint64_t total_samples_scanned = 0;

    // 5. Start DMA using the first control block
    start_dma_channel(dma_chan5, cb_buf.bus_addr);
    
    // Start SMI at continuous mode
    smi_start_capture(&smi_hw, 0xFFFFFFFF, target_rate_hz); 

    printf("Armed (%d scattered buffers chained). Waiting for rising edge on GPIO9...\n", NUM_BUFFERS);

    uint32_t* safe_storage = malloc(BUFFER_BYTES);
    
    int triggered = 0;
    uint32_t current_cb_index = 0; 
    uint32_t found_trigger_index = 0; 

    while (keep_running) {
        // Read the physical address of the currently active control block
        uint32_t active_cb_addr = dma_chan5[0x04 / 4]; 
        
        // Calculate the index (0 to NUM_BUFFERS - 1) of the active block
        uint32_t active_cb_index = (active_cb_addr - cb_buf.bus_addr) / sizeof(struct DmaControlBlock);

        // Safety check to ensure the index is valid and see if it moved
        if (active_cb_index < NUM_BUFFERS && active_cb_index != current_cb_index) {
            
            // The DMA moved to a new block, meaning it just finished the previous block
            uint32_t finished_cb = current_cb_index;
            
            // Access the virtual memory of the specific buffer that was just completed
            volatile uint32_t* current_samples = (volatile uint32_t*)sample_bufs[finished_cb].virtual_addr;
            
            int local_triggered = 0;

            // Start scanning timer
            clock_gettime(CLOCK_MONOTONIC, &start_scan);

            // Scan only the newly completed buffer safely
            for (int i = 0; i < BUFFER_SAMPLES; i++) {
                uint32_t val = current_samples[i];
                total_samples_scanned++;
                
                // Trigger condition: GPIO9 goes high
                if (val & (1 << 1)) {
                    found_trigger_index = i;
                    local_triggered = 1;
                    break; // Stop scanning, trigger found
                }
            }

            // End scanning timer
            clock_gettime(CLOCK_MONOTONIC, &end_scan);
            
            // Accumulate scan time
            uint64_t ns_spent = (end_scan.tv_sec - start_scan.tv_sec) * 1000000000ULL + 
                                (end_scan.tv_nsec - start_scan.tv_nsec);
            total_scan_time_ns += ns_spent;

            // If triggered, handle the data copying OUTSIDE the timer to get accurate sample scan times
            if (local_triggered) {
                printf("Triggered in scattered buffer %d at local index %d!\n", finished_cb, found_trigger_index);
                
                // Copy this safe, completed buffer to our permanent storage
                for(int j = 0; j < BUFFER_SAMPLES; j++) {
                    safe_storage[j] = current_samples[j];
                }
                
                triggered = 1;
                keep_running = 0; // Break main loop
            }
            
            // Update the tracker
            current_cb_index = active_cb_index;
        }
        
        usleep(10); 
    }

    // Stop hardware completely
    smi_stop_capture(&smi_hw);

    // Measure time taken to stop the DMA
    struct timespec start_stop, end_stop;
    clock_gettime(CLOCK_MONOTONIC, &start_stop);
    
    stop_dma_channel(dma_chan5);
    
    clock_gettime(CLOCK_MONOTONIC, &end_stop);
    uint64_t stop_time_ns = (end_stop.tv_sec - start_stop.tv_sec) * 1000000000ULL + 
                            (end_stop.tv_nsec - start_stop.tv_nsec);

    // Print Performance Metrics
    printf("\n--- Performance Metrics ---\n");
    if (total_samples_scanned > 0) {
        double avg_scan_time = (double)total_scan_time_ns / total_samples_scanned;
        printf("Average time to scan a single sample: %.2f ns\n", avg_scan_time);
    }
    printf("Time taken to stop DMA hardware: %llu ns (%.2f us)\n", 
           (unsigned long long)stop_time_ns, stop_time_ns / 1000.0);
    printf("---------------------------\n\n");

    if (triggered) {
        export_and_plot(safe_storage, found_trigger_index); 
    } else {
        printf("Capture aborted manually.\n");
    }

    // Cleanup resources
    smi_cleanup(&smi_hw);
    if (dma_base != MAP_FAILED) munmap((void*)dma_base, 4096);
    close(mem_fd);
    
    // Free all scattered buffers
    for (int i = 0; i < NUM_BUFFERS; i++) {
        free_dma_buffer(&sample_bufs[i]);
    }
    free_dma_buffer(&cb_buf);
    free(safe_storage);
    
    return 0;
}