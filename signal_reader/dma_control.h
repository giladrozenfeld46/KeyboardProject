#ifndef DMA_CONTROL_H
#define DMA_CONTROL_H

#include <stdint.h>

// BCM2711 DMA Register Offsets relative to Channel Base
#define DMA_CS_OFFSET         0x00 // Control and Status
#define DMA_CONBLK_AD_OFFSET  0x04 // Control Block Address

// DMA Control & Status Register Bitmasks
#define DMA_CS_ACTIVE        (1 << 0)
#define DMA_CS_END           (1 << 1)
#define DMA_CS_RESET         (1 << 31)

// Hardware DMA Control Block structure aligned to 32 bytes
struct __attribute__((__packed__, aligned(32))) DmaControlBlock {
    uint32_t ti;         // Transfer Information configuration flags
    uint32_t src_ad;     // Source Bus Address
    uint32_t dest_ad;    // Destination Bus Address
    uint32_t txfr_len;   // Total transfer length in bytes
    uint32_t stride;     // 2D Stride pitch padding (set to 0 for 1D)
    uint32_t nextconbk;  // Bus Address of next Control Block (0 to terminate)
    uint32_t padding[2]; // Reserved alignment padding
};

// Populates a pre-allocated Control Block structure with parameters
void setup_dma_control_block(struct DmaControlBlock* cb, uint32_t ti, uint32_t src_bus_addr, uint32_t dest_bus_addr, uint32_t len_bytes);

// Resets, primes and executes a specific DMA channel using a Control Block
void start_dma_channel(volatile uint32_t* dma_chan_regs, uint32_t cb_bus_addr);

// Forces a specific DMA channel to halt execution immediately
void stop_dma_channel(volatile uint32_t* dma_chan_regs);

// Checks if the DMA transaction has completed
// Returns 1 if finished, 0 if still active
int is_dma_transfer_complete(volatile uint32_t* dma_chan_regs);

#endif // DMA_CONTROL_H