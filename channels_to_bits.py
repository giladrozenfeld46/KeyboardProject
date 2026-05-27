import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib

matplotlib.use('TkAgg')


def load_and_preprocess_csv(file_path):
    """
    Reads the CSV, cleans missing data, and truncates the timeline
    to start exactly at the first rising edge of the first channel.
    """
    df = pd.read_csv(file_path, skiprows=1)

    time_col = [c for c in df.columns if 'Time' in str(c)][0]
    channel_cols = [c for c in df.columns if 'VOLT' in str(c)]

    df = df.dropna(subset=[time_col] + channel_cols).reset_index(drop=True)

    # Isolate first channel to find the starting point
    first_ch = channel_cols[0]
    volt_ch1 = df[first_ch].values

    threshold_ch1 = (np.max(volt_ch1) + np.min(volt_ch1)) / 2.0
    binary_ch1 = (volt_ch1 > threshold_ch1).astype(int)

    # Find the first rising edge
    rising_edges = np.where((binary_ch1[:-1] == 0) & (binary_ch1[1:] == 1))[0]

    if len(rising_edges) > 0:
        start_index = rising_edges[0] + 1
    elif binary_ch1[0] == 1:
        start_index = 0
    else:
        # Signal never goes high
        return None, None

    # Truncate arrays to skip leading zeros
    time_data = df[time_col].values[start_index:]
    channel_data_dict = {ch: df[ch].values[start_index:] for ch in channel_cols}

    return time_data, channel_data_dict


def estimate_bit_duration(time_data, binary_signal):
    """
    Analyzes the signal to find where it transitions between 0 and 1,
    and estimates the exact time duration of a single bit.
    """
    # Find all indices where the signal flips its state
    transitions = np.where(np.diff(binary_signal) != 0)[0]

    if len(transitions) < 2:
        # Not enough data to determine a bit duration
        return None, None

    transition_times = time_data[transitions]

    # Calculate the time differences (dt) between consecutive transitions
    dt = np.diff(transition_times)

    # Filter out hardware noise (e.g., tiny spikes shorter than 10ns)
    valid_dt = dt[dt > 1e-8]
    if len(valid_dt) == 0:
        return None, None

    # Find the shortest legitimate interval, which represents a single bit.
    # We take the median of intervals close to the minimum to avoid outlier distortion.
    min_dt = np.min(valid_dt)
    base_bit_duration = np.median(valid_dt[valid_dt < 1.5 * min_dt])

    return base_bit_duration, transitions


def build_voltage_intervals(time_data, binary_signal, transitions):
    """
    Maps the continuous binary signal into discrete, stable intervals.
    Returns a list of tuples in the format: (start_time, end_time, logic_state).
    """
    transition_times = time_data[transitions]
    intervals = []

    # 1. Handle the leading period (from start of recording to first transition)
    if transition_times[0] > time_data[0]:
        intervals.append((time_data[0], transition_times[0], binary_signal[0]))

    # 2. Handle all stable periods between the first and last transition
    for i in range(len(transitions) - 1):
        start_t = transition_times[i]
        end_t = transition_times[i + 1]

        # The state is sampled exactly one index after the transition occurred
        state = binary_signal[transitions[i] + 1]
        intervals.append((start_t, end_t, state))

    # 3. Handle the trailing period (from last transition to end of recording)
    if time_data[-1] > transition_times[-1]:
        intervals.append((transition_times[-1], time_data[-1], binary_signal[-1]))

    return intervals


def compute_bit_coordinates(time_data, volt_data, intervals, base_bit_duration):
    """
    Slices the stable intervals into individual bits based on the base duration.
    Calculates exact coordinates for visualization (boundaries and text centers).
    """
    boundary_times, boundary_volts = [], []
    bit_centers_x, bit_centers_y = [], []
    bits = []

    for start_t, end_t, state in intervals:
        duration = end_t - start_t

        # Determine how many whole bits fit into this specific interval
        num_bits = int(np.round(duration / base_bit_duration))

        if num_bits > 0:
            # Localize bit duration: divide the exact interval length by num_bits.
            # This perfectly aligns bits inside the interval and prevents visual drift!
            local_bit_dur = duration / num_bits

            for k in range(num_bits):
                # --- A: Calculate the visual boundary (start of the current bit) ---
                b_time = start_t + k * local_bit_dur
                b_idx = min(np.searchsorted(time_data, b_time), len(time_data) - 1)
                boundary_times.append(time_data[b_idx])
                boundary_volts.append(volt_data[b_idx])

                # --- B: Calculate the exact center point (for drawing the 0/1 text) ---
                c_time = start_t + (k + 0.5) * local_bit_dur
                c_idx = min(np.searchsorted(time_data, c_time), len(time_data) - 1)
                bit_centers_x.append(time_data[c_idx])
                bit_centers_y.append(volt_data[c_idx])

                # --- C: Save the actual logical value of the bit ---
                bits.append(state)

    # Append the absolute last boundary to close the plot seamlessly
    last_idx = len(time_data) - 1
    boundary_times.append(time_data[last_idx])
    boundary_volts.append(volt_data[last_idx])

    return bits, boundary_times, boundary_volts, bit_centers_x, bit_centers_y


