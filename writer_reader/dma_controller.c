#include "dma_controller.h"
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

int dma_init(DmaController *dma, int channel_id) {
    if (!dma) return -1;
    
    dma->channel_id = channel_id;
    dma->is_busy = false;
    dma->transfer_size = 0;
    return 0;
}

int dma_start_transfer(DmaController *dma, SpiConfig *spi, void *rx_buffer, void *tx_buffer, size_t length) {
    if (!dma || !spi || spi->fd < 0 || !rx_buffer) {
        return -1;
    }

    dma->is_busy = true;
    dma->transfer_size = length;

    // Configure SPI message transfer descriptor
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx_buffer,
        .rx_buf = (unsigned long)rx_buffer,
        .len = (uint32_t)length,
        .speed_hz = spi->speed_hz,
        .bits_per_word = spi->bits_per_word,
        .delay_usecs = 0,
        .cs_change = 0,
    };

    // Execute direct kernel-driven DMA-capable transfer
    int ret = ioctl(spi->fd, SPI_IOC_MESSAGE(1), &tr);
    if (ret < 1) {
        perror("[DMA] Transfer failed");
        dma->is_busy = false;
        return -1;
    }

    dma->is_busy = false;
    return 0;
}

int dma_wait_completion(DmaController *dma) {
    if (!dma) return -1;
    // For asynchronous DMA architectures, poll transfer ready flag / interrupt
    while (dma->is_busy) {
        // Active wait or event yield
    }
    return 0;
}

void dma_cleanup(DmaController *dma) {
    if (dma) {
        dma->is_busy = false;
        dma->transfer_size = 0;
    }
}