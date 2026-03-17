def calc_crc8(data):
    crc = 0xFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc

spanning_data = b"Spanning Record Content Starts S0 Ends S1"
crc = calc_crc8(spanning_data)
print(f"CRC: 0x{crc:02X}")
if crc == 0x5A:
    print("MATCHES FCB_RECORD_MAGIC!")
