#include "hw_spi_config.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

int hw_spi_init(SpiConfig *config, const char *device_path, uint32_t speed_hz, uint8_t mode, uint8_t bits_per_word) {
    if (!config || !device_path) {
        return -1;
    }

    config->device_path = device_path;
    config->speed_hz = speed_hz;
    config->mode = mode;
    config->bits_per_word = bits_per_word;

    // Open SPI device file descriptor
    config->fd = open(config->device_path, O_RDWR);
    if (config->fd < 0) {
        perror("[HW_SPI] Error opening SPI device");
        return -1;
    }

    // Set SPI Mode
    if (ioctl(config->fd, SPI_IOC_WR_MODE, &config->mode) < 0) {
        perror("[HW_SPI] Error setting SPI mode");
        close(config->fd);
        return -1;
    }

    // Set bits per word
    if (ioctl(config->fd, SPI_IOC_WR_BITS_PER_WORD, &config->bits_per_word) < 0) {
        perror("[HW_SPI] Error setting bits per word");
        close(config->fd);
        return -1;
    }

    // Set maximum clock speed
    if (ioctl(config->fd, SPI_IOC_WR_MAX_SPEED_HZ, &config->speed_hz) < 0) {
        perror("[HW_SPI] Error setting SPI max speed");
        close(config->fd);
        return -1;
    }

    return 0;
}

int hw_spi_set_clock(SpiConfig *config, uint32_t new_speed_hz) {
    if (!config || config->fd < 0) {
        return -1;
    }

    config->speed_hz = new_speed_hz;
    if (ioctl(config->fd, SPI_IOC_WR_MAX_SPEED_HZ, &config->speed_hz) < 0) {
        perror("[HW_SPI] Error updating clock frequency");
        return -1;
    }

    return 0;
}

void hw_spi_close(SpiConfig *config) {
    if (config && config->fd >= 0) {
        close(config->fd);
        config->fd = -1;
    }
}