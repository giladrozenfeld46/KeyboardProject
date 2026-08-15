#include <stdio.h>
#include <stdlib.h>
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"

// Header generated automatically by pioasm
#include "continuous_sample.pio.h" 

#define SAMPLE_PIN 2
#define TARGET_FREQ_HZ 1500000 

// Number of discrete buffers (must be a power of 2 for the DMA ring feature, e.g., 2, 4, 8, 16)
#define NUM_BUFFERS 4
// Size of each disjoint buffer in 32-bit words (e.g., 1024 words = 4KB per buffer)
#define BUFFER_SIZE_WORDS 1024 

#define SAMPLE_CONSUMED 0xFFFFFFFF

// Array of pointers to hold the allocated disjoint buffers
uint32_t* data_buffers[NUM_BUFFERS];


// Global trackers for the reading position
static int current_read_buffer = 0;
static int current_read_word = 0;

// Control blocks: an array holding the physical start addresses of our buffers.
// The array MUST be aligned in memory to its total size for the DMA ring to work.
// 4 buffers * 4 bytes per address = 16 bytes alignment.
uint32_t __attribute__((aligned(16))) control_blocks[NUM_BUFFERS];

// Global DMA channels
int data_chan;
int ctrl_chan;

// Dedicated function to set up the Scatter-Gather DMA in a continuous loop
void setup_scatter_gather_dma(PIO pio, uint sm, int num_buffers, int buffer_size_words) {
    // 1. Allocate disjoint buffers and populate the control blocks array
    for (int i = 0; i < num_buffers; i++) {
        // Allocate memory for each buffer (they do not need to be contiguous in physical memory)
        data_buffers[i] = (uint32_t*)malloc(buffer_size_words * sizeof(uint32_t));
        if (data_buffers[i] == NULL) {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        // Store the address in the control blocks array
        control_blocks[i] = (uint32_t)data_buffers[i];
    }

    // 2. Claim two free DMA channels
    data_chan = dma_claim_unused_channel(true);
    ctrl_chan = dma_claim_unused_channel(true);

    // 3. Configure the Control Channel
    dma_channel_config ctrl_conf = dma_channel_get_default_config(ctrl_chan);
    channel_config_set_transfer_data_size(&ctrl_conf, DMA_SIZE_32);
    channel_config_set_read_increment(&ctrl_conf, true);   // Read next address from control blocks
    channel_config_set_write_increment(&ctrl_conf, false); // Write to the same register always
    
    // Set ring buffer on the read address to loop back automatically
    // The ring size is specified in bits (1<<N bytes). For 16 bytes (4*4), N = 4.
    channel_config_set_ring(&ctrl_conf, false, 4); 

    dma_channel_configure(
        ctrl_chan,
        &ctrl_conf,
        &dma_hw->ch[data_chan].al2_write_addr_trig, // Destination: Data channel write address trigger
        control_blocks,                             // Source: Control blocks array
        1,                                          // Transfer 1 address at a time
        false                                       // Don't start yet
    );

    // 4. Configure the Data Channel
    dma_channel_config data_conf = dma_channel_get_default_config(data_chan);
    channel_config_set_transfer_data_size(&data_conf, DMA_SIZE_32);
    channel_config_set_read_increment(&data_conf, false); // Always read from PIO RX FIFO
    channel_config_set_write_increment(&data_conf, true); // Increment address in the current buffer
    
    // Wait for the PIO state machine to signal data is ready
    channel_config_set_dreq(&data_conf, pio_get_dreq(pio, sm, false));
    
    // Chain to the control channel: when data channel finishes its transfer, start control channel
    channel_config_set_chain_to(&data_conf, ctrl_chan);

    dma_channel_configure(
        data_chan,
        &data_conf,
        NULL,                        // Will be dynamically configured by the control channel
        &pio->rxf[sm],               // Source: PIO RX FIFO
        buffer_size_words,           // Number of 32-bit transfers per block
        false                        // Don't start yet
    );
}

int main() {
    stdio_init_all();

    // Initialize PIO
    PIO pio = pio0;
    uint sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &continuous_sample_program);
    
    pio_sm_set_consecutive_pindirs(pio, sm, SAMPLE_PIN, 1, false);
    pio_gpio_init(pio, SAMPLE_PIN);
    
    pio_sm_config c = continuous_sample_program_get_default_config(offset);
    sm_config_set_in_pins(&c, SAMPLE_PIN);
    
    // Push automatically when 32 bits are collected
    sm_config_set_in_shift(&c, false, true, 32); 
    
    // Set sampling frequency
    float div = (float)clock_get_hz(clk_sys) / TARGET_FREQ_HZ;
    sm_config_set_clkdiv(&c, div);
    
    pio_sm_init(pio, sm, offset, &c);

    // Initialize the Scatter-Gather DMA
    setup_scatter_gather_dma(pio, sm, NUM_BUFFERS, BUFFER_SIZE_WORDS);

    printf("Starting endless Scatter-Gather DMA at %d Hz...\n", TARGET_FREQ_HZ);

    // Start the process: manually trigger the control channel for the first transfer.
    // It will load the address of data_buffers[0] into the data channel and start it.
    dma_channel_start(ctrl_chan);
    
    // Enable the PIO state machine to start feeding data
    pio_sm_set_enabled(pio, sm, true);

    // Infinite loop representing the main application logic
    while (true) {
        // Note: In a real-time application (like a logic analyzer), 
        // you would monitor the DMA progress here (e.g., using interrupts or polling)
        // to process data_buffers[N] while the DMA is actively writing to data_buffers[N+1].
        
        tight_loop_contents();
    }

    return 0;
}

void process_incoming_samples(void) {
    // Infinite loop to continuously process samples as they arrive
    while (true) {
        // Fetch the next sample seamlessly across buffer boundaries
        uint32_t new_sample = get_next_sample();
        
        // Add your logic to analyze the data here
        // Example: printf("Sample: 0x%08X\n", new_sample);
    }
}