#include "smi_hal.h"
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

// Hardware physical base addresses
#define BCM2711_PERI_BASE    0xFE000000 
#define SMI_BASE             (BCM2711_PERI_BASE + 0x600000)
#define CM_BASE              (BCM2711_PERI_BASE + 0x101000) 
#define GPIO_BASE            (BCM2711_PERI_BASE + 0x200000)

// --- SMI Register offsets ---
#define SMI_CS_REG           0x00 
#define SMI_L_REG            0x04 
#define SMI_A_REG            0x08 
#define SMI_D_REG            0x0C 
#define SMI_DSR0_REG         0x10  
#define SMI_DSW0_REG         0x14
#define SMI_DMC_REG          0x30  

// --- Clock Manager (CM) & GPIO Register offsets ---
#define CM_SMICTL            0xB0
#define CM_SMIDIV            0xB4
#define CM_PASSWD            0x5A000000 
#define GPFSEL0              0x00 
#define GPPUPPDN0            0x39 

// SMI Control Bits
#define SMI_CS_ENABLE        (1 << 0)
#define SMI_CS_START         (1 << 3)  
#define SMI_CS_CLEAR         (1 << 4)  

// SMI DMA Control Bits
#define SMI_DMC_DMAEN        (1 << 28) 
#define SMI_DMC_PANICR_SHIFT 18
#define SMI_DMC_REQR_SHIFT   6
#define SMI_DMC_REQR_1       (1 << SMI_DMC_REQR_SHIFT)   
#define SMI_DMC_PANICR_1     (1 << SMI_DMC_PANICR_SHIFT) 

// Timing Register Shifts
#define SMI_DSR_SETUP_SHIFT  28
#define SMI_DSR_STROBE_SHIFT 14
#define SMI_DSR_HOLD_SHIFT   7
#define SMI_DSR_PACE_SHIFT   0

// Hardware Constraints & Clock definitions
#define SMI_SOURCE_CLOCK_HZ  500000000 
#define SMI_MAX_DIVISOR      32        
#define SMI_MAX_TOTAL_CYCLES 252       
#define SMI_MIN_TOTAL_CYCLES 4         

#define GPIO_FUNC_MASK       7         
#define GPIO_FUNC_ALT1       5         
#define GPIO_PUPD_MASK       3         

#define CM_SMI_BUSY          (1 << 7)
#define CM_CLK_SRC_PLLD      6        // 500 MHz clock source

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
    hw->gpio[GPFSEL0] &= ~((GPIO_FUNC_MASK << 24) | (GPIO_FUNC_MASK << 27));
    hw->gpio[GPPUPPDN0] &= ~((GPIO_PUPD_MASK << 16) | (GPIO_PUPD_MASK << 18));

    // 2. Dynamic Clock Calculation based on 500MHz PLLD
    uint32_t clock_divisor = 2; 
    uint32_t base_clock = SMI_SOURCE_CLOCK_HZ / clock_divisor;
    uint32_t total_cycles = base_clock / target_hz;

    if (total_cycles < SMI_MIN_TOTAL_CYCLES) total_cycles = SMI_MIN_TOTAL_CYCLES;
    
    if (total_cycles > SMI_MAX_TOTAL_CYCLES) {
        clock_divisor = total_cycles / 63;
        if (clock_divisor > SMI_MAX_DIVISOR) clock_divisor = SMI_MAX_DIVISOR;
        base_clock = SMI_SOURCE_CLOCK_HZ / clock_divisor;
        total_cycles = base_clock / target_hz;
    }

    uint32_t setup  = (total_cycles * 1) / 10;
    uint32_t strobe = (total_cycles * 5) / 10;
    uint32_t hold   = (total_cycles * 2) / 10;
    
    if (setup == 0) setup = 1;
    if (strobe == 0) strobe = 1;
    if (hold == 0) hold = 1;

    uint32_t pace = total_cycles - setup - strobe - hold;
    if (pace == 0) pace = 1;
    
    if (setup > 63) setup = 63;
    if (strobe > 127) strobe = 127;
    if (hold > 63) hold = 63;
    if (pace > 127) pace = 127;

    // --- CRITICAL HARDWARE SEQUENCE: CLOCK RESET & UPDATE ---
    
    // Step A: Request clock stop (Clear ENAB bit)
    hw->cm[CM_SMICTL / 4] = CM_PASSWD | 0; 
    
    // Step B: Poll BUSY bit until the hardware successfully comes to a full stop
    int timeout = 100000;
    while ((hw->cm[CM_SMICTL / 4] & CM_SMI_BUSY) && timeout > 0) {
        timeout--;
    }

    // Step C: Now that the clock generator is idle, write the new divisor safely
    hw->cm[CM_SMIDIV / 4] = CM_PASSWD | (clock_divisor << 12); 
    usleep(10);
    
    // Step D: Start the clock generator, mapping it to source 6 (PLLD = 500MHz)
    hw->cm[CM_SMICTL / 4] = CM_PASSWD | CM_CLK_SRC_PLLD | (1 << 4); 
    usleep(10);

    // 4. Initialize SMI timing configurations
    hw->smi[SMI_CS_REG / 4] = SMI_CS_ENABLE | SMI_CS_CLEAR;
    usleep(10);
    
    // --- READ TIMING ---
    uint32_t timing_value = (setup << SMI_DSR_SETUP_SHIFT) | 
                            (strobe << SMI_DSR_STROBE_SHIFT) | 
                            (hold << SMI_DSR_HOLD_SHIFT) | 
                            (pace << SMI_DSR_PACE_SHIFT);
                            
    hw->smi[SMI_DSR0_REG / 4] = timing_value; 
    
    // --- WRITE TIMING (NEW) ---
    // Copy the exact same timing to the DSW0 register so Tx is at the same speed as Rx
    hw->smi[SMI_DSW0_REG / 4] = timing_value;
                                
    hw->smi[SMI_L_REG / 4] = num_samples; 
    hw->smi[SMI_A_REG / 4] = 0;

    // 5. Set GPIO to ALT1 for SMI
    hw->gpio[GPFSEL0] |= ((GPIO_FUNC_ALT1 << 24) | (GPIO_FUNC_ALT1 << 27));

    // 6. Start Capture and Enable DMA Request Generation
    hw->smi[SMI_DMC_REG / 4] = SMI_DMC_DMAEN | SMI_DMC_REQR_1 | SMI_DMC_PANICR_1;
    hw->smi[SMI_CS_REG / 4] = SMI_CS_ENABLE | SMI_CS_START;
}

void smi_stop_capture(SmiHardware* hw) {
    if (!hw || !hw->gpio || !hw->smi) return;

    // Instantly revert pins to standard High-Z Input to stop driving the bus
    hw->gpio[GPFSEL0] &= ~((GPIO_FUNC_MASK << 24) | (GPIO_FUNC_MASK << 27));

    // Shut down the SMI engine
    hw->smi[SMI_CS_REG / 4] = 0;
    hw->smi[SMI_DMC_REG / 4] = 0; 
}

void smi_cleanup(SmiHardware* hw) {
    if (!hw) return;
    
    smi_stop_capture(hw);
    
    // Unmap memory
    if (hw->smi != MAP_FAILED) munmap((void*)hw->smi, 4096);
    if (hw->cm != MAP_FAILED) munmap((void*)hw->cm, 4096);
    if (hw->gpio != MAP_FAILED) munmap((void*)hw->gpio, 4096);
}