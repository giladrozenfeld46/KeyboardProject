#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h> // Required for high-resolution timing

#include "smi_manager.h"

#define CHUNK_SIZE 8
#define POST_TRIGGER_SAMPLES 1024

// Assuming D+ is connected to GPIO0 (Bit 0). Change the shift if it's on a different pin.
#define DPLUS_BIT_MASK (1 << 1) 
// Assuming D- is connected to GPIO8 (Bit 1)
#define DMINUS_BIT_MASK (1 << 0)

volatile int keep_running = 1;

void handle_sigint(int sig) {
    keep_running = 0;
}

void export_and_plot(uint32_t* buffer, int count) {
    printf("Exporting %d post-trigger samples...\n", count);
    FILE *fp = fopen("waveform.csv", "w");
    if (!fp) {
        printf("Error: Cannot create waveform.csv\n");
        return;
    }

    fprintf(fp, "Index,DPlus,DMinus\n");
    
    for (int i = 0; i < count; i++) {
        int dplus = (buffer[i] & DPLUS_BIT_MASK) ? 1 : 0;
        int dminus = (buffer[i] & DMINUS_BIT_MASK) ? 1 : 0;
        
        // Since the trigger is strictly before index 0 of this buffer, 
        // we can just plot from index 0 sequentially.
        fprintf(fp, "%d,%d,%d\n", i, dminus, dplus);
    }
    fclose(fp);

    printf("Plotting graph...\n");
    system("python3 plot_waveform.py");
}

int main() {
    printf("--- SMI Logic Analyzer: Trigger & Collect ---\n");
    signal(SIGINT, handle_sigint);
    
    // Initialize the SMI hardware (e.g., 2 MSPS)
    if (smi_manager_init(2000000) != 0) {
        return -1;
    }

    uint32_t chunk[CHUNK_SIZE];
    uint32_t post_trigger_buffer[POST_TRIGGER_SAMPLES];
    
    int collected = 0;
    int triggered = 0;
    
    uint32_t prev_sample = 0;
    int has_history = 0;

    // Variables for performance metrics
    struct timespec start_scan, end_scan;
    uint64_t total_scan_time_ns = 0;
    uint64_t total_samples_analyzed = 0;

    printf("Waiting for RISING EDGE on D+...\n");

    while (keep_running) {
        // Attempt to pull a chunk of 8 samples from the SMI manager
        if (smi_manager_read_chunk(chunk)) {
            
            // Start the timer for this chunk analysis
            clock_gettime(CLOCK_MONOTONIC, &start_scan);

            if (!triggered) {
                // STATE 1: Searching for the trigger
                for (int i = 0; i < CHUNK_SIZE; i++) {
                    uint32_t val = chunk[i];
                    
                    if (has_history) {
                        int prev_dplus = prev_sample & DPLUS_BIT_MASK;
                        int curr_dplus = val & DPLUS_BIT_MASK;
                        
                        // Detect Rising Edge: Previous was LOW, Current is HIGH
                        if (!prev_dplus && curr_dplus) {
                            triggered = 1;
                            printf("Triggered! Collecting next %d samples...\n", POST_TRIGGER_SAMPLES);
                            
                            // Start collecting samples immediately after the trigger point
                            // from the remaining elements in this current chunk
                            for (int j = i + 1; j < CHUNK_SIZE && collected < POST_TRIGGER_SAMPLES; j++) {
                                post_trigger_buffer[collected++] = chunk[j];
                            }
                            break; 
                        }
                    }
                    prev_sample = val;
                    has_history = 1;
                }
            } else {
                // STATE 2: Trigger occurred, copy the entire chunk to our buffer
                for (int i = 0; i < CHUNK_SIZE && collected < POST_TRIGGER_SAMPLES; i++) {
                    post_trigger_buffer[collected++] = chunk[i];
                }
            }

            // Stop the timer after the chunk is fully analyzed
            clock_gettime(CLOCK_MONOTONIC, &end_scan);
            
            // Accumulate scan time
            uint64_t ns_spent = (end_scan.tv_sec - start_scan.tv_sec) * 1000000000ULL + 
                                (end_scan.tv_nsec - start_scan.tv_nsec);
            total_scan_time_ns += ns_spent;
            total_samples_analyzed += CHUNK_SIZE;

            // Stop the main loop if we have collected exactly what we need
            if (collected >= POST_TRIGGER_SAMPLES) {
                break; 
            }

        } else {
            // No chunk ready yet. Small sleep to prevent 100% CPU usage.
            usleep(1);
        }
    }

    // Stop hardware and unmap memory
    smi_manager_cleanup();

    // Print Performance Metrics
    printf("\n--- Performance Metrics ---\n");
    if (total_samples_analyzed > 0) {
        double avg_scan_time = (double)total_scan_time_ns / total_samples_analyzed;
        printf("Average time to analyze a single sample: %.2f ns\n", avg_scan_time);
    }
    printf("Total samples analyzed: %llu\n", (unsigned long long)total_samples_analyzed);
    printf("---------------------------\n\n");

    // Plot if we successfully collected the post-trigger data
    if (triggered && collected > 0) {
        export_and_plot(post_trigger_buffer, collected);
    } else {
        printf("Aborted before trigger was found or collection was complete.\n");
    }

    return 0;
}