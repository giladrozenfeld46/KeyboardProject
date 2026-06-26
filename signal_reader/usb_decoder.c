#include <stdio.h>
#include <stdint.h>
#include "usb_decoder.h"

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

UsbPacket analyze_usb_packet(const uint8_t* pkt, int len) {
    UsbPacket packet = {0};
    packet.total_length = len;
    packet.is_valid_length = 1;

    if (len < 8) {
        packet.is_valid_length = 0;
        packet.pid_name = "Error: Too short";
        packet.type = PKT_TYPE_UNKNOWN;
        return packet;
    }

    // Convert the first 8 chronological bits to a single integer for PID matching.
    packet.pid_val = 0;
    for(int i = 0; i < 8; i++) {
        packet.pid_val |= (pkt[i] << (7 - i));
    }

    packet.pid_name = "Unknown PID";
    packet.type = PKT_TYPE_UNKNOWN;

    // Match PID string mappings to hex values
    switch(packet.pid_val) {
        case 0x87: packet.pid_name = "OUT";   packet.type = PKT_TYPE_TOKEN; break;
        case 0x96: packet.pid_name = "IN";    packet.type = PKT_TYPE_TOKEN; break;
        case 0xA5: packet.pid_name = "SOF";   packet.type = PKT_TYPE_TOKEN; break;
        case 0xB4: packet.pid_name = "SETUP"; packet.type = PKT_TYPE_TOKEN; break;
        case 0xC3: packet.pid_name = "DATA0"; packet.type = PKT_TYPE_DATA; break;
        case 0xD2: packet.pid_name = "DATA1"; packet.type = PKT_TYPE_DATA; break;
        case 0xE1: packet.pid_name = "DATA2"; packet.type = PKT_TYPE_DATA; break;
        case 0xF0: packet.pid_name = "MDATA"; packet.type = PKT_TYPE_DATA; break;
        case 0x4B: packet.pid_name = "ACK";   packet.type = PKT_TYPE_HANDSHAKE; break;
        case 0x5A: packet.pid_name = "NAK";   packet.type = PKT_TYPE_HANDSHAKE; break;
        case 0x78: packet.pid_name = "STALL"; packet.type = PKT_TYPE_HANDSHAKE; break;
        case 0x69: packet.pid_name = "NYET";  packet.type = PKT_TYPE_HANDSHAKE; break;
        case 0x3C: packet.pid_name = "PRE";   packet.type = PKT_TYPE_UNKNOWN; break;
        case 0x1E: packet.pid_name = "SPLIT"; packet.type = PKT_TYPE_UNKNOWN; break;
        case 0x2D: packet.pid_name = "PING";  packet.type = PKT_TYPE_UNKNOWN; break;
        case 0x0F: packet.pid_name = "Reserved"; packet.type = PKT_TYPE_UNKNOWN; break;
    }

    if (packet.type == PKT_TYPE_TOKEN) {
        // Token packets need 19 bits: 8 PID + 7 ADDR + 4 ENDP + 5 CRC5
        if (len < 19) {
            packet.is_valid_length = 0;
        } else {
            packet.addr = extract_bits_lsb_first(pkt, 8, 7);
            packet.endp = extract_bits_lsb_first(pkt, 15, 4);
            packet.crc5_received = extract_bits_lsb_first(pkt, 19, 5);
            packet.crc5_calc = calculate_crc5(pkt, 8, 11);
            packet.crc5_valid = (packet.crc5_received == packet.crc5_calc);
        }
    }
    else if (packet.type == PKT_TYPE_DATA) {
        // Data packets need at least 24 bits: 8 PID + 0 data + 16 CRC16
        if (len < 24) { 
            packet.is_valid_length = 0;
        } else {
            packet.data_bits_len = len - 8 - 16;
            packet.data_bits = &pkt[8]; // Point directly to the start of the data payload
            packet.crc16_received = extract_bits_lsb_first(pkt, len - 16, 16);
            packet.crc16_calc = calculate_crc16(pkt, 8, packet.data_bits_len);
            packet.crc16_valid = (packet.crc16_received == packet.crc16_calc);
        }
    }

    return packet;
}

void fprint_usb_packet(FILE* stream, const UsbPacket* packet, int print_binary) {
    if (!packet->is_valid_length) {
        fprintf(stream, "\n--- USB Packet Analysis: Error - Too short (%d bits) ---\n", packet->total_length);
        return;
    }

    fprintf(stream, "\n========= USB PACKET ANALYSIS =========\n");
    fprintf(stream, "PID:   %s\n", packet->pid_name);

    if (packet->type == PKT_TYPE_TOKEN) {
        fprintf(stream, "ADDR:  %d\n", packet->addr);
        fprintf(stream, "ENDP:  %d\n", packet->endp);
        fprintf(stream, "CRC5:  0x%02X (%s)\n", packet->crc5_received, (packet->crc5_valid) ? "VALID" : "INVALID");
    }
    else if (packet->type == PKT_TYPE_DATA) {
        fprintf(stream, "DATA:  ");
        if (print_binary) {
            for(int i = 0; i < packet->data_bits_len; i++) {
                fprintf(stream, "%d", packet->data_bits[i]);
            }
        } else {
            for(int i = 0; i < packet->data_bits_len; i += 8) {
                int bits_to_read = (packet->data_bits_len - i >= 8) ? 8 : (packet->data_bits_len - i);
                uint8_t byte_val = extract_bits_lsb_first(packet->data_bits, i, bits_to_read);
                fprintf(stream, "%02X ", byte_val);
            }
        }
        fprintf(stream, "\nCRC16: 0x%04X (%s)\n", packet->crc16_received, (packet->crc16_valid) ? "VALID" : "INVALID");
    }
    else if (packet->type == PKT_TYPE_HANDSHAKE) {
        // Handshakes only contain a PID, nothing extra to print
    }
    else {
        fprintf(stream, "Payload: Unknown or Raw bits\n");
    }
    fprintf(stream, "=======================================\n\n");
}

void print_usb_packet(const UsbPacket* packet, int print_binary) {
    fprint_usb_packet(stdout, packet, print_binary);
}