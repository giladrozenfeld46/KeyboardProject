from symbol_analyzer import *
from packet_analyzer import *





def extract_data_packets(symbols):
    # Define the exact start sequence to look for
    start_sequence = ['K', 'J', 'K', 'J', 'K', 'J', 'K', 'K']
    seq_len = len(start_sequence)

    packets = []
    i = 0
    n = len(symbols)

    while i < n:
        # Check if the current position matches the start sequence
        if i + seq_len <= n and symbols[i:i + seq_len] == start_sequence:
            current_packet = []

            # Move the index past the start sequence
            i += seq_len

            # The reference symbol is the last one from the start sequence ('K')
            prev_symbol = 'K'

            #counting ones for bit stuffing
            ones_counter = 0

            # Start decoding the payload
            while i < n:
                curr_symbol = symbols[i]

                # Stop decoding if we hit the end marker
                if curr_symbol == 'SE0':
                    packets.append(current_packet)
                    i += 1  # Move past the end marker
                    break

                # Decode transitions into bits
                if curr_symbol in ('K', 'J'):
                    if ones_counter == 6:
                        ones_counter = 0
                    elif curr_symbol == prev_symbol:
                        current_packet.append(1)  # No change means 1
                        ones_counter += 1
                    else:
                        current_packet.append(0)  # Change means 0
                        ones_counter = 0

                    # Update reference for the next iteration
                    prev_symbol = curr_symbol

                i += 1

            # Handle edge case where the sequence ends without an SE0
            if i == n and current_packet:
                packets.append(current_packet)

        else:
            # If no start sequence is found, move forward by one symbol
            i += 1

    return packets
"""

file_path = 'clean.csv'
plot_channel_bits(file_path)
plot_channel_bits(file_path, channel_to_plot='1 (VOLT)')
decoded_stream = process_csv_to_symbols(file_path)
print(f"Total symbols decoded: {len(decoded_stream)}")
print(f"First 20 symbols: {decoded_stream}")
packets = extract_data_packets(decoded_stream)
print(packets)
"""


file_path = 'data\\clean.csv'

# Process everything directly from CSV to symbols
decoded_stream = process_csv_to_symbols(file_path)

# Extract the individual data packets based on the start sequence
packets = extract_data_packets(decoded_stream)

for idx, pkt in enumerate(packets):
    pkt_info = analyze_packet(pkt)
    pid = pkt_info["PID"]
    pkt_info.pop("PID")
    if len(pkt_info) == 0:
        pkt_info = ""
    print(f"Packet {idx + 1} ({len(pkt)} bits): {pid} {pkt_info}")