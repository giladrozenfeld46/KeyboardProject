#include "dma_control.h"
#include <unistd.h>

void setup_dma_control_block(struct DmaControlBlock* cb, uint32_t ti, uint32_t src_bus_addr, uint32_t dest_bus_addr, uint32_t len_bytes) {
    if (!cb) return;
    
    cb->ti = ti;
    cb->src_ad = src_bus_addr;
    cb->dest_ad = dest_bus_addr;
    cb->txfr_len = len_bytes;
    cb->stride = 0;
    cb->nextconbk = 0; // Singular transaction block configuration
}

void start_dma_channel(volatile uint32_t* dma_chan_regs, uint32_t cb_bus_addr) {
    if (!dma_chan_regs) return;

    // 1. Apply hardware reset to clear channel internal cache and pipeline
    dma_chan_regs[DMA_CS_OFFSET / 4] = DMA_CS_RESET;
    usleep(10);

    // 2. Clear out the peripheral execution END flag status bit
    dma_chan_regs[DMA_CS_OFFSET / 4] = DMA_CS_END;

    // 3. Load the Bus Address pointing to the primary hardware Control Block
    dma_chan_regs[DMA_CONBLK_AD_OFFSET / 4] = cb_bus_addr;

    // 4. Assert the ACTIVE flag to initiate data transfer processing
    dma_chan_regs[DMA_CS_OFFSET / 4] = DMA_CS_ACTIVE;
}

void stop_dma_channel(volatile uint32_t* dma_chan_regs) {
    if (!dma_chan_regs) return;

    // Clear the active bit to halt active sequencing
    dma_chan_regs[DMA_CS_OFFSET / 4] &= ~DMA_CS_ACTIVE;
}

int is_dma_transfer_complete(volatile uint32_t* dma_chan_regs) {
    if (!dma_chan_regs) return 0;
    
    // Check if the END status bit has been set by the controller
    return (dma_chan_regs[DMA_CS_OFFSET / 4] & DMA_CS_END) ? 1 : 0;
}