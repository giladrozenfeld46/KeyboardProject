#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h> 
#include <string.h> 

#include "smi_manager.h"
#include "state_machine.h"
#include "usb_decoder.h" // Added USB Packet Decoder

#define CHUNK_SIZE 8

// Symbol extraction configuration based on consecutive samples
#define TARGET_SYMBOL_SAMPLES 6
#define MIN_SYMBOL_SAMPLES 4

// Debug configurations
#define DEBUG_SAMPLES_COUNT 1024 

// Assuming D+ is connected to GPIO9 (Bit 1). Change the shift if it's on a different pin.
#define DPLUS_BIT_MASK (1 << 1) 
// Assuming D- is connected to GPIO8 (Bit 0)
#define DMINUS_BIT_MASK (1 << 0)

volatile int keep_running = 1;

void handle_sigint(int sig) {
    keep_running = 0;
}

/* =========================================================
 * CORE HARDWARE SAMPLING & DIGITAL FILTERING
 * ========================================================= */

/**
 * Exclusively pulls chunks from SMI Manager and extracts valid symbols.
 * Uses a digital filter logic to discard noise and handle line skew.
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
            
            if (consecutive_count >= TARGET_SYMBOL_SAMPLES) {
                out_symbol->dplus = (current_val & DPLUS_BIT_MASK) ? 1 : 0;
                out_symbol->dminus = (current_val & DMINUS_BIT_MASK) ? 1 : 0;
                consecutive_count = 0; 
                return 1;
            }
        } else {
            // Signal edge detected
            if (consecutive_count >= MIN_SYMBOL_SAMPLES) {
                out_symbol->dplus = (current_val & DPLUS_BIT_MASK) ? 1 : 0;
                out_symbol->dminus = (current_val & DMINUS_BIT_MASK) ? 1 : 0;
                current_val = masked_val;
                consecutive_count = 1;
                return 1;
            } else {
                current_val = masked_val;
                consecutive_count = 1;
            }
        }
    }
    return 0;
}

/* =========================================================
 * EXPORT, PLOTTING, AND PRINTING UTILITIES
 * ========================================================= */

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
        fprintf(fp, "%d,%d,%d\n", i, dplus, dminus);
    }
    fclose(fp);

    printf("Plotting raw sample graph...\n");
    system("python3 plot_waveform.py");
}

