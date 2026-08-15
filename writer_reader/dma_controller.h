#ifndef DMA_CONTROLLER_H
#define DMA_CONTROLLER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hw_spi_config.h"

typedef struct {
    int channel_id;
    bool is_busy;
    size_t transfer_size;
} DmaController;

// Function prototypes
int dma_init(DmaController *dma, int channel_id);
int dma_start_transfer(DmaController *dma, SpiConfig *spi, void *rx_buffer, void *tx_buffer, size_t length);
int dma_wait_completion(DmaController *dma);
void dma_cleanup(DmaController *dma);

#endif // DMA_CONTROLLER_H