def extract_channel_features(time_data, volt_data):
    """
    Main orchestrator function.
    Processes a raw analog channel into logical bits and plotting coordinates.
    """
    # Calculate a dynamic threshold to distinguish high (1) and low (0) states
    threshold = (np.max(volt_data) + np.min(volt_data)) / 2.0
    binary_signal = (volt_data > threshold).astype(int)

    # Step 1: Find signal transitions and calculate the length of a single bit
    base_bit_duration, transitions = estimate_bit_duration(time_data, binary_signal)

    if base_bit_duration is None:
        # Abort if the signal is too noisy or completely flat
        return None

    # Step 2: Organize the signal into discrete chunks of stable voltage
    intervals = build_voltage_intervals(time_data, binary_signal, transitions)

    # Step 3: Break down the intervals into individual bits and generate graph dots
    bits, b_times, b_volts, c_x, c_y = compute_bit_coordinates(
        time_data, volt_data, intervals, base_bit_duration
    )

    # Return a consolidated dictionary containing all processed information
    return {
        'threshold': threshold,
        'bits': bits,
        'boundary_times': b_times,
        'boundary_volts': b_volts,
        'bit_centers_x': c_x,
        'bit_centers_y': c_y
    }

def csv_to_binary_sequence(file_path, plotting=False):
    """
    Main function 1: Returns a dictionary of pure binary sequences for all channels.
    """
    if plotting:
        plot_channel_bits(file_path, channel_to_plot='1 (VOLT)')
        plot_channel_bits(file_path, channel_to_plot='2 (VOLT)')
    time_data, channel_data_dict = load_and_preprocess_csv(file_path)

    if time_data is None:
        return {}

    results = {}
    for ch_name, volt_data in channel_data_dict.items():
        features = extract_channel_features(time_data, volt_data)

        if features:
            results[ch_name] = features['bits']
        else:
            results[ch_name] = []

    return results


def plot_channel_bits(file_path, channel_to_plot='1 (VOLT)'):
    """
    Main function 2: Visually plots the analog signal, boundaries, and bit values.
    """
    time_data, channel_data_dict = load_and_preprocess_csv(file_path)

    if time_data is None or channel_to_plot not in channel_data_dict:
        print(f"Error loading data or channel {channel_to_plot} not found.")
        return

    volt_data = channel_data_dict[channel_to_plot]
    features = extract_channel_features(time_data, volt_data)

    if not features:
        print(f"Not enough valid data to plot {channel_to_plot}.")
        return

    # Render the plot using the features dictionary
    plt.figure(figsize=(16, 7))
    plt.plot(time_data, volt_data, label='Analog Signal', color='blue', alpha=0.6)

    plt.scatter(features['boundary_times'], features['boundary_volts'],
                color='red', zorder=5, label='Bit Boundaries', s=30)

    plt.axhline(features['threshold'], color='green', linestyle='--',
                label='Threshold', alpha=0.5)

    y_range = np.max(volt_data) - np.min(volt_data)
    text_offset = y_range * 0.05

    for x, y, val in zip(features['bit_centers_x'], features['bit_centers_y'], features['bits']):
        y_pos = y + text_offset if val == 1 else y - text_offset
        plt.text(x, y_pos, str(val), fontsize=11, fontweight='bold',
                 color='darkorange', ha='center', va='center', zorder=10)

    plt.title(f'Smart Signal Sync - {channel_to_plot}')
    plt.xlabel('Time (s)')
    plt.ylabel('Voltage (V)')
    plt.legend(loc='upper right')
    plt.grid(True)
    plt.tight_layout()
    plt.show()

