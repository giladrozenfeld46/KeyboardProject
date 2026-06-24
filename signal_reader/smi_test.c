#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

// BCM2711 (Raspberry Pi 4) physical memory base address
#define BCM2711_PERI_BASE 0xFE000000 
#define SMI_BASE          (BCM2711_PERI_BASE + 0x600000)

// SMI Register offsets (in bytes)
// We divide by 4 later when accessing via uint32_t pointer
#define SMI_CS_REG        0x00 // Control & Status
#define SMI_L_REG         0x04 // Transfer Length
#define SMI_A_REG         0x08 // Address
#define SMI_D_REG         0x0C // Data
#define SMI_READ0_REG     0x14 // Read Timing 0

// CS Register bit definitions
#define SMI_CS_ENABLE     (1 << 0)
#define SMI_CS_CLEAR      (1 << 3)
#define SMI_CS_START      (1 << 4)
#define SMI_CS_RXD        (1 << 31) // RX Data ready flag

int main() {
    int mem_fd;
    void *smi_map;
    volatile uint32_t *smi;

    // Open /dev/mem to access physical memory directly
    if ((mem_fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0) {
        printf("Error: Cannot open /dev/mem. Are you running with sudo?\n");
        return -1;
    }

    // Map the 4KB memory block containing the SMI registers
    smi_map = mmap(
        NULL,             
        4096,             
        PROT_READ | PROT_WRITE,
        MAP_SHARED,       
        mem_fd,           
        SMI_BASE          
    );

    if (smi_map == MAP_FAILED) {
        printf("Error: Memory mapping failed.\n");
        close(mem_fd);
        return -1;
    }

    smi = (volatile uint32_t *)smi_map;

    // 1. Clear the SMI internal state
    smi[SMI_CS_REG / 4] = SMI_CS_CLEAR;

    // 2. Configure basic timing for Read Channel 0
    // These are slow, safe placeholder timings for testing
    smi[SMI_READ0_REG / 4] = (0x3F << 24) | (0x3F << 16) | (0x3F << 8) | 0x3F; 

    // 3. Set transfer length to 1 word (32-bit read)
    smi[SMI_L_REG / 4] = 1;

    // 4. Enable the SMI peripheral
    smi[SMI_CS_REG / 4] = SMI_CS_ENABLE;
    printf("SMI Initialized. Requesting read...\n");

    // 5. Trigger a read operation by setting the START bit
    smi[SMI_CS_REG / 4] |= SMI_CS_START;

    // 6. Polling: Wait for the RXD bit to go high (Data Ready)
    int timeout = 1000000;
    while (!(smi[SMI_CS_REG / 4] & SMI_CS_RXD)) {
        timeout--;
        if (timeout == 0) {
            printf("Timeout Error: No data received from SMI.\n");
            break;
        }
    }

    // 7. Read the data from the Data Register
    if (timeout > 0) {
        uint32_t data = smi[SMI_D_REG / 4];
        printf("Success! Read Data: 0x%08X\n", data);
    }

    // 8. Cleanup and unmap memory
    munmap(smi_map, 4096);
    close(mem_fd);
    
    return 0;
}