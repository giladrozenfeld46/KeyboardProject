#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h> 

#include "dma_mem.h"
#include "dma_control.h"
#include "smi_hal.h"
#include "smi_manager.h"

#define BUFFER_SAMPLES       1024 
#define BUFFER_BYTES         (BUFFER_SAMPLES * 4) 
#define NUM_RX_BUFFERS       4
#define CHUNK_SIZE           8

// DMA Configuration 
#define DMA_TI_SMI_CIRCULAR  ((4 << 16) | (1 << 10) | (1 << 4) | (1 << 2))
#define DMA_TI_SMI_TX        ((5 << 16) | (1 << 8) | (0 << 4))
#define DMA_TI_MEM_TO_MEM    ((0 << 16) | (1 << 8) | (0 << 4)) // Unpaced memory copy

// Physical & Bus Addresses (Assuming Pi 4 based on 0xFE base)
#define SMI_PHYSICAL_BASE    0xFE600000 
#define SMI_D_BUS_ADDR       0x7E60000C
#define DMA_CHAN_RX_CS_BUS   0x7E007500 // Bus address for Channel 5 CS register

// SMI CS Register Bits
#define SMI_CS_ENABLE        (1 << 0)
#define SMI_CS_START         (1 << 1)
#define SMI_CS_DONE          (1 << 2)
#define SMI_CS_WRITE         (1 << 3)

// --- GLOBAL MODULE STATE ---
static DmaBuffer rx_cb_buf;
static DmaBuffer tx_cb_buf; 
static DmaBuffer rx_sample_bufs[NUM_RX_BUFFERS]; 
static DmaBuffer tx_sample_buf;
static DmaBuffer dma_wakeup_buf; // Holds the hardware resume command

static SmiHardware smi_hw;
static volatile uint32_t *dma_base = NULL;
static volatile uint32_t *dma_chan_rx = NULL; // Channel 5
static volatile uint32_t *dma_chan_tx = NULL; // Channel 6
static int mem_fd = -1; // FIXED: Global mem_fd declaration added

// Direct SMI Registers mapping for Inlining
static volatile uint32_t *smi_regs = NULL; 

static uint32_t g_target_rate_hz = 0;

// Read Pointers
uint32_t g_current_read_buffer = 0;
uint32_t g_current_read_index = 0;

int smi_manager_init(uint32_t target_rate_hz) {
    printf("Initializing Accelerated SMI Manager...\n");
    g_target_rate_hz = target_rate_hz;
    
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) return -1;

    // Allocate Buffers
    for (int i = 0; i < NUM_RX_BUFFERS; i++) {
        allocate_dma_buffer(&rx_sample_bufs[i], BUFFER_BYTES);
        memset((void*)rx_sample_bufs[i].virtual_addr, 0xFF, BUFFER_BYTES);
    }
    allocate_dma_buffer(&tx_sample_buf, BUFFER_BYTES);
    
    // Allocate Wakeup Command Buffer for DMA hardware chaining
    allocate_dma_buffer(&dma_wakeup_buf, 4);
    *((volatile uint32_t*)dma_wakeup_buf.virtual_addr) = 1; // 1 = DMA ACTIVE bit

    // Allocate Control Blocks (TX needs 2 blocks now)
    allocate_dma_buffer(&rx_cb_buf, sizeof(struct DmaControlBlock) * NUM_RX_BUFFERS);
    allocate_dma_buffer(&tx_cb_buf, sizeof(struct DmaControlBlock) * 2);
    
    // Setup Circular Chain for RX
    struct DmaControlBlock* rx_cbs = (struct DmaControlBlock*)rx_cb_buf.virtual_addr;
    for (int i = 0; i < NUM_RX_BUFFERS; i++) {
        setup_dma_control_block(&rx_cbs[i], DMA_TI_SMI_CIRCULAR, SMI_D_BUS_ADDR, 
                                rx_sample_bufs[i].bus_addr, BUFFER_BYTES);
        rx_cbs[i].nextconbk = (i == NUM_RX_BUFFERS - 1) ? rx_cb_buf.bus_addr : 
                              rx_cb_buf.bus_addr + ((i + 1) * sizeof(struct DmaControlBlock));
    }

    // Setup TX Chain
    struct DmaControlBlock* tx_cbs = (struct DmaControlBlock*)tx_cb_buf.virtual_addr;
    
    // TX CB 0: Payload Transfer to SMI
    setup_dma_control_block(&tx_cbs[0], DMA_TI_SMI_TX, tx_sample_buf.bus_addr, SMI_D_BUS_ADDR, BUFFER_BYTES);
    tx_cbs[0].nextconbk = tx_cb_buf.bus_addr + sizeof(struct DmaControlBlock); // Chain to CB 1
    
    // TX CB 1: Hardware-triggered Wakeup of RX DMA
    // Writes the value '1' directly into the RX DMA Channel CS register
    setup_dma_control_block(&tx_cbs[1], DMA_TI_MEM_TO_MEM, dma_wakeup_buf.bus_addr, DMA_CHAN_RX_CS_BUS, 4);
    tx_cbs[1].nextconbk = 0; // End of chain

    // Map Hardware Registers
    dma_base = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, 0xFE007000);
    smi_regs = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, SMI_PHYSICAL_BASE);
    
    dma_chan_rx = dma_base + (0x500 / 4); 
    dma_chan_tx = dma_base + (0x600 / 4); 
    
    // Reset DMA Channels
    dma_chan_rx[0] = (1 << 31); 
    dma_chan_tx[0] = (1 << 31);
    usleep(1000); 
    dma_chan_rx[0] = 0;
    dma_chan_tx[0] = 0;

    // Start Operations
    smi_init(&smi_hw, mem_fd);
    start_dma_channel((volatile uint32_t*)dma_chan_rx, rx_cb_buf.bus_addr);
    smi_start_capture(&smi_hw, 0xFFFFFFFF, target_rate_hz); 

    return 0;
}

