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
* Transmits a sequence of bits to GPIO pins via the SMI and DMA.
* Automatically switches the hardware from Read to Write, sends the data,
* and seamlessly switches back to Read mode using a secondary DMA channel.
* @param data Pointer to the array of 32-bit values to transmit.
* @param length Number of 32-bit samples to send.
* @return 0 on success, -1 on failure.
*/
int smi_manager_write_data(uint32_t *data, int length);

/**
 * Stops the hardware, unmaps memory, and frees all allocated DMA buffers.
 * Should be called when the program is exiting or capture is complete.
 */
void smi_manager_cleanup(void);

#endif // SMI_MANAGER_H