#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "smi_manager.h"

// Assuming D+ is connected to GPIO9 (Bit 1)
#define DPLUS_BIT_MASK (1 << 1) 
// Assuming D- is connected to GPIO8 (Bit 0)
#define DMINUS_BIT_MASK (1 << 0)

#define TEST_LENGTH 100

int main() {
    printf("--- SMI Writing Tester ---\n");

    // Initialize with 2MHz sample rate, same as main.c
    if (smi_manager_init(2000000) != 0) {
        printf("Error: Failed to initialize SMI Manager.\n");
        return -1;
    }

    printf("SMI Initialized. Preparing data to send...\n");

    // Prepare a test pattern to send to GPIO8 and GPIO9
    uint32_t test_data[TEST_LENGTH];

    for (int i = 0; i < TEST_LENGTH; i++) {
        test_data[i] = 0; // Default to all 0s

        // Create a simple alternating pattern for testing
        // You can change this pattern to whatever you need to test
        if (i % 4 == 0) {
            // Both HIGH
            test_data[i] = DPLUS_BIT_MASK | DMINUS_BIT_MASK;
        } else if (i % 4 == 1) {
            // D+ HIGH, D- LOW
            test_data[i] = DPLUS_BIT_MASK;
        } else if (i % 4 == 2) {
            // D+ LOW, D- HIGH
            test_data[i] = DMINUS_BIT_MASK;
        } else {
            // Both LOW
            test_data[i] = 0;
        }
    }

    printf("Sending %d samples...\n", TEST_LENGTH);
    
    if (smi_manager_write_data(test_data, TEST_LENGTH) != 0) {
        printf("Error: Failed to write data via SMI.\n");
    } else {
        printf("Data written successfully via SMI DMA.\n");
    }

    // Wait a brief moment to ensure we don't clean up too fast after transmission
    usleep(10000); 

    printf("Cleaning up hardware state...\n");
    smi_manager_cleanup();
    printf("Done.\n");

    return 0;
}
