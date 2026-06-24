import os
from symbol_analyzer import *
from packet_analyzer import *
from channels_to_bits import *
import time

import os


def analyze_single_file(file_path, as_binary=False):
    """
    Processes a single CSV file and returns the analyzed packets.
    Returns a list of dictionaries containing time, size, and packet data.
    """
    analyzed_results = []

    # 1. Extract the raw packets (symbols) from the CSV
    symbol_packets = extract_packets_from_csv(file_path)

    # 2. Decode the symbols into actual data bits
    final_bit_packets = decode_symbols_to_bits(symbol_packets)

    # 3. Analyze each packet and store the results in a structured format
    for pkt_time, payload_bits in final_bit_packets.items():
        start_time = time.perf_counter()
        packet_data, packet_len = analyze_packet(payload_bits, as_binary=as_binary)
        end_time = time.perf_counter()
        time_taken = end_time - start_time

        analyzed_results.append({
            'time_taken': time_taken,
            'time': pkt_time,
            'size': packet_len,
            'data': packet_data
        })

    return analyzed_results


def process_directory(input_folder_name):
    """
    Processes all CSV files in the given directory.
    Creates a new directory named '<input_folder_name>_results' and saves
    the analysis output as text files with matching names.
    """
    # Define the name of the output directory
    output_folder_name = f"{input_folder_name}_results"

    # Create the output directory if it doesn't already exist
    os.makedirs(output_folder_name, exist_ok=True)

    # Check if the input directory exists
    if not os.path.exists(input_folder_name):
        print(f"Error: The directory '{input_folder_name}' does not exist.")
        return

    # Iterate over all files in the input directory
    for filename in os.listdir(input_folder_name):
        # Process only CSV files
        if filename.endswith(".csv"):
            input_file_path = os.path.join(input_folder_name, filename)

            # Extract the base name without the .csv extension to create the .txt filename
            base_name = os.path.splitext(filename)[0]
            output_file_path = os.path.join(output_folder_name, f"{base_name}.txt")

            print(f"Processing '{filename}'...")

            # Open the new text file for writing the results
            with open(output_file_path, 'w', encoding='utf-8') as out_file:
                try:
                    # Execute the analysis logic via the single-file function
                    file_results = analyze_single_file(input_file_path)

                    # Iterate through the returned results and write them to the file
                    for result in file_results:
                        out_file.write(f"time: {result['time']:.6f}s, size: {result['size']}, Data: {result['data']}\n")

                    print(f"Successfully saved results to '{output_file_path}'.")

                except Exception as e:
                    # If any error occurs during processing, write it to the file and console
                    error_msg = f"Failed to process {filename}. Error: {e}"
                    print(error_msg)
                    out_file.write(error_msg + "\n")

# ==========================================
# Main execution
# ==========================================
if __name__ == "__main__":
    # Specify the name of your data folder here
    folder_to_analyze = "numbers"

    # Run the batch processing
    process_directory(folder_to_analyze)
    caps_results = analyze_single_file("caps lock.csv", as_binary=True)
    for result in caps_results:
        print(f"process_time: {result['time_taken']:.6f}s, process_per_symbol: {result['time_taken']/result['size']:.8f}s")
        print(f"process_time: {result['time_taken']:.6f}s, time: {result['time']:.6f}s, size: {result['size']}, Data: {result['data']}\n")
