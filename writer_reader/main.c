#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "hw_spi_config.h"
#include "dma_controller.h"
#include "ram_buffer.h"

#define DEFAULT_SPI_DEVICE   "/dev/spidev0.0"
#define DEFAULT_SPEED_HZ     25000000        // 25 MHz SPI Clock
#define DEFAULT_SAMPLE_COUNT 100000          // 100,000 samples per channel
#define OUTPUT_BIN_FILE      "capture_output.bin"

int main(int argc, char *argv[]) {
    size_t sample_count = DEFAULT_SAMPLE_COUNT;
    const char *spi_device = DEFAULT_SPI_DEVICE;

    if (argc > 1) {
        sample_count = (size_t)atoi(argv[1]);
    }

    printf("==================================================\n");
    printf("[MAIN] Starting Dual-Channel Acquisition Orchestrator\n");
    printf("[MAIN] Target Samples: %zu per channel\n", sample_count);
    printf("==================================================\n");

    // 1. Allocate continuous physical RAM buffer
    ContiguousRamBuffer ram_buf;
    if (ram_buffer_allocate(&ram_buf, sample_count) != 0) {
        fprintf(stderr, "[MAIN] Failed to allocate RAM buffer\n");
        return EXIT_FAILURE;
    }
    printf("[MAIN] Allocated %zu bytes in contiguous RAM\n", ram_buf.total_bytes);

    // 2. Initialize SPI hardware interface
    SpiConfig spi;
    if (hw_spi_init(&spi, spi_device, DEFAULT_SPEED_HZ, 0, 16) != 0) {
        fprintf(stderr, "[MAIN] Failed to initialize SPI interface\n");
        ram_buffer_free(&ram_buf);
        return EXIT_FAILURE;
    }
    printf("[MAIN] SPI initialized at %u Hz on %s\n", spi.speed_hz, spi_device);

    // 3. Initialize DMA controller
    DmaController dma;
    if (dma_init(&dma, 0) != 0) {
        fprintf(stderr, "[MAIN] Failed to initialize DMA controller\n");
        hw_spi_close(&spi);
        ram_buffer_free(&ram_buf);
        return EXIT_FAILURE;
    }

    // 4. Trigger DMA acquisition
    printf("[MAIN] Triggering DMA burst transfer...\n");
    if (dma_start_transfer(&dma, &spi, ram_buf.raw_data, NULL, ram_buf.total_bytes) != 0) {
        fprintf(stderr, "[MAIN] DMA transfer failed\n");
        dma_cleanup(&dma);
        hw_spi_close(&spi);
        ram_buffer_free(&ram_buf);
        return EXIT_FAILURE;
    }

    dma_wait_completion(&dma);
    printf("[MAIN] DMA transfer complete!\n");

    // 5. De-interleave dual-channel data
    printf("[MAIN] De-interleaving Channel 1 and Channel 2 data...\n");
    ram_buffer_split_dual_channel(&ram_buf);

    // 6. Export data to binary file
    printf("[MAIN] Exporting data to '%s'...\n", OUTPUT_BIN_FILE);
    if (ram_buffer_export_bin(&ram_buf, OUTPUT_BIN_FILE) != 0) {
        fprintf(stderr, "[MAIN] Failed to export capture data\n");
    }

    // 7. Clean up hardware & memory resources
    dma_cleanup(&dma);
    hw_spi_close(&spi);
    ram_buffer_free(&ram_buf);
    printf("[MAIN] Hardware cleanup complete.\n");

    // 8. Launch Python visualization script
    printf("[MAIN] Launching Python plot viewer...\n");
    char command[256];
    snprintf(command, sizeof(command), "python3 plot_viewer.py %s", OUTPUT_BIN_FILE);
    int sys_ret = system(command);
    if (sys_ret != 0) {
        printf("[MAIN] Note: Python script finished or exited with code %d\n", sys_ret);
    }

    return EXIT_SUCCESS;
}