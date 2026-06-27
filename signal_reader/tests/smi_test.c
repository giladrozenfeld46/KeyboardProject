#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define BCM2711_PERI_BASE 0xFE000000 
#define SMI_BASE          (BCM2711_PERI_BASE + 0x600000)
#define CM_BASE           (BCM2711_PERI_BASE + 0x101000) 
#define GPIO_BASE         (BCM2711_PERI_BASE + 0x200000) 

#define SMI_CS_REG        0x00 
#define SMI_L_REG         0x04 
#define SMI_A_REG         0x08 
#define SMI_D_REG         0x0C 
#define SMI_SR0_REG       0x20 

#define SMI_CS_ENABLE     (1 << 0)
#define SMI_CS_DONE       (1 << 1)  
#define SMI_CS_START      (1 << 3)  
#define SMI_CS_CLEAR      (1 << 4)  
#define SMI_CS_WRITE      (1 << 5)  

#define CM_SMICTL         0xB0
#define CM_SMIDIV         0xB4
#define CM_PASSWD         0x5A000000 

#define GPFSEL0           0x00 
#define GPPUPPDN0         0x39 

int main() {
    int mem_fd;
    void *smi_map, *cm_map, *gpio_map;
    volatile uint32_t *smi, *cm, *gpio;

    if ((mem_fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0) {
        printf("Error: Cannot open /dev/mem.\n");
        return -1;
    }

    smi_map  = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, SMI_BASE);
    cm_map   = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, CM_BASE);
    gpio_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, GPIO_BASE);

    if (smi_map == MAP_FAILED || cm_map == MAP_FAILED || gpio_map == MAP_FAILED) {
        printf("Error: Memory mapping failed.\n");
        return -1;
    }

    smi  = (volatile uint32_t *)smi_map;
    cm   = (volatile uint32_t *)cm_map;
    gpio = (volatile uint32_t *)gpio_map;

    // --- 1. PREPARE GPIO AS PASSIVE INPUT (HIGH-Z) ---
    // Clear ALT functions (set to INPUT) and disable internal pull resistors
    gpio[GPFSEL0] &= ~((7 << 24) | (7 << 27));
    gpio[GPPUPPDN0] &= ~((3 << 16) | (3 << 18));

    // --- 2. SETUP CLOCK ---
    cm[CM_SMICTL / 4] = CM_PASSWD | (0 << 4); 
    usleep(10);
    cm[CM_SMIDIV / 4] = CM_PASSWD | (4 << 12); 
    usleep(10);
    cm[CM_SMICTL / 4] = CM_PASSWD | 1 | (1 << 4); 
    usleep(10);

    // --- 3. INITIALIZE SMI IN STRICT READ MODE ---
    // The SMI hardware is awakened, buffers cleared, and timing set
    // It is fully expecting to READ (SMI_CS_WRITE is 0)
    smi[SMI_CS_REG / 4] = SMI_CS_ENABLE | SMI_CS_CLEAR;
    usleep(10);
    smi[SMI_SR0_REG / 4] = (0x3F << 24) | (0x3F << 16) | (0x3F << 8) | 0x3F; 
    smi[SMI_L_REG / 4] = 1;
    smi[SMI_A_REG / 4] = 0;

    // --- 4. HANDOVER PINS TO SMI ---
    // Now that SMI is in READ mode, switching the MUX will not cause contention
    gpio[GPFSEL0] |= ((5 << 24) | (5 << 27)); // Set to ALT1

    // --- 5. EXECUTE READ ---
    smi[SMI_CS_REG / 4] = SMI_CS_ENABLE | SMI_CS_START;

    int timeout = 1000000;
    while (!(smi[SMI_CS_REG / 4] & SMI_CS_DONE)) {
        timeout--;
        if (timeout == 0) break;
    }

    // --- 6. IMMEDIATE RELEASE ---
    // Disconnect SMI from the pins immediately after reading
    gpio[GPFSEL0] &= ~((7 << 24) | (7 << 27));

    if (timeout > 0) {
        uint32_t data = smi[SMI_D_REG / 4];
        printf("State of SD0 (GPIO 8): %d\n", (data & (1 << 0)) ? 1 : 0);
        printf("State of SD1 (GPIO 9): %d\n", (data & (1 << 1)) ? 1 : 0);
    } else {
        printf("Timeout Error!\n");
    }

    munmap(smi_map, 4096);
    munmap(cm_map, 4096);
    munmap(gpio_map, 4096);
    close(mem_fd);
    
    return 0;
}
