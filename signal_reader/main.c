#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h> 
#include <string.h> 

#include "smi_manager.h"
#include "state_machine.h"
#include "usb_decoder.h" 

#define CHUNK_SIZE 8

// Symbol extraction configuration based on consecutive samples
#define TARGET_SYMBOL_SAMPLES 6
#define MIN_SYMBOL_SAMPLES 4

// Debug and Recording configurations
#define DEBUG_SAMPLES_COUNT 1024 
#define MAX_RECORDED_PACKETS 10000 // Maximum number of transactions to hold in RAM before writing

// Assuming D+ is connected to GPIO9 (Bit 1). Change the shift if it's on a different pin.
#define DPLUS_BIT_MASK (1 << 1) 
// Assuming D- is connected to GPIO8 (Bit 0)
#define DMINUS_BIT_MASK (1 << 0)

volatile int keep_running = 1;

void handle_sigint(int sig) {
    keep_running = 0;
}

// Structure to hold a complete paired transaction (Token + Data) in RAM
typedef struct {
    uint8_t token_bits[MAX_DECODED_BITS];
    int token_bit_count;
    uint8_t data_bits[MAX_DECODED_BITS];
    int data_bit_count;
} RecordedTransaction;

/* =========================================================
 * CORE HARDWARE SAMPLING & DIGITAL FILTERING
 * ========================================================= */

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

