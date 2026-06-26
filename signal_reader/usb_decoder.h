#ifndef USB_DECODER_H
#define USB_DECODER_H

#include <stdint.h>
#include <stdio.h> // Added for FILE*

// Define general packet types based on PID
typedef enum {
    PKT_TYPE_TOKEN,
    PKT_TYPE_DATA,
    PKT_TYPE_HANDSHAKE,
    PKT_TYPE_UNKNOWN
} PacketType;

// Structure to hold all decoded packet information
typedef struct {
    uint8_t pid_val;
    const char* pid_name;
    PacketType type;
    
    // Token packet fields
    uint8_t addr;
    uint8_t endp;
    uint8_t crc5_received;
    uint8_t crc5_calc;
    int crc5_valid;
    
    // Data packet fields
    const uint8_t* data_bits; // Pointer to the start of the data payload in the original buffer
    int data_bits_len;
    uint16_t crc16_received;
    uint16_t crc16_calc;
    int crc16_valid;
    
    // General validation
    int total_length;
    int is_valid_length;
} UsbPacket;

/**
 * Analyzes a stream of NRZI decoded USB bits and extracts packet information into a struct.
 * @param pkt Array of bits (0s and 1s)
 * @param len Number of bits in the array
 * @return UsbPacket struct containing the decoded data
 */
UsbPacket analyze_usb_packet(const uint8_t* pkt, int len);

/**
 * Prints the content of a decoded UsbPacket struct directly to stdout.
 * @param packet Pointer to the UsbPacket struct
 * @param print_binary Set to 1 to print data payload as binary, 0 for Hex
 */
void print_usb_packet(const UsbPacket* packet, int print_binary);

/**
 * Core printing logic that writes a decoded UsbPacket to a given FILE stream.
 * @param stream Target FILE pointer (e.g., stdout or an opened file)
 * @param packet Pointer to the UsbPacket struct
 * @param print_binary Set to 1 to print data payload as binary, 0 for Hex
 */
void fprint_usb_packet(FILE* stream, const UsbPacket* packet, int print_binary);

#endif // USB_DECODER_H