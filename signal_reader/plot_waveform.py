import numpy as np
import matplotlib.pyplot as plt
import os

csv_file = 'waveform.csv'

if not os.path.exists(csv_file):
    print(f"Error: {csv_file} not found.")
    exit(1)

# Load data using numpy
data = np.loadtxt(csv_file, delimiter=',', skiprows=1)
time_axis = data[:, 0]
gpio8 = data[:, 1]
gpio9 = data[:, 2]

# Create the plot
fig, ax = plt.subplots(figsize=(10, 5))

# Plot GPIO 8 and 9 with a small vertical offset for clarity
ax.step(time_axis, gpio8, label='GPIO 8 (Trigger)', color='blue', where='post')
ax.step(time_axis, gpio9 + 1.5, label='GPIO 9', color='red', where='post')

# Add a vertical red line at index 0 to show the exact trigger moment
ax.axvline(x=0, color='red', linestyle='--', alpha=0.5, label='Trigger Point')

ax.set_title('SMI Logic Analyzer Capture')
ax.set_xlabel('Samples relative to trigger')
ax.set_ylabel('Logic Level')
ax.set_yticks([0, 1, 1.5, 2.5])
ax.set_yticklabels(['Low', 'High', 'Low', 'High'])
ax.grid(True, linestyle=':', alpha=0.6)
ax.legend(loc='upper right')

plt.tight_layout()
plt.show()