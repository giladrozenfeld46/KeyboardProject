#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>

#include "smi_manager.h"
#include "dma_control.h"
#include "dma_mem.h"
#include "smi_hal.h"

// Configuration for multiple separate buffers
#define BUFFER_SAMPLES       1024
#define BUFFER_BYTES         (BUFFER_SAMPLES * 4)
#define NUM_BUFFERS          5
#define NUM_BUFFERS_RX       4 // Reserve the 16th buffer strictly for TX
#define TX_BUFFER_INDEX      5

// Optimization: Number of samples to fetch in a single burst (must be a divisor of BUFFER_SAMPLES)
#define CHUNK_SIZE           8

// DMA Configuration: SMI DREQ, Wait for DREQ, Increment Dest, Interrupt Enable
#define DMA_TI_SMI_CIRCULAR  ((4 << 16) | (1 << 10) | (1 << 4) | (1 << 2))

// DMA Configuration for TX (Wait for DEST DREQ, Increment Source)
#define DMA_TI_SMI_TX        ((4 << 16) | (1 << 6) | (1 << 8))

// DMA Configuration for Register Setup (No DREQ, just copy 1 word instantly)
#define DMA_TI_NO_DREQ       (1 << 3) // WAIT_RESP

// Physical bus addresses for SMI registers (for Pi 4)
#define SMI_CS_REG           0x7E600000
#define SMI_DATA_REG         0x7E60000C

// --- GLOBAL MODULE STATE ---
static DmaBuffer cb_buf;
static DmaBuffer tx_cb_buf; // Control blocks for the Tx sequence
static DmaBuffer sample_bufs[NUM_BUFFERS];
static SmiHardware smi_hw;
static volatile uint32_t *dma_base = NULL;
static volatile uint32_t *dma_chan5 = NULL; // RX Channel
static volatile uint32_t *dma_chan4 = NULL; // TX Channel
static int mem_fd = -1;

// --- GLOBAL READ POINTERS ---
static int g_current_read_buffer = 0;
static int g_current_read_index = 0;

int smi_manager_init(uint32_t sample_rate) {
    printf("Initializing SMI Manager...\n");

    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("Failed to open /dev/mem");
        return -1;
    }

    // 1. Allocate sample buffers (15 for RX, 1 for TX)
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (allocate_dma_buffer(&sample_bufs[i], BUFFER_BYTES) != 0) {
            printf("Failed to allocate sample buffer %d.\n", i);
            for (int j = 0; j < i; j++) {
                free_dma_buffer(&sample_bufs[j]);
            }
            return -1;
        }
    }

    // 2. Allocate memory for RX Control Blocks
    if (allocate_dma_buffer(&cb_buf, sizeof(struct DmaControlBlock) * NUM_BUFFERS_RX) != 0) {
        printf("Failed to allocate RX control blocks.\n");
        for (int i = 0; i < NUM_BUFFERS; i++) {
            free_dma_buffer(&sample_bufs[i]);
        }
        return -1;
    }

    // 2.5 Allocate memory for TX Control Blocks (3 CBs + 2 configuration words = 104 bytes -> padding to 128)
    if (allocate_dma_buffer(&tx_cb_buf, sizeof(struct DmaControlBlock) * 3 + 8) != 0) {
        printf("Failed to allocate TX control blocks.\n");
        for (int i = 0; i < NUM_BUFFERS; i++) {
            free_dma_buffer(&sample_bufs[i]);
        }
        return -1;
    }

    struct DmaControlBlock* cbs = (struct DmaControlBlock*)cb_buf.virtual_addr;

    // 3. Chain the RX blocks together circularly (Using 15 buffers)
    for (int i = 0; i < NUM_BUFFERS_RX; i++) {
        setup_dma_control_block(&cbs[i], DMA_TI_SMI_CIRCULAR, SMI_DATA_REG, 
                                sample_bufs[i].bus_addr, BUFFER_BYTES);
        
        if (i == NUM_BUFFERS_RX - 1) {
            cbs[i].nextconbk = cb_buf.bus_addr; // Loop back to start
        } else {
            cbs[i].nextconbk = cb_buf.bus_addr + ((i + 1) * sizeof(struct DmaControlBlock));
        }
    }

    // 3.5 Set up TX Control Blocks (Setup -> Send Data -> Restore)
    struct DmaControlBlock* tx_cbs = (struct DmaControlBlock*)tx_cb_buf.virtual_addr;
    // Place config words right after the 3 CBs to keep everything in the same DMA memory block
    uint32_t* configs = (uint32_t*)((uint8_t*)tx_cbs + (3 * sizeof(struct DmaControlBlock))); 

    // WRITE Mode: Enable(1) + Start(4) + Clear(8) + Write(16) = 0x1D
    configs[0] = 0x39; 
    // READ Mode: Enable(1) + Start(4) + Clear(8) = 0x0D
    configs[1] = 0x0D; 

    uint32_t configs_bus_addr = tx_cb_buf.bus_addr + (3 * sizeof(struct DmaControlBlock));

    // CB 0: Write config to SMI CS (Switch to TX Mode)
    setup_dma_control_block(&tx_cbs[0], DMA_TI_NO_DREQ, configs_bus_addr, SMI_CS_REG, 4);
    tx_cbs[0].nextconbk = tx_cb_buf.bus_addr + sizeof(struct DmaControlBlock);

    // CB 1: Send Data to SMI FIFO (Length is dynamic, will be set during write call)
    setup_dma_control_block(&tx_cbs[1], DMA_TI_SMI_TX, sample_bufs[TX_BUFFER_INDEX].bus_addr, SMI_DATA_REG, 0);
    tx_cbs[1].nextconbk = tx_cb_buf.bus_addr + 2 * sizeof(struct DmaControlBlock);

    // CB 2: Restore config to SMI CS (Switch back to RX Mode instantly)
    setup_dma_control_block(&tx_cbs[2], DMA_TI_NO_DREQ, configs_bus_addr + 4, SMI_CS_REG, 4);
    tx_cbs[2].nextconbk = 0; // End of chain

    // Initialize hardware
    smi_init(&smi_hw, mem_fd);

    // Map DMA channels 4 and 5
    dma_base = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, 0xFE007000);
    if (dma_base == MAP_FAILED) {
        perror("Failed to mmap DMA");
        for (int i = 0; i < NUM_BUFFERS; i++) {
            free_dma_buffer(&sample_bufs[i]);
        }
        return -1;
    }

    dma_chan5 = dma_base + (0x500 / 4); // RX Channel base address
    dma_chan4 = dma_base + (0x400 / 4); // TX Channel base address

    // 4. Hardware Reset for DMA 
    dma_chan5[0] = (1 << 31); 
    dma_chan4[0] = (1 << 31);
    usleep(1000); 
    dma_chan5[0] = 0;
    dma_chan4[0] = 0;

    // Reset read pointers
    g_current_read_buffer = 0;
    g_current_read_index = 0;

    // 5. Start SMI and DMA RX
    smi_start_capture(&smi_hw, 0xFFFFFFFF, sample_rate); 
    start_dma_channel((volatile uint32_t*)dma_chan5, cb_buf.bus_addr);

    printf("SMI and DMA RX initialized and running.\n");
    return 0;
}

