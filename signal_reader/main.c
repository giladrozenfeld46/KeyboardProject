#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h> // Required for high-resolution timing
#include <string.h> // Required for strcmp

#include "smi_manager.h"

#define CHUNK_SIZE 8
#define MAX_DECODED_BITS 2048

#define SAMPLES_PER_SYMBOL 6

// Debug configurations
#define DEBUG_SYMBOLS_COUNT 128 // Number of raw symbols to collect in debug mode

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
    STATE_IDLE,
    STATE_ANALYZE,
    STATE_DEBUG // New debug state for raw symbol collection
} DecoderState;

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
 * Exports the collected symbols to a CSV file and plots them using a Python script
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

    printf("Plotting graph...\n");
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
            printf(" "); // Space after every byte
        }
        if ((i + 1) % 64 == 0) {
            printf("\n"); // Newline after 64 bits
        }
    }
    printf("\n-----------------------------------\n");
}

/**
 * Handles the IDLE state logic. 
 * Looks for the SYNC pattern in the incoming symbols.
 */
DecoderState handle_state_idle(Symbol sym, Symbol* sync_buffer, const Symbol* expected_sync, Symbol* out_prev_symbol) {
    // Shift the new symbol into the 8-symbol buffer (Shift Register)
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
        printf("SYNC pattern detected! Switching state...\n");
        // The reference for the first NRZI comparison is the last symbol of the SYNC
        *out_prev_symbol = sync_buffer[7]; 
        return STATE_ANALYZE; // The main loop will decide if we go to ANALYZE or DEBUG based on the macro
    }

    return STATE_IDLE; // Keep waiting
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
        return STATE_IDLE;
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
        return STATE_IDLE;
    }

    // NRZI Decoding Logic:
    // Transition (change) -> 0
    // No transition (same) -> 1
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
    printf("--- SMI Logic Analyzer: Modular State Machine Decoder ---\n");
    signal(SIGINT, handle_sigint);
    
    int debug_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = 1;
            break;
        }
    }

    if (smi_manager_init(2000000) != 0) {
        return -1;
    }

    DecoderState state = STATE_IDLE;
    Symbol sym;
    
    // Arrays and trackers for IDLE state
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

    printf("STATE: IDLE. Waiting for 8-symbol SYNC pattern...\n");
    if (debug_mode) {
        printf("DEBUG MODE ENABLED: Will collect %d raw symbols after SYNC.\n", DEBUG_SYMBOLS_COUNT);
    }

    // The clean main loop
    while (keep_running) {
        if (get_next_symbol(&sym)) {
            
            // Start measuring the state machine processing time
            clock_gettime(CLOCK_MONOTONIC, &start_time);
            
            switch (state) {
                case STATE_IDLE:
                    // If handle_state_idle returns STATE_ANALYZE, it means we found the SYNC
                    if (handle_state_idle(sym, sync_buffer, expected_sync, &prev_symbol) == STATE_ANALYZE) {
                        if (debug_mode) {
                            state = STATE_DEBUG;
                            debug_count = 0;
                            printf("Switching to DEBUG state.\n");
                        } else {
                            state = STATE_ANALYZE;
                            bit_count = 0; // Reset bit counter upon entering ANALYZE
                            printf("Switching to ANALYZE state.\n");
                        }
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
    printf("Total symbols analyzed: %llu\n", (unsigned long long)symbols_analyzed_count);
    if (symbols_analyzed_count > 0) {
        double avg_analyze_time = (double)total_analyze_time_ns / symbols_analyzed_count;
        printf("Average time to analyze a symbol (State Machine logic): %.2f ns\n", avg_analyze_time);
    }
    printf("---------------------------\n");

    // Output results based on the chosen mode
    if (debug_mode) {
        if (debug_count > 0) {
            printf("\n--- Debug: Raw Symbols (%d) ---\n", debug_count);
            for (int i = 0; i < debug_count; i++) {
                printf("Symbol %3d: D+ = %d, D- = %d\n", i, debug_buffer[i].dplus, debug_buffer[i].dminus);
            }
            printf("-------------------------------\n");
            
            // Export to CSV and plot the graph
            export_and_plot_symbols(debug_buffer, debug_count);
        } else {
            printf("No debug symbols were collected.\n");
        }
    } else {
        // Print the extracted bits if we successfully captured any in normal mode
        if (bit_count > 0) {
            print_decoded_bits(bit_buffer, bit_count);
        } else {
            printf("No data was decoded.\n");
        }
    }

    return 0;
}