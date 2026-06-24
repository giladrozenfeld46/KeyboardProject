from channels_to_bits import *
import matplotlib

matplotlib.use('TkAgg')


def decode_symbols_to_bits(symbol_packets):
    """
    Converts a dictionary of symbols into a dictionary of payload bits.
    Verifies and removes the sync sequence, applies NRZI decoding,
    and performs bit destuffing (removing inserted 0 after six consecutive 1s).
    """
    # Define the exact required start sequence
    sync_sequence = ['K', 'J', 'K', 'J', 'K', 'J', 'K', 'K']
    decoded_packets = {}

    for start_time, symbols in symbol_packets.items():
        # 1. Check if the packet is long enough
        if len(symbols) < 8:
            raise ValueError(f"Packet at {start_time:.6f}s is too short to contain a sync sequence.")

        # 2. Verify the exact sync sequence at the beginning
        if symbols[:8] != sync_sequence:
            raise ValueError(f"Invalid or missing sync sequence in packet at {start_time:.6f}s.")

        bits = []

        # 3. NRZI Decoding with Bit Destuffing
        # The last symbol of the sync sequence ('K') serves as the reference
        # state for decoding the very first payload bit.
        prev_symbol = 'K'

        # Counter to track consecutive '1's for bit destuffing
        consecutive_ones = 0

        # Start iterating immediately after the 8-symbol sync sequence
        for current_symbol in symbols[8:]:

            # SE0 signifies the End of Packet (EOP)
            if current_symbol == 'SE0':
                break

            # Decode valid differential states into bits
            if current_symbol in ('K', 'J'):
                if current_symbol == prev_symbol:
                    decoded_bit = 1  # No state change means 1
                else:
                    decoded_bit = 0  # State change means 0

                # Update the reference symbol for the next iteration
                prev_symbol = current_symbol

                # Bit Destuffing Logic
                # If we just saw six '1's, the current bit is the stuffed bit.
                if consecutive_ones == 6:
                    # Reset the counter and skip appending this bit to the payload
                    consecutive_ones = 0
                    continue

                # Append the valid bit to the payload
                bits.append(decoded_bit)

                # Update the consecutive ones counter based on the current valid bit
                if decoded_bit == 1:
                    consecutive_ones += 1
                else:
                    consecutive_ones = 0

        decoded_packets[start_time] = bits

    return decoded_packets