#include <stdio.h>
#include <stdint.h>
#include "usb_decoder.h"

// Define general packet types based on PID
typedef enum {
    PKT_TYPE_TOKEN,
    PKT_TYPE_DATA,
    PKT_TYPE_HANDSHAKE,
    PKT_TYPE_UNKNOWN
} PacketType;

/**
 * Helper to extract an integer value from a sequence of bits.
 * USB transmits the LSB (Least Significant Bit) first.
 */
static uint32_t extract_bits_lsb_first(const uint8_t* bits, int start, int len) {
    uint32_t val = 0;
    for (int i = 0; i < len; i++) {
        if (bits[start + i]) {
            val |= (1 << i);
        }
    }
    return val;
}

/**
 * Calculates the USB CRC5 used for Token packets (IN/OUT/SETUP).
 * Polynomial: 0x14 (x^5 + x^2 + 1)
 */
static uint8_t calculate_crc5(const uint8_t* bits, int start, int len) {
    uint8_t res = 0x1F;
    for (int i = 0; i < len; i++) {
        uint8_t bit = bits[start + i];
        uint8_t b = (bit ^ res) & 1;
        res >>= 1;
        if (b) {
            res ^= 0x14;
        }
    }
    return res ^ 0x1F;
}

/**
 * Calculates the USB CRC16 used for Data packets.
 * Polynomial: 0xA001
 */
static uint16_t calculate_crc16(const uint8_t* bits, int start, int len) {
    uint16_t res = 0xFFFF;
    for (int i = 0; i < len; i++) {
        uint8_t bit = bits[start + i];
        uint8_t b = (bit ^ res) & 1;
        res >>= 1;
        if (b) {
            res ^= 0xA001;
        }
    }
    return res ^ 0xFFFF;
}

void analyze_usb_packet(const uint8_t* pkt, int len, int print_binary) {
    if (len < 8) {
        printf("\n--- USB Packet Analysis: Error - Too short (%d bits) ---\n", len);
        return;
    }

    // Convert the first 8 chronological bits to a single integer for PID matching.
    // This matches the Python mapping logic (e.g. "10000111" -> 0x87)
    uint8_t pid_val = 0;
    for(int i = 0; i < 8; i++) {
        pid_val |= (pkt[i] << (7 - i));
    }

    const char* pid_name = "Unknown PID";
    PacketType type = PKT_TYPE_UNKNOWN;

    // Match PID string mappings to hex values
    switch(pid_val) {
        case 0x87: pid_name = "OUT";   type = PKT_TYPE_TOKEN; break;
        case 0x96: pid_name = "IN";    type = PKT_TYPE_TOKEN; break;
        case 0xA5: pid_name = "SOF";   type = PKT_TYPE_TOKEN; break;
        case 0xB4: pid_name = "SETUP"; type = PKT_TYPE_TOKEN; break;
        case 0xC3: pid_name = "DATA0"; type = PKT_TYPE_DATA; break;
        case 0xD2: pid_name = "DATA1"; type = PKT_TYPE_DATA; break;
        case 0xE1: pid_name = "DATA2"; type = PKT_TYPE_DATA; break;
        case 0xF0: pid_name = "MDATA"; type = PKT_TYPE_DATA; break;
        case 0x4B: pid_name = "ACK";   type = PKT_TYPE_HANDSHAKE; break;
        case 0x5A: pid_name = "NAK";   type = PKT_TYPE_HANDSHAKE; break;
        case 0x78: pid_name = "STALL"; type = PKT_TYPE_HANDSHAKE; break;
        case 0x69: pid_name = "NYET";  type = PKT_TYPE_HANDSHAKE; break;
        case 0x3C: pid_name = "PRE";   type = PKT_TYPE_UNKNOWN; break;
        case 0x1E: pid_name = "SPLIT"; type = PKT_TYPE_UNKNOWN; break;
        case 0x2D: pid_name = "PING";  type = PKT_TYPE_UNKNOWN; break;
        case 0x0F: pid_name = "Reserved"; type = PKT_TYPE_UNKNOWN; break;
    }

    printf("\n========= USB PACKET ANALYSIS =========\n");
    printf("PID:   %s\n", pid_name);

    if (type == PKT_TYPE_TOKEN) {
        // Token packets need 19 bits: 8 PID + 7 ADDR + 4 ENDP + 5 CRC5
        if (len < 19) {
            printf("Error: Incomplete Token Packet\n");
        } else {
            uint8_t addr = extract_bits_lsb_first(pkt, 8, 7);
            uint8_t endp = extract_bits_lsb_first(pkt, 15, 4);
            uint8_t crc5_received = extract_bits_lsb_first(pkt, 19, 5);
            uint8_t crc5_calc = calculate_crc5(pkt, 8, 11); // Calc CRC on ADDR + ENDP

            printf("ADDR:  %d\n", addr);
            printf("ENDP:  %d\n", endp);
            printf("CRC5:  0x%02X (%s)\n", crc5_received, (crc5_received == crc5_calc) ? "VALID" : "INVALID");
        }
    }
    else if (type == PKT_TYPE_DATA) {
        // Data packets need at least 24 bits: 8 PID + 0 data + 16 CRC16
        if (len < 24) { 
            printf("Error: Incomplete Data Packet\n");
        } else {
            int data_bits = len - 8 - 16;
            uint16_t crc16_received = extract_bits_lsb_first(pkt, len - 16, 16);
            uint16_t crc16_calc = calculate_crc16(pkt, 8, data_bits); // Calc CRC on DATA

            printf("DATA:  ");
            if (print_binary) {
                for(int i = 0; i < data_bits; i++) {
                    printf("%d", pkt[8 + i]);
                }
            } else {
                for(int i = 0; i < data_bits; i += 8) {
                    uint8_t byte_val = extract_bits_lsb_first(pkt, 8 + i, 8);
                    printf("%02X ", byte_val);
                }
            }
            printf("\nCRC16: 0x%04X (%s)\n", crc16_received, (crc16_received == crc16_calc) ? "VALID" : "INVALID");
        }
    }
    else if (type == PKT_TYPE_HANDSHAKE) {
        // Handshakes only contain a PID, so there is nothing extra to print
    }
    else {
        printf("Payload: Unknown or Raw bits\n");
    }
    printf("=======================================\n\n");
}