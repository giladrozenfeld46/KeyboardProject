#include "smi_hal.h"
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

// Hardware physical base addresses
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

// SMI Control Bits
#define SMI_CS_ENABLE        (1 << 0)
#define SMI_CS_START         (1 << 3)  
#define SMI_CS_CLEAR         (1 << 4)  
#define SMI_DCS_ENABLE       (1 << 0) 

// --- NEW: Descriptive Hardware Constraints ---
#define SMI_SOURCE_CLOCK_HZ  500000000 // 500 MHz PLLD clock source
#define SMI_MAX_DIVISOR      32        // Maximum integer divisor for the clock
#define SMI_MAX_STAGE_CYCLES 63        // Hardware limit: 6 bits per timing stage
#define SMI_NUM_STAGES       4         // Setup, Strobe, Hold, Pace
#define SMI_MAX_TOTAL_CYCLES (SMI_MAX_STAGE_CYCLES * SMI_NUM_STAGES) // 252 cycles
#define SMI_MIN_TOTAL_CYCLES 4         // Minimum 1 cycle per stage

#define GPIO_FUNC_MASK       7         // 3 bits to clear GPIO function (111 in binary)
#define GPIO_FUNC_ALT1       5         // ALT1 function code for SMI
#define GPIO_PUPD_MASK       3         // 2 bits to clear Pull-Up/Down (11 in binary)


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

    // 1. Prepare GPIO as safe High-Z input
    hw->gpio[GPFSEL0] &= ~((GPIO_FUNC_MASK << 24) | (GPIO_FUNC_MASK << 27));
    hw->gpio[GPPUPPDN0] &= ~((GPIO_PUPD_MASK << 16) | (GPIO_PUPD_MASK << 18));
    

    // --- DYNAMIC FREQUENCY CALCULATION ---
    uint32_t clock_divisor = 2; // Start with a safe default divisor
    
    uint32_t base_clock = SMI_SOURCE_CLOCK_HZ / clock_divisor;
    uint32_t total_cycles = base_clock / target_hz;

    // Apply hardware boundaries
    if (total_cycles < SMI_MIN_TOTAL_CYCLES) {
        total_cycles = SMI_MIN_TOTAL_CYCLES;
    }
    
    if (total_cycles > SMI_MAX_TOTAL_CYCLES) {
        clock_divisor = total_cycles / SMI_MAX_STAGE_CYCLES;
        if (clock_divisor > SMI_MAX_DIVISOR) {
            clock_divisor = SMI_MAX_DIVISOR;
        }
        base_clock = SMI_SOURCE_CLOCK_HZ / clock_divisor;
        total_cycles = base_clock / target_hz;
    }

    // Distribute cycles (10% Setup, 50% Strobe, 20% Hold, 20% Pace)
    uint32_t setup  = (total_cycles * 1) / 10;
    uint32_t strobe = (total_cycles * 5) / 10;
    uint32_t hold   = (total_cycles * 2) / 10;
    
    if (setup == 0) setup = 1;
    if (strobe == 0) strobe = 1;
    if (hold == 0) hold = 1;

    uint32_t pace = total_cycles - setup - strobe - hold;
    if (pace == 0) pace = 1;
    if (pace > SMI_MAX_STAGE_CYCLES) pace = SMI_MAX_STAGE_CYCLES;

    // 2. Configure Hardware Clock
    hw->cm[CM_SMICTL / 4] = CM_PASSWD | (0 << 4); 
    usleep(10);
    hw->cm[CM_SMIDIV / 4] = CM_PASSWD | (clock_divisor << 12); 
    usleep(10);
    hw->cm[CM_SMICTL / 4] = CM_PASSWD | 1 | (1 << 4); 
    usleep(10);

    // 3. Initialize SMI timing and buffers
    hw->smi[SMI_CS_REG / 4] = SMI_CS_ENABLE | SMI_CS_CLEAR;
    usleep(10);
    
    hw->smi[SMI_SR0_REG / 4] = (setup << 24) | (strobe << 16) | (hold << 8) | pace; 
    hw->smi[SMI_L_REG / 4] = num_samples; 
    hw->smi[SMI_A_REG / 4] = 0;

    // 4. Set GPIO to ALT1 for SMI
    hw->gpio[GPFSEL0] |= ((GPIO_FUNC_ALT1 << 24) | (GPIO_FUNC_ALT1 << 27));

    // 5. Start Capture
    hw->smi[SMI_DCS_REG / 4] = SMI_DCS_ENABLE;
    hw->smi[SMI_CS_REG / 4] = SMI_CS_ENABLE | SMI_CS_START;
}

void smi_stop_capture(SmiHardware* hw) {
    if (!hw || !hw->gpio || !hw->smi) return;

    // CRITICAL FIX: Immediately revert pins to standard High-Z Input
    // This removes the SMI hardware from the bus and stops it from driving 0V.
    hw->gpio[GPFSEL0] &= ~((GPIO_FUNC_MASK << 24) | (GPIO_FUNC_MASK << 27));

    // Shut down the SMI engine
    hw->smi[SMI_CS_REG / 4] = 0;
    hw->smi[SMI_DCS_REG / 4] = 0;
}

void smi_cleanup(SmiHardware* hw) {
    if (!hw) return;
    if (hw->gpio != MAP_FAILED) hw->gpio[GPFSEL0] &= ~((GPIO_FUNC_MASK << 24) | (GPIO_FUNC_MASK << 27));
    if (hw->smi != MAP_FAILED) hw->smi[SMI_CS_REG / 4] = 0;
    if (hw->smi != MAP_FAILED) munmap((void*)hw->smi, 4096);
    if (hw->cm != MAP_FAILED) munmap((void*)hw->cm, 4096);
    if (hw->gpio != MAP_FAILED) munmap((void*)hw->gpio, 4096);
}