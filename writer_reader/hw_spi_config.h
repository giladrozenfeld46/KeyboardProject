#ifndef HW_SPI_CONFIG_H
#define HW_SPI_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// SPI Configuration parameters
typedef struct {
    const char *device_path;
    uint32_t speed_hz;
    uint8_t mode;
    uint8_t bits_per_word;
    int fd;
} SpiConfig;

// Function prototypes
int hw_spi_init(SpiConfig *config, const char *device_path, uint32_t speed_hz, uint8_t mode, uint8_t bits_per_word);
int hw_spi_set_clock(SpiConfig *config, uint32_t new_speed_hz);
void hw_spi_close(SpiConfig *config);

#endif // HW_SPI_CONFIG_H