void print_decoded_bits(const uint8_t* bits, int count) {
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

/**
 * Formats and writes the paired Token and Data packets nicely into the text file.
 */
void flush_records_to_file(RecordedTransaction* records, int count) {
    if (count == 0) return;
    
    printf("\nSaving %d paired transactions to 'recorded_packets.txt'...\n", count);
    FILE *fp = fopen("recorded_packets.txt", "w");
    if (!fp) {
        printf("Error: Could not open recorded_packets.txt for writing.\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        fprintf(fp, "=== Transaction #%d ===\n", i + 1);
        
        // Print Token line (if exists)
        if (records[i].token_bit_count > 0) {
            UsbPacket token_info = analyze_usb_packet(records[i].token_bits, records[i].token_bit_count);
            fprintf(fp, "[TOKEN] PID: %-5s | ADDR: %-3d | ENDP: %-2d | CRC5:  0x%02X (%s)\n",
                    token_info.pid_name, token_info.addr, token_info.endp, 
                    token_info.crc5_received, token_info.crc5_valid ? "VALID" : "INVALID");
        }

        // Print Data line (if exists)
        if (records[i].data_bit_count > 0) {
            UsbPacket data_info = analyze_usb_packet(records[i].data_bits, records[i].data_bit_count);
            fprintf(fp, "[DATA]  PID: %-5s | CRC16: 0x%04X (%s) | PAYLOAD: ",
                    data_info.pid_name, data_info.crc16_received, data_info.crc16_valid ? "VALID" : "INVALID");
            
            // Reconstruct payload bytes from bits for compact printing
            for(int j = 0; j < data_info.data_bits_len; j += 8) {
                int bits_to_read = (data_info.data_bits_len - j >= 8) ? 8 : (data_info.data_bits_len - j);
                uint32_t byte_val = 0;
                for (int k = 0; k < bits_to_read; k++) {
                    if (data_info.data_bits[j + k]) {
                        byte_val |= (1 << k);
                    }
                }
                fprintf(fp, "%02X ", byte_val);
            }
            fprintf(fp, "\n");
        }
        
        fprintf(fp, "\n");
    }

    fclose(fp);
    printf("Successfully saved records to file.\n");
}

void run_main_decoder_loop(int debug_mode, int record_mode) {
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
    int consecutive_ones = 0;

    Symbol debug_buffer[DEBUG_SYMBOLS_COUNT];
    int debug_count = 0;
    
    // RAM buffer for recording multiple paired transactions before file I/O
    RecordedTransaction* transaction_record_buffer = NULL;
    int recorded_transaction_count = 0;
    
    // Variables for tracking IN/OUT token pairs
    uint8_t pending_token_bits[MAX_DECODED_BITS];
    int pending_token_bit_count = 0;
    int has_pending_token = 0;

    if (record_mode) {
        transaction_record_buffer = malloc(sizeof(RecordedTransaction) * MAX_RECORDED_PACKETS);
        if (!transaction_record_buffer) {
            printf("Warning: Could not allocate memory for transaction recording! Recording disabled.\n");
            record_mode = 0;
        } else {
            printf("RECORD MODE ENABLED: Capturing up to %d paired transactions in RAM.\n", MAX_RECORDED_PACKETS);
        }
    }

    struct timespec start_time, end_time;
    uint64_t total_analyze_time_ns = 0;
    uint64_t symbols_analyzed_count = 0;

    if (debug_mode == 2) {
        printf("STATE: WAIT ACTIVITY. DEBUG 2 ENABLED: Waiting for SYNC.\n");
    } else {
        printf("STATE: WAIT ACTIVITY. Waiting for line activity...\n");
        if (!record_mode) {
            printf("(Tip: Printing to screen takes time. Use --record for high-speed continuous capture).\n");
        }
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
                        consecutive_ones = 0; 
                    }
                    break;
                    
                case STATE_ANALYZE:
                    {
                        DecoderState next_state = handle_state_analyze(sym, &prev_symbol, bit_buffer, &bit_count, &keep_running, &consecutive_ones);
                        
                        // Check if we finished reading the current packet
                        if (next_state == STATE_WAIT_ACTIVITY && bit_count > 0) {
                            
                            UsbPacket pkt_info = analyze_usb_packet(bit_buffer, bit_count);
                            
                            if (record_mode && transaction_record_buffer) {
                                // Smart Transaction Recording Logic
                                if (pkt_info.type == PKT_TYPE_TOKEN) {
                                    // Store the token and wait to see if DATA follows
                                    memcpy(pending_token_bits, bit_buffer, bit_count);
                                    pending_token_bit_count = bit_count;
                                    has_pending_token = 1;
                                }
                                else if (pkt_info.type == PKT_TYPE_DATA) {
                                    // DATA packet arrived!
                                    if (recorded_transaction_count < MAX_RECORDED_PACKETS) {
                                        if (has_pending_token) {
                                            // Pair found! Record both Token and Data
                                            memcpy(transaction_record_buffer[recorded_transaction_count].token_bits, pending_token_bits, pending_token_bit_count);
                                            transaction_record_buffer[recorded_transaction_count].token_bit_count = pending_token_bit_count;
                                        } else {
                                            // Orphan DATA packet (no preceding IN/OUT token)
                                            transaction_record_buffer[recorded_transaction_count].token_bit_count = 0;
                                        }
                                        
                                        // Save the DATA packet
                                        memcpy(transaction_record_buffer[recorded_transaction_count].data_bits, bit_buffer, bit_count);
                                        transaction_record_buffer[recorded_transaction_count].data_bit_count = bit_count;
                                        
                                        recorded_transaction_count++;
                                        
                                        if (recorded_transaction_count == MAX_RECORDED_PACKETS) {
                                            printf("Record buffer FULL! Stopping capture.\n");
                                            keep_running = 0;
                                        }
                                    }
                                    has_pending_token = 0; // Consume the token
                                }
                                else {
                                    // Any other packet (SOF, SETUP, Handshakes) cancels the pending IN/OUT token
                                    has_pending_token = 0;
                                }
                            } else {
                                // If not recording, just print DATA packets to the screen as before
                                if (pkt_info.type == PKT_TYPE_DATA) {
                                    print_decoded_bits(bit_buffer, bit_count);
                                    print_usb_packet(&pkt_info, 0); 
                                }
                            }
                            
                            bit_count = 0;    
                            if (recorded_transaction_count < MAX_RECORDED_PACKETS) {
                                keep_running = 1; 
                            }
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
    
    // Defer the extremely slow file I/O until the hardware has stopped
    if (record_mode && transaction_record_buffer) {
        flush_records_to_file(transaction_record_buffer, recorded_transaction_count);
        free(transaction_record_buffer);
    }
    
    print_performance_metrics(total_analyze_time_ns, symbols_analyzed_count);
    output_decoder_results(debug_mode, debug_buffer, debug_count, bit_buffer, bit_count);
}

/* =========================================================
 * APPLICATION ENTRY POINT
 * ========================================================= */

int parse_arguments(int argc, char *argv[], int* record_mode) {
    int debug_mode = -1; // Normal Mode
    *record_mode = 0;    // Default is off
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug1") == 0) debug_mode = 1;
        else if (strcmp(argv[i], "--debug2") == 0) debug_mode = 2;
        else if (strcmp(argv[i], "--record") == 0) *record_mode = 1;
    }
    return debug_mode;
}

int main(int argc, char *argv[]) {
    printf("--- SMI Logic Analyzer: Multi-Stage Packet Decoder ---\n");
    signal(SIGINT, handle_sigint);
    
    int record_mode = 0;
    int debug_mode = parse_arguments(argc, argv, &record_mode);

    if (smi_manager_init(2000000) != 0) {
        return -1;
    }

    if (debug_mode == 1) {
        run_debug1_fast_path();
    } else {
        run_main_decoder_loop(debug_mode, record_mode);
    }

    return 0;
}