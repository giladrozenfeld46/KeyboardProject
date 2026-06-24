#ifndef SMI_HAL_H
#define SMI_HAL_H

#include <stdint.h>

// Structure to hold mapped hardware register pointers
typedef struct {
    volatile uint32_t* smi;
    volatile uint32_t* cm;
    volatile uint32_t* gpio;
} SmiHardware;

// Maps SMI, Clock Manager, and GPIO registers into memory.
// Returns 0 on success, -1 on failure.
int smi_init(SmiHardware* hw, int mem_fd);

// Safely configures pins to High-Z, prepares SMI for read, 
// switches pins to ALT1, and arms the DREQ signal.
// Safely configures pins, sets the dynamic sampling rate, and starts capture
void smi_start_capture(SmiHardware* hw, uint32_t num_samples, uint32_t target_hz);

// Safely disables SMI and returns pins to safe High-Z input state.
// Also unmaps the hardware registers.
void smi_cleanup(SmiHardware* hw);

#endif // SMI_HAL_H