int smi_manager_read_chunk(uint32_t *out_samples) {
    uint32_t current_cb_bus_addr = dma_chan5[1]; // CONBLK_AD
    int active_buffer_index = -1;

    for (int i = 0; i < NUM_BUFFERS_RX; i++) {
        uint32_t expected_addr = cb_buf.bus_addr + (i * sizeof(struct DmaControlBlock));
        if (current_cb_bus_addr == expected_addr) {
            active_buffer_index = i;
            break;
        }
    }

    if (active_buffer_index == -1) {
        return 0; 
    }

    uint32_t remaining_bytes = dma_chan5[3]; // TX_LEN
    uint32_t written_bytes = BUFFER_BYTES - remaining_bytes;
    uint32_t active_index_samples = written_bytes / 4;

    if (g_current_read_buffer == active_buffer_index) {
        if (g_current_read_index + CHUNK_SIZE > active_index_samples) {
            return 0; 
        }
    }

    uint32_t* buffer_data = (uint32_t*)sample_bufs[g_current_read_buffer].virtual_addr;
    memcpy(out_samples, &buffer_data[g_current_read_index], CHUNK_SIZE * sizeof(uint32_t));
    g_current_read_index += CHUNK_SIZE;

    // Wrap around logic for the buffers
    if (g_current_read_index >= BUFFER_SAMPLES) {
        g_current_read_index = 0;
        g_current_read_buffer++;
        
        if (g_current_read_buffer >= NUM_BUFFERS_RX) {
            g_current_read_buffer = 0;
        }
    }

    return 1;
}

int smi_manager_write_data(uint32_t *data, int length) {
    if (!data || length <= 0 || length > BUFFER_SAMPLES) {
    printf("Error: Invalid TX data length.\n");
    return -1;
    }

    // 1. Copy the data array to our dedicated TX DMA buffer
    memcpy((void*)sample_bufs[TX_BUFFER_INDEX].virtual_addr, data, length * sizeof(uint32_t));

    // 2. Set the dynamic length in the middle Control Block (CB 1)
    struct DmaControlBlock* tx_cbs = (struct DmaControlBlock*)tx_cb_buf.virtual_addr;
    tx_cbs[1].txfr_len = length * sizeof(uint32_t);

    // 3. Trigger DMA Channel 4!
    // This stops RX implicitly (because SMI switches direction), pumps the data, 
    // and then sets SMI back to RX, allowing DMA Channel 5 to resume automatically.
    start_dma_channel((volatile uint32_t*)dma_chan4, tx_cb_buf.bus_addr);

    // 4. Block until transmission is complete
    while (dma_chan4[0] & 1) { // ACTIVE bit
        usleep(1);
    }

    return 0;
}

void smi_manager_cleanup() {
    printf("Stopping SMI and DMA hardware...\n");

    if (dma_chan5) {
        // HARD RESET: Instantly freeze the DMA channels
        dma_chan5[0] = (1 << 31);
        dma_chan4[0] = (1 << 31);
    }

    smi_stop_capture(&smi_hw);

    if (dma_base && dma_base != MAP_FAILED) {
        munmap((void*)dma_base, 4096);
        dma_base = NULL;
    }

    // Free all scattered buffers
    for (int i = 0; i < NUM_BUFFERS; i++) {
        free_dma_buffer(&sample_bufs[i]);
    }
    free_dma_buffer(&cb_buf);
    free_dma_buffer(&tx_cb_buf);

    if (mem_fd >= 0) {
        close(mem_fd);
        mem_fd = -1;
    }

    printf("SMI Manager cleaned up successfully.\n");
}