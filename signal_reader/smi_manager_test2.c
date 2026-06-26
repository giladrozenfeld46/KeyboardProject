#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <time.h> 
#include <string.h>

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
    DmaBuffer sample_bufs[NUM_BUFFERS]; 
    SmiHardware smi_hw;
    
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        printf("Error: Cannot open /dev/mem. Use sudo.\n");
        return -1;
    }

    uint32_t target_rate_hz = 2000000; 

    // 1. Allocate separate memory blocks for each buffer 
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (allocate_dma_buffer(&sample_bufs[i], BUFFER_BYTES) != 0) {
            printf("Failed to allocate sample buffer %d.\n", i);
            for (int j = 0; j < i; j++) {
                free_dma_buffer(&sample_bufs[j]);
            }
            return -1;
        }
    }
    
    // 2. Allocate memory for all Control Blocks
    if (allocate_dma_buffer(&cb_buf, sizeof(struct DmaControlBlock) * NUM_BUFFERS) != 0) {
        printf("Failed to allocate control blocks.\n");
        for (int i = 0; i < NUM_BUFFERS; i++) {
            free_dma_buffer(&sample_bufs[i]);
        }
        return -1;
    }
    
    struct DmaControlBlock* cbs = (struct DmaControlBlock*)cb_buf.virtual_addr;

    // 3. Chain the blocks together
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
    volatile uint32_t *dma_base = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, 0xFE007000);
    volatile uint32_t *dma_chan5 = dma_base + (0x500 / 4);

    // 4. Hardware Reset for DMA 
    dma_chan5[0] = (1 << 31); 
    usleep(1000); 
    dma_chan5[0] = 0;

    // Variables for performance metrics
    struct timespec start_scan, end_scan;
    uint64_t total_scan_time_ns = 0;
    uint64_t total_samples_scanned = 0;

    // Start DMA 
    start_dma_channel(dma_chan5, cb_buf.bus_addr);
    smi_start_capture(&smi_hw, 0xFFFFFFFF, target_rate_hz); 

    printf("Armed (%d scattered buffers chained). Waiting for FALLING EDGE on GPIO8...\n", NUM_BUFFERS);

    uint32_t* safe_storage = malloc(BUFFER_BYTES);
    
    int triggered = 0;
    uint32_t current_cb_index = 0; 
    uint32_t found_trigger_index = 0; 
    
    // VARIABLES FOR EDGE DETECTION
    uint32_t prev_sample_val = 0xFFFFFFFF; // Assume high at boot
    int has_history = 0;

    while (keep_running) {
        uint32_t active_cb_addr = dma_chan5[0x04 / 4]; 
        uint32_t active_cb_index = (active_cb_addr - cb_buf.bus_addr) / sizeof(struct DmaControlBlock);

        if (active_cb_index < NUM_BUFFERS && active_cb_index != current_cb_index) {
            
            uint32_t finished_cb = current_cb_index;
            volatile uint32_t* current_samples = (volatile uint32_t*)sample_bufs[finished_cb].virtual_addr;
            
            int local_triggered = 0;

            // FAST MEMORY COPY: Move the entire block to CPU cache before scanning to maximize speed
            uint32_t local_cached_buffer[BUFFER_SAMPLES];
            memcpy(local_cached_buffer, (void*)current_samples, BUFFER_BYTES);

            clock_gettime(CLOCK_MONOTONIC, &start_scan);

            for (int i = 0; i < BUFFER_SAMPLES; i++) {
                uint32_t val = local_cached_buffer[i];
                total_samples_scanned++;
                
                if (has_history) {
                    // EDGE DETECTION LOGIC:
                    // Check if GPIO8 was HIGH in the previous sample, and is LOW in the current sample
                    int prev_gpio8 = prev_sample_val & (1 << 0);
                    int curr_gpio8 = val & (1 << 0);

                    if (prev_gpio8 && !curr_gpio8) {
                        found_trigger_index = i;
                        local_triggered = 1;
                        break; 
                    }
                }
                
                // Save current value for the next iteration's comparison
                prev_sample_val = val;
                has_history = 1;
            }

            clock_gettime(CLOCK_MONOTONIC, &end_scan);
            
            uint64_t ns_spent = (end_scan.tv_sec - start_scan.tv_sec) * 1000000000ULL + 
                                (end_scan.tv_nsec - start_scan.tv_nsec);
            total_scan_time_ns += ns_spent;

            if (local_triggered) {
                printf("Falling Edge Triggered in scattered buffer %d at local index %d!\n", finished_cb, found_trigger_index);
                
                // Copy the cached buffer to permanent storage instantly
                memcpy(safe_storage, local_cached_buffer, BUFFER_BYTES);
                
                triggered = 1;
                keep_running = 0; 
            }
            
            current_cb_index = active_cb_index;
        }
        
        usleep(10); 
    }

    struct timespec start_stop, end_stop;
    clock_gettime(CLOCK_MONOTONIC, &start_stop);
    
    // HARD RESET: Instantly freeze the DMA channel before stopping the SMI
    dma_chan5[0] = (1 << 31);
    smi_stop_capture(&smi_hw);
    
    clock_gettime(CLOCK_MONOTONIC, &end_stop);
    uint64_t stop_time_ns = (end_stop.tv_sec - start_stop.tv_sec) * 1000000000ULL + 
                            (end_stop.tv_nsec - start_stop.tv_nsec);

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

    smi_cleanup(&smi_hw);
    if (dma_base != MAP_FAILED) munmap((void*)dma_base, 4096);
    close(mem_fd);
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        free_dma_buffer(&sample_bufs[i]);
    }
    free_dma_buffer(&cb_buf);
    free(safe_storage);
    
    return 0;
}