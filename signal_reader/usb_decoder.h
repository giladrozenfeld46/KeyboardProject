#ifndef USB_DECODER_H
#define USB_DECODER_H

#include <stdint.h>

/**
 * Analyzes a stream of NRZI decoded USB bits and prints the packet information.
 * * @param pkt Array of bits (0s and 1s)
 * @param len Number of bits in the array
 * @param print_binary Set to 1 to print data payload as binary, 0 for Hex
 */
void analyze_usb_packet(const uint8_t* pkt, int len, int print_binary);

#endif // USB_DECODER_H