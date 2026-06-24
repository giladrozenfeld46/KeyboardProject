# 8-bit sequence dictionary mapping the full byte array
binary_to_text_8bit = {
    "10000111": "OUT",
    "10010110": "IN",
    "10100101": "SOF",
    "10110100": "SETUP",
    "11000011": "DATA0",
    "11010010": "DATA1",
    "11100001": "DATA2",
    "11110000": "MDATA",
    "01001011": "ACK",
    "01011010": "NAK",
    "01111000": "STALL",
    "01101001": "NYET",
    "00111100": "PRE",
    "00011110": "SPLIT",
    "00101101": "PING",
    "00001111": "Reserved"
}


def crc_remainder(input_bitstring, polynomial_bitstring, initial_filler='0'):
    """Calculate the CRC remainder of a string of bits using a chosen polynomial.
    initial_filler should be '1' or '0'.
    """
    polynomial_bitstring = polynomial_bitstring.lstrip("0")
    len_input = len(input_bitstring)
    initial_padding = (len(polynomial_bitstring) - 1) * initial_filler
    input_padded_array = list(input_bitstring + initial_padding)
    while "1" in input_padded_array[:len_input]:
        cur_shift = input_padded_array.index("1")
        for i in range(len(polynomial_bitstring)):
            input_padded_array[cur_shift + i] \
                = str(int(polynomial_bitstring[i] != input_padded_array[cur_shift + i]))
    return "".join(input_padded_array)[len_input:]


def crc5usb(bit_string):
    # Convert the string of bits into an integer
    input_val = int(bit_string, 2)

    # Initialize the remainder
    res = 0x1f

    # Determine the number of bits to process based on string length
    num_bits = len(bit_string)

    for _ in range(num_bits):
        # Extract the lowest bit of the XOR result
        b = (input_val ^ res) & 1

        # Shift the input right by 1
        input_val >>= 1

        if b:
            # Shift right and apply the polynomial XOR (0x14 is 10100 in binary)
            res = (res >> 1) ^ 0x14
        else:
            # Just shift right
            res >>= 1

    # Return the inverted remainder as a clean 5-bit string (e.g., '10110')
    return format(res ^ 0x1f, '05b')


def crc16usb(bit_string):
    # Convert the string of bits into an integer
    input_val = int(bit_string, 2)

    # Initialize the remainder
    res = 0xffff

    # Determine the number of bits to process based on string length
    num_bits = len(bit_string)

    for _ in range(num_bits):
        # Extract the lowest bit of the XOR result
        b = (input_val ^ res) & 1

        # Shift the input right by 1
        input_val >>= 1

        if b:
            # Shift right and apply the polynomial XOR (0x14 is 10100 in binary)
            res = (res >> 1) ^ 0xA001
        else:
            # Just shift right
            res >>= 1

    # Return the inverted remainder as a clean 16-bit string
    return format(res ^ 0xffff, '016b')


def analyze_packet(packet, as_binary=False):
    # Convert the first 8 bits (integers) into a single continuous string
    byte_str_pid = "".join(map(str, packet[:8]))

    # Safely get the PID from the dictionary.
    # Returns "Unknown PID" if the sequence is corrupted or missing.
    pid = binary_to_text_8bit.get(byte_str_pid, "Unknown PID")

    packet_len = len(packet)
    packet_info = {}

    if pid == "IN" or pid == "OUT" or pid == "SETUP":
        packet_info = analyze_IN_OUT_packets(packet)
    elif pid in ["DATA0", "DATA1", "DATA2", "MDATA"]:
        # Pass the as_binary flag to the DATA packet analyzer
        packet_info = analyze_DATA_packets(packet, as_binary)
    elif pid == "ACK" or pid == "NAK":
        packet_info = analyze_handshake_packets(packet)
    else:
        packet_info = {"PID": pid, "payload": packet[8:]}

    return packet_info, packet_len


def analyze_IN_OUT_packets(pkt):
    bit_str = "".join(map(str, pkt))
    bin_pid = bit_str[:8]
    addr = int(bit_str[8:15][::-1], 2)  # reverse for msb
    endp = bit_str[15:19][::-1]  # reverse for msb
    crc5 = bit_str[19:][::-1]  # reverse for msb
    pid = binary_to_text_8bit.get(bin_pid, "Unknown PID")

    # Calculate CRC5 using the address and endpoint payload
    calc_crc5 = crc5usb(bit_str[8:-5][::-1])

    # Compare calculated CRC with received CRC
    is_crc_valid = (calc_crc5 == crc5)

    return {
        "PID": pid,
        "ADDR": addr,
        "ENDP": endp,
        "CRC5": crc5,
        "CRC_VALID": is_crc_valid
    }


def analyze_DATA_packets(pkt, as_binary=False):
    bit_str = "".join(map(str, pkt))
    bin_pid = bit_str[:8]
    data_bin = bit_str[8:-16][::-1]  # reverse for msb
    crc16 = bit_str[-16:][::-1]  # reverse for msb
    pid = binary_to_text_8bit.get(bin_pid, "Unknown PID")

    # Calculate CRC16 using the data payload (must remain in binary for calculation)
    if data_bin != '':
        calc_crc16 = crc16usb(data_bin)

        # Compare calculated CRC with received CRC
        is_crc_valid = (calc_crc16 == crc16)
    else:
        is_crc_valid = False

    # Determine whether to output binary or hex based on the as_binary flag
    if as_binary:
        data_out = data_bin
    else:
        if data_bin:
            # Calculate how many hex characters are needed (4 bits per hex character)
            hex_length = len(data_bin) // 4
            # Format as uppercase hex, padded with leading zeros if necessary
            data_out = f"{int(data_bin, 2):0{hex_length}X}"
        else:
            # Handle zero-length payloads safely
            data_out = ""

    return {
        "PID": pid,
        "DATA": data_out,
        "CRC16": crc16,
        "CRC_VALID": is_crc_valid
    }


def analyze_handshake_packets(pkt):
    bit_str = "".join(map(str, pkt))
    bin_pid = bit_str[:8]
    pid = binary_to_text_8bit.get(bin_pid, "Unknown PID")
    return {"PID": pid}