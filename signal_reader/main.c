#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "smi_manager.h"

#define CHUNK_SIZE 8
#define POST_TRIGGER_SYMBOLS 128

#define SAMPLES_PER_SYMBOL 6

// Assuming D+ is connected to GPIO9 (Bit 1). Change the shift if it's on a different pin.
#define DPLUS_BIT_MASK (1 << 1) 
// Assuming D- is connected to GPIO8 (Bit 0)
#define DMINUS_BIT_MASK (1 << 0)

volatile int keep_running = 1;

void handle_sigint(int sig) {
    keep_running = 0;
}

typedef struct {
    uint8_t dplus;
    uint8_t dminus;
} Symbol;

void export_and_plot(Symbol* buffer, int count) {
    printf("Exporting %d post-trigger symbols...\n", count);
    FILE *fp = fopen("waveform.csv", "w");
    if (!fp) {
        printf("Error: Cannot create waveform.csv\n");
        return;
    }

    fprintf(fp, "Index,DPlus,DMinus\n");
    
    for (int i = 0; i < count; i++) {
        fprintf(fp, "%d,%d,%d\n", i, buffer[i].dplus, buffer[i].dminus);
    }
    fclose(fp);

    printf("Plotting graph...\n");
    system("python3 plot_waveform.py");
}

/**
 * Exclusively pulls chunks from SMI Manager and extracts valid symbols.
 * Uses synchronization logic: waits for SAMPLES_PER_SYMBOL consecutive identical samples.
 * If a change occurs before reaching the threshold, the counter resets and ignores previous samples.
 */
int get_next_symbol(Symbol* out_symbol) {
    static uint32_t chunk[CHUNK_SIZE];
    static int chunk_idx = CHUNK_SIZE; // Initially empty to force a hardware read on first call
    static uint32_t current_val = 0xFFFFFFFF;
    static int consecutive_count = 0;

    while (keep_running) {
        // Fetch a new chunk if we have processed all elements in the current chunk
        if (chunk_idx >= CHUNK_SIZE) {
            while (keep_running && !smi_manager_read_chunk(chunk)) {
                usleep(1); // Wait for hardware to provide data
            }
            if (!keep_running) return 0;
            chunk_idx = 0;
        }

        uint32_t val = chunk[chunk_idx++];
        
        // Mask to focus only on D+ and D- bits
        uint32_t masked_val = val & (DPLUS_BIT_MASK | DMINUS_BIT_MASK);

        // Initialize state on the very first valid sample
        if (current_val == 0xFFFFFFFF) {
            current_val = masked_val;
            consecutive_count = 1;
            continue;
        }

        if (masked_val == current_val) {
            consecutive_count++;
            
            // Passed enough samples to be considered a stable symbol
            if (consecutive_count >= SAMPLES_PER_SYMBOL) {
                out_symbol->dplus = (masked_val & DPLUS_BIT_MASK) ? 1 : 0;
                out_symbol->dminus = (masked_val & DMINUS_BIT_MASK) ? 1 : 0;
                
                // Reset counter to correctly detect the next symbol 
                // (whether it's an identical repeating symbol or a new one)
                consecutive_count = 0; 
                return 1;
            }
        } else {
            // Signal changed before a full symbol period was reached.
            // Reset synchronization to align with the new edge and ignore previous unstable samples.
            current_val = masked_val;
            consecutive_count = 1;
        }
    }
    return 0;
}

int main() {
    printf("--- SMI Logic Analyzer: Symbol Synchronizer & Decoder ---\n");
    signal(SIGINT, handle_sigint);
    
    // Initialize the SMI hardware (e.g., 2 MSPS)
    if (smi_manager_init(2000000) != 0) {
        return -1;
    }

    Symbol post_trigger_buffer[POST_TRIGGER_SYMBOLS];
    
    int collected = 0;
    int triggered = 0;
    Symbol sym;

    printf("Waiting for RISING EDGE on D+ (Symbol Level)...\n");

    while (keep_running) {
        // get_next_symbol is now the EXCLUSIVE consumer of smi_manager_read_chunk
        if (get_next_symbol(&sym)) {
            
            if (!triggered) {
                // STATE 1: Searching for the trigger (D+ goes HIGH)
                if (sym.dplus == 1) {
                    triggered = 1;
                    printf("Triggered! Collecting next %d symbols...\n", POST_TRIGGER_SYMBOLS);
                    post_trigger_buffer[collected++] = sym;
                }
            } else {
                // STATE 2: Trigger occurred, collect decoded symbols sequentially
                post_trigger_buffer[collected++] = sym;
                
                // Stop the main loop if we have collected exactly what we need
                if (collected >= POST_TRIGGER_SYMBOLS) {
                    break; 
                }
            }
        }
    }

    // Stop hardware and unmap memory safely
    smi_manager_cleanup();

    // Plot and print if we successfully collected the post-trigger data
    if (triggered && collected > 0) {
        export_and_plot(post_trigger_buffer, collected);
        
        printf("\n--- Decoded Symbols ---\n");
        for (int i = 0; i < collected; i++) {
            printf("Symbol %3d: D+ = %d, D- = %d\n", i, post_trigger_buffer[i].dplus, post_trigger_buffer[i].dminus);
        }
        printf("--------------------------\n");
        
    } else {
        printf("Aborted before trigger was found or collection was complete.\n");
    }

    return 0;
}