#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <stdint.h>

#define MAX_DECODED_BITS 2048
#define DEBUG_SYMBOLS_COUNT 128 

// Symbol structure
typedef struct {
    uint8_t dplus;
    uint8_t dminus;
} Symbol;

// Define the State Machine states
typedef enum {
    STATE_WAIT_ACTIVITY, // Wait for unusual voltage (D+ HIGH or D- LOW)
    STATE_SYNC_SEARCH,   // Wait for the 8-symbol SYNC pattern
    STATE_ANALYZE,       // Decode NRZI to bits
    STATE_DEBUG          // Collect raw symbols for debugging (used by debug2)
} DecoderState;

/**
 * Helper to check if two symbols are logically identical
 */
int is_symbol_equal(Symbol a, Symbol b);

/**
 * Handles the WAIT_ACTIVITY state logic.
 * Looks for an unusual voltage state (D+ is HIGH) to start analysis.
 * Injects the triggering symbol into the sync buffer so it's not missed.
 */
DecoderState handle_state_wait_activity(Symbol sym, Symbol* sync_buffer);

/**
 * Handles the SYNC_SEARCH state logic. 
 * Looks for the SYNC pattern in the incoming symbols.
 */
DecoderState handle_state_sync_search(Symbol sym, Symbol* sync_buffer, const Symbol* expected_sync, Symbol* out_prev_symbol, int debug_mode);

/**
 * Handles the DEBUG state logic (Used by debug2).
 * Collects a specified amount of raw symbols for debugging purposes.
 */
DecoderState handle_state_debug(Symbol sym, Symbol* debug_buffer, int* debug_count, volatile int* keep_running);

/**
 * Handles the ANALYZE state logic.
 * Decodes NRZI symbols into bits and detects End of Packet (SE0).
 */
DecoderState handle_state_analyze(Symbol sym, Symbol* prev_symbol, uint8_t* bit_buffer, int* bit_count, volatile int* keep_running);

#endif // STATE_MACHINE_H