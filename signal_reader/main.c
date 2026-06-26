// ... (All includes, defines, and utility functions remain exactly the same) ...

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
    
    // Added tracker for USB Bit Stuffing
    int consecutive_ones = 0;

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
                        consecutive_ones = 0; // Initialize unstuffing counter for the new packet
                    }
                    break;
                    
                case STATE_ANALYZE:
                    {
                        // Passed the address of consecutive_ones
                        DecoderState next_state = handle_state_analyze(sym, &prev_symbol, bit_buffer, &bit_count, &keep_running, &consecutive_ones);
                        
                        // Check if we finished reading the current packet (SE0 or Error detected)
                        if (next_state == STATE_WAIT_ACTIVITY) {
                            
                            if (bit_count > 0) {
                                print_decoded_bits(bit_buffer, bit_count);
                                analyze_usb_packet(bit_buffer, bit_count, 0); 
                            }
                            
                            bit_count = 0;    
                            keep_running = 1; 
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

// ... (main function remains exactly the same) ...