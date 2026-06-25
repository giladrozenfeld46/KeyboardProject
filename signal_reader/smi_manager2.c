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

// Increased buffer size to hold pre-trigger and post-trigger data
#define FIXED_BUFFER_SAMPLES 2048
#define FIXED_BUFFER_BYTES   (FIXED_BUFFER_SAMPLES * 4) 
#define DMA_TI_SMI_CAPTURE   ((4 << 16) | (1 << 10) | (1 << 4))

volatile int keep_running = 1;

void handle_sigint(int sig) { 
    keep_running = 0; 
}

// Function to export data and call the plotting script
void export_and_plot(volatile uint32_t* buffer, uint32_t trigger_index) {
    printf("Exporting data to waveform.csv...\n");
    FILE *fp = fopen("waveform.csv", "w");
    if (!fp) {
        printf("Error: Cannot open waveform.csv for writing.\n");
        return;
    }

    fprintf(fp, "Index,GPIO8,GPIO9\n");

    // We want to print the data chronologically.
    // The oldest data is right after the trigger_index in the circular buffer.
    uint32_t start_index = (trigger_index + 1) % FIXED_BUFFER_SAMPLES;
    
    for (int i = 0; i < FIXED_BUFFER_SAMPLES; i++) {
        uint32_t current = (start_index + i) % FIXED_BUFFER_SAMPLES;
        uint32_t val = buffer[current];
        
        int gpio8 = (val & (1 << 0)) ? 1 : 0;
        int gpio9 = (val & (1 << 1)) ? 1 : 0;
        
        // Mark the trigger point as index 0 for the graph X-axis
        int time_axis = i - (FIXED_BUFFER_SAMPLES / 2); 
        
        fprintf(fp, "%d,%d,%d\n", time_axis, gpio8, gpio9);
    }
    fclose(fp);

    printf("Plotting graph...\n");
    // Call the Python plotting script automatically
    system("python3 plot_waveform.py");
}

int main() {
    printf("--- SMI Logic Analyzer Mode ---\n");
    signal(SIGINT, handle_sigint);

    DmaBuffer cb_buf, sample_buf;
    SmiHardware smi_hw;
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    
    if (mem_fd < 0) return -1;

    uint32_t target_rate_hz = 7500000; 

    allocate_dma_buffer(&sample_buf, FIXED_BUFFER_BYTES);
    allocate_dma_buffer(&cb_buf, sizeof(struct DmaControlBlock));

    struct DmaControlBlock* cb = (struct DmaControlBlock*)cb_buf.virtual_addr;
    setup_dma_control_block(cb, DMA_TI_SMI_CAPTURE, 0x7E60000C, sample_buf.bus_addr, FIXED_BUFFER_BYTES);
    cb->nextconbk = cb_buf.bus_addr; 

    smi_init(&smi_hw, mem_fd);
    volatile uint32_t *dma_base = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, 0xFE007000);
    volatile uint32_t *dma_chan5 = dma_base + (0x500 / 4);

    // --- THE FIX: HARDWARE DMA RESET ---
    printf("Clearing DMA hardware ghosts...\n");
    dma_chan5[0] = (1 << 31); // Set the RESET bit in the DMA CS register
    usleep(1000);             // Give the silicon 1ms to flush its internal FIFOs
    dma_chan5[0] = 0;         // Clear the register completely
    // -----------------------------------

    start_dma_channel(dma_chan5, cb_buf.bus_addr);
    smi_start_capture(&smi_hw, 0xFFFFFFFF, target_rate_hz); 

    printf("Armed. Waiting for falling edge on GPIO8...\n");

    volatile uint32_t* samples = (volatile uint32_t*)sample_buf.virtual_addr;
    uint32_t last_index = 0;
    
    int triggered = 0;
    uint32_t trigger_idx = 0;
    uint32_t target_stop_idx = 0;

    while (keep_running) {
        uint32_t current_dest = dma_chan5[0x0C / 4]; 
        uint32_t current_index = (current_dest - sample_buf.bus_addr) / 4;

        while (last_index != current_index && keep_running) {
            uint32_t val = samples[last_index];
            
            if (!triggered) {
                // Looking for the trigger (GPIO8 goes low)
                if (!(val & (1 << 0))) {
                    triggered = 1;
                    trigger_idx = last_index;
                    // Set the stop target 1024 samples into the future
                    target_stop_idx = (last_index + (FIXED_BUFFER_SAMPLES / 2)) % FIXED_BUFFER_SAMPLES;
                    
                }
            } else {
                // We are triggered, wait until we hit the target stop index
                if (last_index == target_stop_idx) {
                    keep_running = 0; // Stop the main loop
                    break;
                }
            }

            last_index = (last_index + 1) % FIXED_BUFFER_SAMPLES;
        }
         
    }

    // Stop hardware immediately to freeze the buffer
    smi_stop_capture(&smi_hw);
    
    // THE FIX: Explicitly abort the continuous DMA hardware loop!
    stop_dma_channel(dma_chan5);

    if (triggered) {
        export_and_plot(samples, trigger_idx);
    } else {
        printf("\nCapture aborted before trigger was found.\n");
    }

    // Cleanup
    smi_cleanup(&smi_hw);
    if (dma_base != MAP_FAILED) munmap((void*)dma_base, 4096);
    close(mem_fd);
    free_dma_buffer(&sample_buf);
    free_dma_buffer(&cb_buf);
    
    return 0;
}