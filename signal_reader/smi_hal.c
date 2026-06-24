#include "smi_hal.h"
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

// Hardware physical base addresses for Raspberry Pi 4 (BCM2711)
#define BCM2711_PERI_BASE    0xFE000000 
#define SMI_BASE             (BCM2711_PERI_BASE + 0x600000)
#define CM_BASE              (BCM2711_PERI_BASE + 0x101000) 
#define GPIO_BASE            (BCM2711_PERI_BASE + 0x200000)

// Register offsets
#define SMI_CS_REG           0x00 
#define SMI_L_REG            0x04 
#define SMI_A_REG            0x08 
#define SMI_DCS_REG          0x10 
#define SMI_SR0_REG          0x20 

#define CM_SMICTL            0xB0
#define CM_SMIDIV            0xB4
#define CM_PASSWD            0x5A000000 

#define GPFSEL0              0x00 
#define GPPUPPDN0            0x39 

// Bit flags
#define SMI_CS_ENABLE        (1 << 0)
#define SMI_CS_START         (1 << 3)  
#define SMI_CS_CLEAR         (1 << 4)  
#define SMI_DCS_ENABLE       (1 << 0) 

int smi_init(SmiHardware* hw, int mem_fd) {
    hw->smi  = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, SMI_BASE);
    hw->cm   = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, CM_BASE);
    hw->gpio = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, GPIO_BASE);

    if (hw->smi == MAP_FAILED || hw->cm == MAP_FAILED || hw->gpio == MAP_FAILED) {
        return -1;
    }
    return 0;
}


void smi_start_capture(SmiHardware* hw, uint32_t num_samples, uint32_t target_hz) {
    if (!hw || !hw->smi || !hw->cm || !hw->gpio) return;

    // 1. Prepare GPIO as safe High-Z input FIRST
    hw->gpio[GPFSEL0] &= ~((7 << 24) | (7 << 27));
    hw->gpio[GPPUPPDN0] &= ~((3 << 16) | (3 << 18));

    // --- DYNAMIC FREQUENCY CALCULATION ---
    uint32_t source_clock = 500000000; // 500 MHz PLLD
    uint32_t clock_divisor = 2;        // Default base divider (250MHz base clock)
    
    // Calculate total SMI cycles needed to step down to the target frequency
    uint32_t base_clock = source_clock / clock_divisor;
    uint32_t total_cycles = base_clock / target_hz;

    // Boundary checks for physical limitations
    if (total_cycles < 4) total_cycles = 4; // Hardware minimum (1 cycle per stage)
    if (total_cycles > 252) {
        // If the target frequency is very low, increase the clock divisor
        clock_divisor = total_cycles / 63;
        if (clock_divisor > 32) clock_divisor = 32; // Hardware maximum for integer divider
        base_clock = source_clock / clock_divisor;
        total_cycles = base_clock / target_hz;
    }

    // Distribute cycles across the 4 stages (10% Setup, 50% Strobe, 20% Hold, 20% Pace)
    uint32_t setup  = (total_cycles * 1) / 10;
    uint32_t strobe = (total_cycles * 5) / 10;
    uint32_t hold   = (total_cycles * 2) / 10;
    
    // Ensure no stage is completely 0 to maintain signal integrity
    if (setup == 0) setup = 1;
    if (strobe == 0) strobe = 1;
    if (hold == 0) hold = 1;

    uint32_t pace = total_cycles - setup - strobe - hold;
    if (pace == 0) pace = 1;
    if (pace > 63) pace = 63; // Max hardware limit per stage

    // 2. Configure Dynamic Clock
    hw->cm[CM_SMICTL / 4] = CM_PASSWD | (0 << 4); // Stop clock
    usleep(10);
    hw->cm[CM_SMIDIV / 4] = CM_PASSWD | (clock_divisor << 12); 
    usleep(10);
    hw->cm[CM_SMICTL / 4] = CM_PASSWD | 1 | (1 << 4); // Start clock
    usleep(10);

    // 3. Initialize SMI hardware with dynamic timing
    hw->smi[SMI_CS_REG / 4] = SMI_CS_ENABLE | SMI_CS_CLEAR;
    usleep(10);
    
    // Pack the calculated cycles into the timing register
    hw->smi[SMI_SR0_REG / 4] = (setup << 24) | (strobe << 16) | (hold << 8) | pace; 
    hw->smi[SMI_L_REG / 4] = num_samples; 
    hw->smi[SMI_A_REG / 4] = 0;

    // 4. Safely handover control of the pins to SMI (ALT1)
    hw->gpio[GPFSEL0] |= ((5 << 24) | (5 << 27));

    // 5. Enable DMA Requests (DREQ) and start SMI capture
    hw->smi[SMI_DCS_REG / 4] = SMI_DCS_ENABLE;
    hw->smi[SMI_CS_REG / 4] = SMI_CS_ENABLE | SMI_CS_START;
}


void smi_cleanup(SmiHardware* hw) {
    if (!hw) return;

    // Disconnect SMI and revert pins to safe High-Z inputs
    if (hw->gpio != MAP_FAILED) {
        hw->gpio[GPFSEL0] &= ~((7 << 24) | (7 << 27));
    }
    
    // Disable SMI
    if (hw->smi != MAP_FAILED) {
        hw->smi[SMI_CS_REG / 4] = 0;
    }

    // Unmap memory
    if (hw->smi != MAP_FAILED) munmap((void*)hw->smi, 4096);
    if (hw->cm != MAP_FAILED) munmap((void*)hw->cm, 4096);
    if (hw->gpio != MAP_FAILED) munmap((void*)hw->gpio, 4096);
}