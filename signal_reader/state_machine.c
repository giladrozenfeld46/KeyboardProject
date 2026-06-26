#include <stdio.h>
#include "state_machine.h"

int is_symbol_equal(Symbol a, Symbol b) {
    return (a.dplus == b.dplus) && (a.dminus == b.dminus);
}

DecoderState handle_state_wait_activity(Symbol sym, Symbol* sync_buffer) {
    // Check for unusual activity: D+ going high
    if (sym.dplus == 1) {
        printf("Unusual line activity detected (D+ HIGH)!\n");
        printf("Switching to SYNC SEARCH state...\n");
        
        // Push this first symbol into the shift register! 
        // This prevents the first SYNC symbol from being consumed and lost.
        for (int i = 0; i < 7; i++) {
            sync_buffer[i] = sync_buffer[i+1];
        }
        sync_buffer[7] = sym;
        
        return STATE_SYNC_SEARCH;
    }
    
    return STATE_WAIT_ACTIVITY; // Keep waiting
}

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

DecoderState handle_state_debug(Symbol sym, Symbol* debug_buffer, int* debug_count, volatile int* keep_running) {
    debug_buffer[(*debug_count)++] = sym;

    // Stop if we reached the desired count or detected SE0 (End of Packet)
    if (*debug_count >= DEBUG_SYMBOLS_COUNT) {
        if (sym.dplus == 0 && sym.dminus == 0) {
            printf("SE0 (End of Packet) detected during debug collection.\n");
        } else {
            printf("Collected %d debug symbols.\n", *debug_count);
        }
        *keep_running = 0; // Signal the main loop to stop
        return STATE_WAIT_ACTIVITY;
    }

    return STATE_DEBUG; // Stay in debug mode
}

DecoderState handle_state_analyze(Symbol sym, Symbol* prev_symbol, uint8_t* bit_buffer, int* bit_count, volatile int* keep_running) {
    // End of Packet Detection: Both lines LOW (SE0)
    if (sym.dplus == 0 && sym.dminus == 0) {
        printf("SE0 (End of Packet) detected! Stopping analysis.\n");
        *keep_running = 0; // Signal the main loop to stop
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
        *keep_running = 0;
    }

    // Update previous symbol for the next iteration
    *prev_symbol = sym;
    
    return STATE_ANALYZE; // Stay in analysis mode
}