void export_and_plot_symbols(Symbol* buffer, int count) {
    printf("Exporting %d symbols to CSV...\n", count);
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

void print_decoded_bits(uint8_t* bits, int count) {
    printf("\n--- Decoded NRZI Bits (%d bits) ---\n", count);
    for (int i = 0; i < count; i++) {
        printf("%d", bits[i]);
        if ((i + 1) % 8 == 0) printf(" "); 
        if ((i + 1) % 64 == 0) printf("\n"); 
    }
    printf("\n-----------------------------------\n");
}

void print_performance_metrics(uint64_t total_time, uint64_t total_symbols) {
    printf("\n--- Performance Metrics ---\n");
    printf("Total symbols processed: %llu\n", (unsigned long long)total_symbols);
    if (total_symbols > 0) {
        double avg_time = (double)total_time / total_symbols;
        printf("Average processing time per symbol: %.2f ns\n", avg_time);
    }
    printf("---------------------------\n");
}

/* =========================================================
 * DEBUG 1: RAW SAMPLES & OFFLINE EXTRACTION
 * ========================================================= */

int collect_raw_samples(uint32_t* sample_buffer) {
    uint32_t chunk[CHUNK_SIZE];
    int collected = 0;
    int triggered = 0;
    
    while (keep_running && collected < DEBUG_SAMPLES_COUNT) {
        if (smi_manager_read_chunk(chunk)) {
            if (!triggered) {
                for (int i = 0; i < CHUNK_SIZE; i++) {
                    if (chunk[i] & DPLUS_BIT_MASK) {
                        triggered = 1;
                        printf("Initial D+ HIGH detected! Collecting raw samples...\n");
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
    return collected;
}

int extract_symbols_offline(uint32_t* sample_buffer, int collected, Symbol* debug_symbols) {
    int sym_count = 0;
    uint32_t current_val = 0xFFFFFFFF;
    int consecutive_count = 0; 
    
    for (int i = 0; i < collected; i++) {
        uint32_t masked_val = sample_buffer[i] & (DPLUS_BIT_MASK | DMINUS_BIT_MASK);
        
        if (current_val == 0xFFFFFFFF) {
            current_val = masked_val;
            consecutive_count = 1;
            continue;
        }
        
        if (masked_val == current_val) {
            consecutive_count++;
            if (consecutive_count >= TARGET_SYMBOL_SAMPLES) {
                debug_symbols[sym_count].dplus = (current_val & DPLUS_BIT_MASK) ? 1 : 0;
                debug_symbols[sym_count].dminus = (current_val & DMINUS_BIT_MASK) ? 1 : 0;
                sym_count++;
                consecutive_count = 0; 
                if (sym_count >= DEBUG_SYMBOLS_COUNT) break;
            }
        } else {
            if (consecutive_count >= MIN_SYMBOL_SAMPLES) {
                debug_symbols[sym_count].dplus = (current_val & DPLUS_BIT_MASK) ? 1 : 0;
                debug_symbols[sym_count].dminus = (current_val & DMINUS_BIT_MASK) ? 1 : 0;
                sym_count++;
                current_val = masked_val;
                consecutive_count = 1;
                if (sym_count >= DEBUG_SYMBOLS_COUNT) break;
            } else {
                current_val = masked_val;
                consecutive_count = 1;
            }
        }
    }
    return sym_count;
}

void run_debug1_fast_path() {
    printf("DEBUG 1 ENABLED: Waiting for initial D+ HIGH to collect %d raw samples.\n", DEBUG_SAMPLES_COUNT);
    uint32_t sample_buffer[DEBUG_SAMPLES_COUNT];
    
    int collected = collect_raw_samples(sample_buffer);
    smi_manager_cleanup(); // Free hardware early, processing is now offline
    
    if (collected > 0) {
        printf("\nNOTE: The program will plot the raw samples first.\n");
        printf("CLOSE THE PLOT WINDOW to proceed to the symbol extraction graph!\n\n");
        export_and_plot_samples(sample_buffer, collected);
        
        printf("\n--- Extracting Symbols from Captured Raw Samples ---\n");
        Symbol debug_symbols[DEBUG_SYMBOLS_COUNT];
        int sym_count = extract_symbols_offline(sample_buffer, collected, debug_symbols);
        
        if (sym_count > 0) {
            printf("Successfully extracted %d symbols. Plotting symbol graph...\n", sym_count);
            export_and_plot_symbols(debug_symbols, sym_count);
        } else {
            printf("Could not extract any stable symbols from the raw samples.\n");
        }
    } else {
        printf("Aborted before collection finished.\n");
    }
}

/* =========================================================
 * MAIN STATE MACHINE DECODER FLOW
 * ========================================================= */

void output_decoder_results(int debug_mode, Symbol* debug_buffer, int debug_count, uint8_t* bit_buffer, int bit_count) {
    if (debug_mode == 2) {
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
    } else if (debug_mode == -1) {
        // Only print if there's an incomplete packet interrupted by Ctrl+C
        if (bit_count > 0) {
            printf("\n[Interrupted] Incomplete packet data (%d bits):\n", bit_count);
            print_decoded_bits(bit_buffer, bit_count);
        }
    }
}

void run_main_decoder_loop(int debug_mode) {
    DecoderState state = STATE_WAIT_ACTIVITY;
    Symbol sym;
    Symbol sync_buffer[8] = {{0, 0}}; 
    
    // Usually in USB (Full Speed): K J K J K J K K
    Symbol expected_sync[8] = {
        {1, 0}, {0, 1}, {1, 0}, {0, 1}, 
        {1, 0}, {0, 1}, {1, 0}, {1, 0}
    };

    uint8_t bit_buffer[MAX_DECODED_BITS];
    int bit_count = 0;
    Symbol prev_symbol = {0, 0};

    Symbol debug_buffer[DEBUG_SYMBOLS_COUNT];
    int debug_count = 0;

    struct timespec start_time, end_time;
    uint64_t total_analyze_time_ns = 0;
    uint64_t symbols_analyzed_count = 0;

    if (debug_mode == 2) {
        printf("STATE: WAIT ACTIVITY. DEBUG 2 ENABLED: Waiting for SYNC.\n");
    } else {
        printf("STATE: WAIT ACTIVITY. Waiting for line activity...\n");
    }

    while (keep_running) {
        if (get_next_symbol(&sym)) {
            clock_gettime(CLOCK_MONOTONIC, &start_time);
            
            switch (state) {
                case STATE_WAIT_ACTIVITY:
                    state = handle_state_wait_activity(sym, sync_buffer);
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
                    {
                        DecoderState next_state = handle_state_analyze(sym, &prev_symbol, bit_buffer, &bit_count, &keep_running);
                        
                        // Check if we finished reading the current packet (SE0 detected)
                        if (next_state == STATE_WAIT_ACTIVITY) {
                            
                            // Send the collected bits directly to the USB decoder right now!
                            if (bit_count > 0) {
                                print_decoded_bits(bit_buffer, bit_count);
                                analyze_usb_packet(bit_buffer, bit_count, 0); 
                            }
                            
                            bit_count = 0;    // Reset the bit counter for the next packet
                            keep_running = 1; // Override the stop signal to CONTINUE listening for more packets!
                        }
                        
                        state = next_state;
                    }
                    break;

                case STATE_DEBUG:
                    state = handle_state_debug(sym, debug_buffer, &debug_count, &keep_running);
                    break;
            }
            
            clock_gettime(CLOCK_MONOTONIC, &end_time);
            
            uint64_t ns_spent = (end_time.tv_sec - start_time.tv_sec) * 1000000000ULL + 
                                (end_time.tv_nsec - start_time.tv_nsec);
            total_analyze_time_ns += ns_spent;
            symbols_analyzed_count++;
        }
    }

    smi_manager_cleanup();
    print_performance_metrics(total_analyze_time_ns, symbols_analyzed_count);
    output_decoder_results(debug_mode, debug_buffer, debug_count, bit_buffer, bit_count);
}

/* =========================================================
 * APPLICATION ENTRY POINT
 * ========================================================= */

int parse_arguments(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug1") == 0) return 1;
        if (strcmp(argv[i], "--debug2") == 0) return 2;
    }
    return -1; // Normal Mode
}

int main(int argc, char *argv[]) {
    printf("--- SMI Logic Analyzer: Multi-Stage Packet Decoder ---\n");
    signal(SIGINT, handle_sigint);
    
    int debug_mode = parse_arguments(argc, argv);

    if (smi_manager_init(2000000) != 0) {
        return -1;
    }

    if (debug_mode == 1) {
        run_debug1_fast_path();
    } else {
        run_main_decoder_loop(debug_mode);
    }

    return 0;
}