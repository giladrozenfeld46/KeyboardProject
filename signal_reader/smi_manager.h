#ifndef SMI_MANAGER_H
#define SMI_MANAGER_H

#include <stdint.h>

/**
 * Initializes the DMA and SMI hardware, allocates buffers, 
 * pre-fills them with 0xFFFFFFFF, and starts the capture in the background.
 * * @param target_rate_hz The desired sampling rate in Hz (e.g., 25000000 for 25 MSPS).
 * @return 0 on success, -1 on failure.
 */
int smi_manager_init(uint32_t target_rate_hz);

/**
 * Attempts to read a chunk of samples from the buffer using fast memory copying.
 * The number of samples returned per chunk is defined by CHUNK_SIZE in smi_manager.c (currently 8).
 * * @param out_samples Pointer to an array where the fetched samples will be stored.
 * Must be large enough to hold CHUNK_SIZE elements (e.g., uint32_t arr[8]).
 * @return 1 if a chunk was successfully read, 0 if the chunk is not ready yet.
 */
int smi_manager_read_chunk(uint32_t *out_samples);

/**
 * Writes a sequence of GPIO8 and GPIO9 values to the SMI hardware for transmission.
 * The sequences are expected to be arrays of 0s and 1s, representing LOW and HIGH states.
 * The length of the sequences should not exceed BUFFER_SAMPLES (1024).
 * @param gpio8_seq Pointer to an array of uint8_t values for GPIO8.
 * @param gpio9_seq Pointer to an array of uint8_t values for GPIO9.
 * @param length The number of samples in each sequence (must be <= BUFFER_SAMPLES).
 */
void smi_manager_write_sequence(const uint8_t* gpio8_seq, const uint8_t* gpio9_seq, size_t length);

/**
 * Stops the hardware, unmaps memory, and frees all allocated DMA buffers.
 * Should be called when the program is exiting or capture is complete.
 */
void smi_manager_cleanup(void);

#endif // SMI_MANAGER_H