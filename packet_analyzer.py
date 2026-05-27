

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

    # Return the inverted remainder
    return bin(res ^ 0x1f)

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

    # Return the inverted remainder
    return bin(res ^ 0xffff)

def analyze_packet(packet):
    # Convert the first 8 bits (integers) into a single continuous string
    byte_str = "".join(map(str, packet[:8]))

    # Safely get the PID from the dictionary.
    # Returns "Unknown PID" if the sequence is corrupted or missing.
    pid = binary_to_text_8bit.get(byte_str, "Unknown PID")

    packet_info = {}

    if pid == "IN" or pid == "OUT":
        packet_info = analyze_IN_OUT_packets(packet)
    elif pid == "DATA0" or pid == "DATA1" or pid == "DATA2" or pid == "MDATA":
        packet_info = analyze_DATA_packets(packet)
    elif pid == "ACK" or pid == "NAK":
        packet_info = analyze_handshake_packets(packet)

    return packet_info


def analyze_IN_OUT_packets(pkt):
    bit_str = "".join(map(str, pkt))
    bin_pid = bit_str[:8]
    addr = int(bit_str[8:15][::-1], 2) # reverse for msb
    endp = bit_str[15:19][::-1] # reverse for msb
    crc5 = bit_str[19:][::-1] # reverse for msb
    pid = binary_to_text_8bit.get(bin_pid, "Unknown PID")

    #crc_result = crc5usb(bit_str[8:-5][::-1])
    #print(crc_result)

    return {"PID" : pid, "ADDR" : addr, "ENDP" : endp, "CRC5" : crc5}

def analyze_DATA_packets(pkt):
    bit_str = "".join(map(str, pkt))
    bin_pid = bit_str[:8]
    data = bit_str[8:-16][::-1] # reverse for msb
    crc16 = bit_str[-16:][::-1] # reverse for msb
    # crc_result = crc16usb(data)
    # print(crc_result)
    pid = binary_to_text_8bit.get(bin_pid, "Unknown PID")

    return {"PID" : pid, "DATA": data, "CRC16" : crc16}

def analyze_handshake_packets(pkt):
    bit_str = "".join(map(str, pkt))
    bin_pid = bit_str[:8]
    pid = binary_to_text_8bit.get(bin_pid, "Unknown PID")
    return {"PID" : pid}



