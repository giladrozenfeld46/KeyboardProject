import sys
import struct
import numpy as np
import matplotlib.pyplot as plt

def load_capture_data(filepath: str):
    """Reads binary sample data containing two synchronized channels."""
    with open(filepath, "rb") as f:
        # Read total sample count (uint32)
        raw_count = f.read(4)
        if len(raw_count) < 4:
            raise ValueError("Invalid capture file: Missing header")
        
        num_samples = struct.unpack("I", raw_count)[0]
        
        # Read Channel A and Channel B buffers
        ch_a_bytes = f.read(num_samples * 2)
        ch_b_bytes = f.read(num_samples * 2)
        
        ch_a = np.frombuffer(ch_a_bytes, dtype=np.uint16)
        ch_b = np.frombuffer(ch_b_bytes, dtype=np.uint16)
        
    return ch_a, ch_b, num_samples

def plot_dual_channels(ch_a: np.ndarray, ch_b: np.ndarray, num_samples: int):
    """Renders time-domain traces and signal analysis for both channels."""
    sample_indices = np.arange(num_samples)

    fig, axes = plt.subplots(3, 1, figsize=(12, 8), sharex=True)
    fig.suptitle(f"Dual-Channel High-Speed Capture ({num_samples:,} Samples)", fontsize=14, fontweight='bold')

    # Channel A Plot
    axes[0].plot(sample_indices, ch_a, color='#1f77b4', label='Channel 1 (Data+ / V1)', linewidth=1.2)
    axes[0].set_ylabel('ADC Counts')
    axes[0].grid(True, linestyle='--', alpha=0.6)
    axes[0].legend(loc='upper right')

    # Channel B Plot
    axes[1].plot(sample_indices, ch_b, color='#ff7f0e', label='Channel 2 (Data- / V2)', linewidth=1.2)
    axes[1].set_ylabel('ADC Counts')
    axes[1].grid(True, linestyle='--', alpha=0.6)
    axes[1].legend(loc='upper right')

    # Differential / Difference Signal (Ch1 - Ch2)
    diff_signal = ch_a.astype(np.int32) - ch_b.astype(np.int32)
    axes[2].plot(sample_indices, diff_signal, color='#2ca02c', label='Differential (Ch1 - Ch2)', linewidth=1.0)
    axes[2].set_xlabel('Sample Index')
    axes[2].set_ylabel('Diff Counts')
    axes[2].grid(True, linestyle='--', alpha=0.6)
    axes[2].legend(loc='upper right')

    plt.tight_layout()
    plt.show()

if __name__ == "__main__":
    file_path = sys.argv[1] if len(sys.argv) > 1 else "capture_output.bin"
    try:
        data_a, data_b, count = load_capture_data(file_path)
        print(f"[Viewer] Successfully loaded {count} samples from '{file_path}'. Rendering plot...")
        plot_dual_channels(data_a, data_b, count)
    except Exception as e:
        print(f"[Viewer] Error: {e}")