int smi_manager_read_chunk(uint32_t *out_samples) {
    if (!out_samples) return 0;

    volatile uint32_t* current_buffer_ptr = (volatile uint32_t*)rx_sample_bufs[g_current_read_buffer].virtual_addr;

    if (current_buffer_ptr[g_current_read_index + (CHUNK_SIZE - 1)] == 0xFFFFFFFF) {
        return 0; 
    }

    memcpy(out_samples, (void*)&current_buffer_ptr[g_current_read_index], CHUNK_SIZE * sizeof(uint32_t));
    memset((void*)&current_buffer_ptr[g_current_read_index], 0xFF, CHUNK_SIZE * sizeof(uint32_t));
    
    g_current_read_index += CHUNK_SIZE;
    
    if (g_current_read_index >= BUFFER_SAMPLES) {
        g_current_read_index = 0;
        g_current_read_buffer++;
        if (g_current_read_buffer >= NUM_RX_BUFFERS) {
            g_current_read_buffer = 0;
        }
    }

    return 1;
}

void smi_manager_write_sequence(const uint8_t* gpio8_seq, const uint8_t* gpio9_seq, size_t length) {
    if (length > BUFFER_SAMPLES) length = BUFFER_SAMPLES;

    // 1. PRE-PROCESS payload
    volatile uint32_t* tx_buf = (volatile uint32_t*)tx_sample_buf.virtual_addr;
    for (size_t i = 0; i < length; i++) {
        uint32_t word = 0;
        if (gpio8_seq[i]) word |= (1 << 0);
        if (gpio9_seq[i]) word |= (1 << 1);
        tx_buf[i] = word;
    }

    // Update payload length dynamically in the TX Control Block
    struct DmaControlBlock* tx_cbs = (struct DmaControlBlock*)tx_cb_buf.virtual_addr;
    tx_cbs[0].txfr_len = length * sizeof(uint32_t); // FIXED: Using txfr_len instead of length

    // 2. INLINE PAUSE RX DMA
    dma_chan_rx[0] &= ~1; 
    while (dma_chan_rx[0] & 1); // Wait for physical pause
    
    smi_regs[0] = 0; // Disable SMI to clear state

    // 3. INLINE CONFIGURE SMI TX
    smi_regs[1] = length; // Set transfer length in SMI L register
    smi_regs[0] = SMI_CS_ENABLE | SMI_CS_WRITE | SMI_CS_START; 

    // 4. FIRE TX DMA (This handles data + hardware RX wake up automatically)
    dma_chan_tx[1] = tx_cb_buf.bus_addr; // Point to CB 0
    dma_chan_tx[0] = 1; // Start TX DMA

    // 5. INLINE POLL SMI DONE
    // The DMA finishes quickly, but we MUST wait for the SMI FIFO to push out the last bits
    while (!(smi_regs[0] & SMI_CS_DONE));

    // 6. INLINE SWITCH TO RX
    // At this exact moment, TX DMA has already executed CB 1 and set RX DMA ACTIVE bit.
    // RX DMA is awake, but waiting for DREQ. We now provide the DREQ.
    smi_regs[0] = 0; 
    smi_regs[1] = 0xFFFFFFFF; // Infinite read
    smi_regs[0] = SMI_CS_ENABLE | SMI_CS_START; // Read mode
}

void smi_manager_cleanup() {
    printf("Stopping SMI and DMA hardware...\n");
    
    // FIXED: Updated to correctly reference the new channel pointers
    if (dma_chan_rx) dma_chan_rx[0] = (1 << 31);
    if (dma_chan_tx) dma_chan_tx[0] = (1 << 31);
    
    smi_stop_capture(&smi_hw);
    smi_cleanup(&smi_hw);
    
    if (dma_base && dma_base != MAP_FAILED) {
        munmap((void*)dma_base, 4096);
    }
    
    // FIXED: Cleanup uses the globally declared mem_fd
    if (mem_fd >= 0) {
        close(mem_fd);
    }
    
    // FIXED: Memory freeing logic updated for the separated RX and TX buffers
    for (int i = 0; i < NUM_RX_BUFFERS; i++) {
        free_dma_buffer(&rx_sample_bufs[i]);
    }
    free_dma_buffer(&tx_sample_buf);
    free_dma_buffer(&dma_wakeup_buf);
    
    free_dma_buffer(&rx_cb_buf);
    free_dma_buffer(&tx_cb_buf);
    
    printf("SMI Manager cleaned up successfully.\n");
}