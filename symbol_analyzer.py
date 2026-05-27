from channels_to_bits import *
import matplotlib

matplotlib.use('TkAgg')


def decode_differential_symbols(ch1_bits, ch2_bits):
    """
    Takes two synchronized binary sequences and decodes them into symbols.
    """
    symbols = []

    # Iterate through both sequences simultaneously.
    # Since the new modular logic truncates both channels at the exact
    # same starting timestamp, the bits are perfectly synchronized.
    for b1, b2 in zip(ch1_bits, ch2_bits):

        # Apply the specific logic mapping provided
        if b1 == 0 and b2 == 0:
            symbols.append("SE0")

        elif b1 == 1 and b2 == 0:
            symbols.append("K")

        elif b1 == 0 and b2 == 1:
            symbols.append("J")

        elif b1 == 1 and b2 == 1:
            symbols.append("SE1")

    return symbols


def process_csv_to_symbols(file_path, ch1_name='1 (VOLT)', ch2_name='2 (VOLT)'):
    """
    End-to-end pipeline: extracts binary data from the CSV and converts
    it into a single stream of differential symbols.
    """
    # 1. Get the binary sequences using the modularized function
    # Note: csv_to_binary_sequence automatically handles the leading zeros truncation
    binary_data = csv_to_binary_sequence(file_path, plotting=False)

    # Ensure data was extracted successfully and contains both required channels
    if not binary_data or ch1_name not in binary_data or ch2_name not in binary_data:
        print("Error: Could not find the required channels or data is empty.")
        return []

    ch1_bits = binary_data[ch1_name]
    ch2_bits = binary_data[ch2_name]

    # 2. Convert the two binary streams into a single symbol stream
    final_symbols = decode_differential_symbols(ch1_bits, ch2_bits)

    return final_symbols

# ==========================================
# Example usage of the complete pipeline:
# ==========================================
# file_path = 'clean.csv'
#
# # Process everything directly from CSV to symbols
# decoded_stream = process_csv_to_symbols(file_path)
#
# # Extract the individual data packets based on the start sequence
# packets = extract_data_packets(decoded_stream)
#
# for idx, pkt in enumerate(packets):
#     print(f"Packet {idx + 1} ({len(pkt)} bits): {pkt}")