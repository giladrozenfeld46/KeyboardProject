import pyvisa
import time

# --- Configuration Constants ---
# Replace with your actual oscilloscope VISA address
SCOPE_ADDRESS = 'USB0::0x2A8D::0x179B::CN59012116::0::INSTR'

# Define the voltage thresholds (in Volts)
HIGH_THRESHOLD_V = 2.0
LOW_THRESHOLD_V = 0.5


def main():
    rm = pyvisa.ResourceManager('@py')

    try:
        # Open connection to the oscilloscope
        scope = rm.open_resource(SCOPE_ADDRESS)
        scope.timeout = 10000
        print(f"Successfully connected to: {scope.query('*IDN?')}")

        # Ensure the oscilloscope is running continuously so we can take live measurements
        scope.write(':RUN')
        time.sleep(1)

        print(f"Waiting for Channel 1 to go above {HIGH_THRESHOLD_V}V...")

        # 1. Wait for CH1 to go HIGH
        while True:
            # Query the current DC voltage of Channel 1
            ch1_voltage_str = scope.query(':MEASure:VDC? CHANnel1')
            try:
                ch1_voltage = float(ch1_voltage_str)
            except ValueError:
                ch1_voltage = 0.0

            if ch1_voltage > HIGH_THRESHOLD_V:
                print(f"Trigger! CH1 voltage is {ch1_voltage}V. Starting capture loop.")
                break

            # Small delay to avoid overwhelming the USB bus
            time.sleep(0.1)

        # 2. Capture and check loop
        frame_counter = 0
        while True:
            print(f"Capturing frame {frame_counter}...")

            # Stop continuous run and digitize exactly one full screen of data for both channels
            scope.write(':DIGitize CHANnel1,CHANnel2')

            # --- DATA DOWNLOADING SECTION ---
            # In a full script, you would download the raw waveform data here using:
            # scope.write(':WAVeform:SOURce CHANnel1')
            # ch1_data = scope.query(':WAVeform:DATA?')
            # Save ch1_data to file...
            # --------------------------------

            frame_counter += 1

            # Resume running to allow the oscilloscope to calculate new measurements
            scope.write(':RUN')
            time.sleep(0.5)

            # Measure both channels to check the stop condition
            ch1_val = float(scope.query(':MEASure:VDC? CHANnel1'))
            ch2_val = float(scope.query(':MEASure:VDC? CHANnel2'))

            print(f"Current levels -> CH1: {ch1_val:.2f}V, CH2: {ch2_val:.2f}V")

            # 3. Check if both channels are LOW
            if ch1_val < LOW_THRESHOLD_V and ch2_val < LOW_THRESHOLD_V:
                print("Both channels are below the low threshold. Stopping recording.")
                break

    except Exception as e:
        print(f"Communication error: {e}")

    finally:
        # Clean up and close connection
        if 'scope' in locals():
            scope.write(':RUN')
            scope.close()
            print("Connection closed.")


if __name__ == "__main__":
    main()
    
