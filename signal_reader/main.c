#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h> 
#include <string.h> 

#include "smi_manager.h"

#define CHUNK_SIZE 8
#define MAX_DECODED_BITS 2048

#define SAMPLES_PER_SYMBOL 6

// Debug configurations
#define DEBUG_SYMBOLS_COUNT 128 
#define DEBUG_SAMPLES_COUNT 1024 

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

// Define the State Machine states
typedef enum {
    STATE_WAIT_ACTIVITY, // Wait for unusual voltage (D+ HIGH or D- LOW)
    STATE_SYNC_SEARCH,   // Wait for the 8-symbol SYNC pattern
    STATE_ANALYZE,       // Decode NRZI to bits
    STATE_DEBUG          // Collect raw symbols for debugging
} DecoderState;

/**
 * Exclusively pulls chunks from SMI Manager and extracts valid symbols.
 * Uses synchronization logic: waits for SAMPLES_PER_SYMBOL consecutive identical samples.
 */
int get_next_symbol(Symbol* out_symbol) {
    static uint32_t chunk[CHUNK_SIZE];
    static int chunk_idx = CHUNK_SIZE; 
    static uint32_t current_val = 0xFFFFFFFF;
    static int consecutive_count = 0;

    while (keep_running) {
        if (chunk_idx >= CHUNK_SIZE) {
            while (keep_running && !smi_manager_read_chunk(chunk)) {
                usleep(1); 
            }
            if (!keep_running) return 0;
            chunk_idx = 0;
        }

        uint32_t val = chunk[chunk_idx++];
        uint32_t masked_val = val & (DPLUS_BIT_MASK | DMINUS_BIT_MASK);

        if (current_val == 0xFFFFFFFF) {
            current_val = masked_val;
            consecutive_count = 1;
            continue;
        }

        if (masked_val == current_val) {
            consecutive_count++;
            
            if (consecutive_count >= SAMPLES_PER_SYMBOL) {
                out_symbol->dplus = (masked_val & DPLUS_BIT_MASK) ? 1 : 0;
                out_symbol->dminus = (masked_val & DMINUS_BIT_MASK) ? 1 : 0;
                consecutive_count = 0; 
                return 1;
            }
        } else {
            current_val = masked_val;
            consecutive_count = 1;
        }
    }
    return 0;
}

/**
 * Helper to check if two symbols are logically identical
 */
int is_symbol_equal(Symbol a, Symbol b) {
    return (a.dplus == b.dplus) && (a.dminus == b.dminus);
}

/**
 * Exports collected raw samples (for debug0) to a CSV file and plots them
 */
void export_and_plot_samples(uint32_t* buffer, int count) {
    printf("Exporting %d raw samples to CSV...\n", count);
    FILE *fp = fopen("waveform.csv", "w");
    if (!fp) {
        printf("Error: Cannot create waveform.csv\n");
        return;
    }

    fprintf(fp, "Index,DPlus,DMinus\n");
    for (int i = 0; i < count; i++) {
        int dplus = (buffer[i] & DPLUS_BIT_MASK) ? 1 : 0;
        int dminus = (buffer[i] & DMINUS_BIT_MASK) ? 1 : 0;
        fprintf(fp, "%d,%d,%d\n", i, dminus, dplus);
    }
    fclose(fp);

    printf("Plotting sample graph...\n");
    system("python3 plot_waveform.py");
}

/**
 * Exports the collected symbols to a CSV file and plots them
 */
void export_and_plot_symbols(Symbol* buffer, int count) {
    printf("Exporting %d debug symbols to CSV...\n", count);
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

    printf("Plotting symbol graph...\n");
    system("python3 plot_waveform.py");
}

/**
 * Prints the collected bits array in groups of 8 for readability
 */
void print_decoded_bits(uint8_t* bits, int count) {
    printf("\n--- Decoded NRZI Bits (%d bits) ---\n", count);
    for (int i = 0; i < count; i++) {
        printf("%d", bits[i]);
        if ((i + 1) % 8 == 0) {
            printf(" "); 
        }
        if ((i + 1) % 64 == 0) {
            printf("\n"); 
        }
    }
    printf("\n-----------------------------------\n");
}

/**
 * Handles the DEBUG 0 mode (Fast Path)
 * Bypasses symbol extraction and saves raw samples triggered by D+ HIGH
 */
