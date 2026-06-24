import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib

matplotlib.use('TkAgg')


# ===================================================================
# HELPER CLASS: Allows attaching timestamps to a standard list object
# ===================================================================
class SymbolList(list):
    def __init__(self, iterable, timestamps=None):
        super().__init__(iterable)
        self.timestamps = timestamps or []


def decode_differential_symbols(ch1_bits, ch2_bits):
    """
    Takes two synchronized binary sequences and decodes them into symbols.
    """
    symbols = []
    for b1, b2 in zip(ch1_bits, ch2_bits):
        if b1 == 0 and b2 == 0:
            symbols.append("SE0")
        elif b1 == 1 and b2 == 0:
            symbols.append("K")
        elif b1 == 0 and b2 == 1:
            symbols.append("J")
        elif b1 == 1 and b2 == 1:
            symbols.append("SE1")
    return symbols


def convert_packets_to_symbols(packets_dict, ch1_name, ch2_name):
    """
    Converts raw channel bits into decoded symbols.
    Returns a SymbolList which acts like a normal list but holds corrected timestamps.
    """
    symbol_packets = {}

    for start_time, channels_data in packets_dict.items():
        if ch1_name in channels_data and ch2_name in channels_data:
            ch1_bits = channels_data[ch1_name]
            ch2_bits = channels_data[ch2_name]

            symbols_list = decode_differential_symbols(ch1_bits, ch2_bits)

            # Wrap the standard list in our custom class to attach the timestamps
            symbol_packets[start_time] = SymbolList(
                symbols_list,
                timestamps=channels_data.get('timestamps', [])
            )
        else:
            print(f"Warning: Missing channel data for packet starting at {start_time}")

    return symbol_packets


def extract_packets_from_csv(file_path):
    """
    Extracts packets using true Edge-Driven Clock Recovery.
    Snaps to physical edges to completely eliminate clock drift.
    """
    bit_rate = 1500000.0
    ideal_bit_duration = 1.0 / bit_rate

    df = pd.read_csv(file_path, header=0, skiprows=[1])
    time_col = df.columns[0]
    ch1_col = df.columns[1]
    ch2_col = df.columns[2]
    channel_cols = [ch1_col, ch2_col]

    df = df.dropna(subset=[time_col] + channel_cols).reset_index(drop=True)

    time_data = df[time_col].values
    volt_ch1 = df[ch1_col].values
    volt_ch2 = df[ch2_col].values

    thresh_ch1 = (np.max(volt_ch1) + np.min(volt_ch1)) / 2.0
    thresh_ch2 = (np.max(volt_ch2) + np.min(volt_ch2)) / 2.0

    bin_ch1 = (volt_ch1 > thresh_ch1).astype(int)
    bin_ch2 = (volt_ch2 > thresh_ch2).astype(int)

    packets = {}
    in_packet = False

    start_time = 0.0
    last_sync_time = 0.0
    current_ch1 = 0
    current_ch2 = 0

    ch1_bits = []
    ch2_bits = []
    timestamps = []

    for i in range(1, len(time_data)):
        if not in_packet:
            # Detect packet start (K state: CH1 High, CH2 Low)
            if bin_ch1[i] == 1 and bin_ch2[i] == 0:
                if bin_ch1[i - 1] != 1 or bin_ch2[i - 1] != 0:
                    in_packet = True
                    start_time = time_data[i]
                    last_sync_time = time_data[i]
                    current_ch1 = 1
                    current_ch2 = 0
                    ch1_bits = []
                    ch2_bits = []
                    timestamps = []
        else:
            elapsed = time_data[i] - last_sync_time

            # 1. Edge Detected: State has changed
            if bin_ch1[i] != current_ch1 or bin_ch2[i] != current_ch2:
                # Determine how many bits fit in this elapsed time using the ideal duration
                num_bits = int(round(elapsed / ideal_bit_duration))

                if num_bits > 0:
                    # Calculate the actual physical duration of a bit in this specific chunk
                    # This ensures symbols are perfectly centered even if the device's clock is slightly off
                    actual_bit_duration = elapsed / num_bits

                    for k in range(num_bits):
                        ch1_bits.append(current_ch1)
                        ch2_bits.append(current_ch2)
                        timestamps.append(last_sync_time + (k + 0.5) * actual_bit_duration)

                # CRITICAL FIX: Snap the sync clock to the ACTUAL physical time of the edge.
                # This resets the accumulated drift to absolutely zero at every physical transition.
                last_sync_time = time_data[i]

                # Update current state
                current_ch1 = bin_ch1[i]
                current_ch2 = bin_ch2[i]

            # 2. Check for SE0 (End of Packet)
            elif current_ch1 == 0 and current_ch2 == 0 and elapsed >= (ideal_bit_duration * 0.5):
                num_bits = int(round(elapsed / ideal_bit_duration))
                if num_bits == 0: num_bits = 1

                for k in range(num_bits):
                    ch1_bits.append(0)
                    ch2_bits.append(0)
                    timestamps.append(last_sync_time + (k + 0.5) * ideal_bit_duration)

                in_packet = False
                packets[start_time] = {
                    ch1_col: ch1_bits,
                    ch2_col: ch2_bits,
                    'timestamps': timestamps
                }

    if in_packet:
        elapsed = time_data[-1] - last_sync_time
        num_bits = int(round(elapsed / ideal_bit_duration))
        if num_bits > 0:
            actual_bit_duration = elapsed / num_bits
            for k in range(num_bits):
                ch1_bits.append(current_ch1)
                ch2_bits.append(current_ch2)
                timestamps.append(last_sync_time + (k + 0.5) * actual_bit_duration)

        packets[start_time] = {
            ch1_col: ch1_bits,
            ch2_col: ch2_bits,
            'timestamps': timestamps
        }

    symbol_packets = convert_packets_to_symbols(packets, ch1_col, ch2_col)
    return symbol_packets