void run_debug0_fast_path() {
    printf("DEBUG 0 ENABLED: Waiting for initial D+ HIGH to collect %d raw samples.\n", DEBUG_SAMPLES_COUNT);
    
    uint32_t chunk[CHUNK_SIZE];
    uint32_t sample_buffer[DEBUG_SAMPLES_COUNT];
    int collected = 0;
    int triggered = 0;
    
    while (keep_running && collected < DEBUG_SAMPLES_COUNT) {
        if (smi_manager_read_chunk(chunk)) {
            if (!triggered) {
                for (int i = 0; i < CHUNK_SIZE; i++) {
                    if (chunk[i] & DPLUS_BIT_MASK) {
                        triggered = 1;
                        printf("Initial D+ HIGH detected! Collecting raw samples...\n");
                        // Collect the rest of the current chunk
                        for (int j = i; j < CHUNK_SIZE && collected < DEBUG_SAMPLES_COUNT; j++) {
                            sample_buffer[collected++] = chunk[j];
                        }
                        break; 
                    }
                }
            } else {
                for (int i = 0; i < CHUNK_SIZE && collected < DEBUG_SAMPLES_COUNT; i++) {
                    sample_buffer[collected++] = chunk[i];
                }
            }
        } else {
            usleep(1);
        }
    }
    
    smi_manager_cleanup();
    if (collected > 0) {
        export_and_plot_samples(sample_buffer, collected);
    } else {
        printf("Aborted before collection finished.\n");
    }
}

/**
 * Handles the WAIT_ACTIVITY state logic.
 * Looks for an unusual voltage state (D+ is HIGH or D- is LOW) to start analysis.
 */
DecoderState handle_state_wait_activity(Symbol sym, int debug_mode) {
    // Check for unusual activity: D+ going high OR D- going low
    if (sym.dplus == 1 || sym.dminus == 0) {
        printf("Unusual line activity detected (D+ HIGH or D- LOW)!\n");
        
        if (debug_mode == 1) {
            printf("Switching to DEBUG state for symbol collection...\n");
            return STATE_DEBUG; 
        }
        
        printf("Switching to SYNC SEARCH state...\n");
        return STATE_SYNC_SEARCH;
    }
    
    return STATE_WAIT_ACTIVITY; // Keep waiting
}

/**
 * Handles the SYNC_SEARCH state logic. 
 * Looks for the SYNC pattern in the incoming symbols.
 */
DecoderState handle_state_sync_search(Symbol sym, Symbol* sync_buffer, const Symbol* expected_sync, Symbol* out_prev_symbol, int debug_mode) {
    // Shift the new symbol into the 8-symbol buffer
    for (int i = 0; i < 7; i++) {
        sync_buffer[i] = sync_buffer[i+1];
    }
    sync_buffer[7] = sym;

    // Check if the current buffer matches the expected sequence
    int match = 1;
    for (int i = 0; i < 8; i++) {
        if (!is_symbol_equal(sync_buffer[i], expected_sync[i])) {
            match = 0;
            break;
        }
    }

    if (match) {
        printf("SYNC pattern detected!\n");
        // The reference for the first NRZI comparison is the last symbol of the SYNC
        *out_prev_symbol = sync_buffer[7]; 
        
        if (debug_mode == 2) {
            printf("Switching to DEBUG state...\n");
            return STATE_DEBUG;
        }
        printf("Switching to ANALYZE state...\n");
        return STATE_ANALYZE; 
    }

    return STATE_SYNC_SEARCH; // Keep looking for SYNC
}

/**
 * Handles the DEBUG state logic.
 * Collects a specified amount of raw symbols for debugging purposes.
 */
DecoderState handle_state_debug(Symbol sym, Symbol* debug_buffer, int* debug_count) {
    debug_buffer[(*debug_count)++] = sym;

    // Stop if we reached the desired count or detected SE0 (End of Packet)
    if (*debug_count >= DEBUG_SYMBOLS_COUNT) {
        if (sym.dplus == 0 && sym.dminus == 0) {
            printf("SE0 (End of Packet) detected during debug collection.\n");
        } else {
            printf("Collected %d debug symbols.\n", *debug_count);
        }
        keep_running = 0; // Signal the main loop to stop
        return STATE_WAIT_ACTIVITY;
    }

    return STATE_DEBUG; // Stay in debug mode
}

/**
 * Handles the ANALYZE state logic.
 * Decodes NRZI symbols into bits and detects End of Packet (SE0).
 */
DecoderState handle_state_analyze(Symbol sym, Symbol* prev_symbol, uint8_t* bit_buffer, int* bit_count) {
    // End of Packet Detection: Both lines LOW (SE0)
    if (sym.dplus == 0 && sym.dminus == 0) {
        printf("SE0 (End of Packet) detected! Stopping analysis.\n");
        keep_running = 0; // Signal the main loop to stop
        return STATE_WAIT_ACTIVITY;
    }

    // NRZI Decoding Logic
    uint8_t current_bit;
    if (!is_symbol_equal(sym, *prev_symbol)) {
        current_bit = 0;
    } else {
        current_bit = 1;
    }

    // Save the decoded bit
    if (*bit_count < MAX_DECODED_BITS) {
        bit_buffer[(*bit_count)++] = current_bit;
    } else {
        printf("Warning: Max bit buffer reached! Stopping early.\n");
        keep_running = 0;
    }

    // Update previous symbol for the next iteration
    *prev_symbol = sym;
    
    return STATE_ANALYZE; // Stay in analysis mode
}

int main(int argc, char *argv[]) {
    printf("--- SMI Logic Analyzer: Multi-Stage Packet Decoder ---\n");
    signal(SIGINT, handle_sigint);
    
    // -1 = Normal Mode, 0 = Raw Samples, 1 = Raw Symbols on Activity, 2 = Raw Symbols after SYNC
    int debug_mode = -1; 
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug0") == 0) {
            debug_mode = 0;
            break;
        } else if (strcmp(argv[i], "--debug1") == 0) {
            debug_mode = 1;
            break;
        } else if (strcmp(argv[i], "--debug2") == 0) {
            debug_mode = 2;
            break;
        }
    }

    if (smi_manager_init(2000000) != 0) {
        return -1;
    }

    // Fast path for raw sample capture (bypasses state machine completely)
    if (debug_mode == 0) {
        run_debug0_fast_path();
        return 0;
    }

    DecoderState state = STATE_WAIT_ACTIVITY;
    Symbol sym;
    
    // Arrays and trackers for SYNC_SEARCH state
    Symbol sync_buffer[8] = {{0, 0}}; 
    
    // Usually in USB (Full Speed): K J K J K J K K
    Symbol expected_sync[8] = {
        {0, 1}, {1, 0}, {0, 1}, {1, 0}, 
        {0, 1}, {1, 0}, {0, 1}, {0, 1}
    };

    // Arrays and trackers for ANALYZE state
    uint8_t bit_buffer[MAX_DECODED_BITS];
    int bit_count = 0;
    Symbol prev_symbol = {0, 0};

    // Arrays and trackers for DEBUG state
    Symbol debug_buffer[DEBUG_SYMBOLS_COUNT];
    int debug_count = 0;

    // Performance metrics variables
    struct timespec start_time, end_time;
    uint64_t total_analyze_time_ns = 0;
    uint64_t symbols_analyzed_count = 0;

    if (debug_mode == 1) {
        printf("STATE: WAIT ACTIVITY. DEBUG 1 ENABLED: Waiting for activity to collect %d symbols.\n", DEBUG_SYMBOLS_COUNT);
    } else if (debug_mode == 2) {
        printf("STATE: WAIT ACTIVITY. DEBUG 2 ENABLED: Waiting for SYNC pattern to collect %d symbols.\n", DEBUG_SYMBOLS_COUNT);
    } else {
        printf("STATE: WAIT ACTIVITY. Waiting for line activity...\n");
    }

    // The clean main loop
    while (keep_running) {
        if (get_next_symbol(&sym)) {
            
            // Start measuring the state machine processing time
            clock_gettime(CLOCK_MONOTONIC, &start_time);
            
            switch (state) {
                case STATE_WAIT_ACTIVITY:
                    state = handle_state_wait_activity(sym, debug_mode);
                    if (state == STATE_DEBUG) {
                        debug_count = 0;
                    }
                    break;

                case STATE_SYNC_SEARCH:
                    state = handle_state_sync_search(sym, sync_buffer, expected_sync, &prev_symbol, debug_mode);
                    if (state == STATE_DEBUG) {
                        debug_count = 0;
                    } else if (state == STATE_ANALYZE) {
                        bit_count = 0; 
                    }
                    break;
                    
                case STATE_ANALYZE:
                    state = handle_state_analyze(sym, &prev_symbol, bit_buffer, &bit_count);
                    break;

                case STATE_DEBUG:
                    state = handle_state_debug(sym, debug_buffer, &debug_count);
                    break;
            }
            
            // Stop measuring the processing time
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            
            uint64_t ns_spent = (end_time.tv_sec - start_time.tv_sec) * 1000000000ULL + 
                                (end_time.tv_nsec - start_time.tv_nsec);
            total_analyze_time_ns += ns_spent;
            symbols_analyzed_count++;
            
        }
    }

    // Stop hardware and unmap memory safely
    smi_manager_cleanup();

    // Print Performance Metrics
    printf("\n--- Performance Metrics ---\n");
    printf("Total symbols processed: %llu\n", (unsigned long long)symbols_analyzed_count);
    if (symbols_analyzed_count > 0) {
        double avg_analyze_time = (double)total_analyze_time_ns / symbols_analyzed_count;
        printf("Average processing time per symbol: %.2f ns\n", avg_analyze_time);
    }
    printf("---------------------------\n");

    // Output results based on the chosen mode
    if (debug_mode == 1 || debug_mode == 2) {
        if (debug_count > 0) {
            printf("\n--- Debug: Raw Symbols (%d) ---\n", debug_count);
            for (int i = 0; i < debug_count; i++) {
                printf("Symbol %3d: D+ = %d, D- = %d\n", i, debug_buffer[i].dplus, debug_buffer[i].dminus);
            }
            printf("-------------------------------\n");
            export_and_plot_symbols(debug_buffer, debug_count);
        } else {
            printf("No debug symbols were collected.\n");
        }
    } else {
        if (bit_count > 0) {
            print_decoded_bits(bit_buffer, bit_count);
        } else {
            printf("No data was decoded.\n");
        }
    }

    return 0;
}