def plot_packet_extraction_optimized(file_path):
    """
    Visualizes packet extraction using the perfectly aligned physical timestamps.
    """
    all_packets = extract_packets_from_csv(file_path)

    df = pd.read_csv(file_path, header=0, skiprows=[1])
    time_col = df.columns[0]
    ch1_col = df.columns[1]
    ch2_col = df.columns[2]
    channel_cols = [ch1_col, ch2_col]

    df = df.dropna(subset=[time_col] + channel_cols).reset_index(drop=True)

    time_data = df[time_col].values
    volt_ch1 = df[ch1_col].values
    volt_ch2 = df[ch2_col].values

    fig, axes = plt.subplots(nrows=2, ncols=1, figsize=(16, 12), sharex=True)
    ax1, ax2 = axes[0], axes[1]

    ax1.plot(time_data, volt_ch1, color='blue', alpha=0.5, label=f'Channel {ch1_col}')
    ax2.plot(time_data, volt_ch2, color='purple', alpha=0.5, label=f'Channel {ch2_col}')

    for start_time, symbols in all_packets.items():
        packet_len = len(symbols)
        timestamps = getattr(symbols, 'timestamps', [])

        # Guard clause if timestamps array is missing or empty
        if not timestamps:
            continue

        end_time = timestamps[-1] + (1.0 / 1500000.0 / 2.0)
        packet_center_time = start_time + ((end_time - start_time) / 2.0)

        # Draw boundaries & shading
        ax1.axvline(start_time, color='lime', linestyle='-', linewidth=2)
        ax1.axvline(end_time, color='red', linestyle='-', linewidth=2)
        ax2.axvline(start_time, color='lime', linestyle='-', linewidth=2)
        ax2.axvline(end_time, color='red', linestyle='-', linewidth=2)

        ax1.axvspan(start_time, end_time, color='yellow', alpha=0.15)
        ax2.axvspan(start_time, end_time, color='yellow', alpha=0.15)

        # Length Badge
        ax1.text(packet_center_time, 0.95, f'Len: {packet_len}',
                 transform=ax1.get_xaxis_transform(), ha='center', va='top',
                 fontsize=10, color='black', fontweight='bold',
                 bbox=dict(facecolor='white', alpha=0.8, edgecolor='black', boxstyle='round,pad=0.3'),
                 zorder=15)

        # Annotate symbols using their exact physical timestamps
        for symbol, bit_time in zip(symbols, timestamps):
            ax1.text(bit_time, np.mean(volt_ch1), symbol,
                     ha='center', va='center', fontsize=9, color='darkorange', fontweight='bold')
            ax2.text(bit_time, np.mean(volt_ch2), symbol,
                     ha='center', va='center', fontsize=9, color='darkorange', fontweight='bold')

    ax1.set_title("Packet Extraction Visualization (Perfectly Aligned)")
    ax1.set_ylabel("Voltage (V)")
    ax2.set_xlabel("Time (s)")
    ax2.set_ylabel("Voltage (V)")
    ax1.grid(True)
    ax2.grid(True)

    plt.tight_layout()
    plt.show()

plot_packet_extraction_optimized("space pressed.csv")