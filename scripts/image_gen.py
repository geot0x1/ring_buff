import struct

# Constants from fcb.h
SECTOR_SIZE = 65536  # Example size
NUM_SECTORS = 4
FLASH_SIZE = SECTOR_SIZE * NUM_SECTORS

FCB_SECTOR_MAGIC = 0x0FCBF1F0
FCB_RECORD_MAGIC = 0x5A
FCB_SECTOR_HDR_SIZE = 16
FCB_RECORD_HDR_SIZE = 4

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

def create_sector_header(sequence, data_start=16):
    # magic (4), seq (4), data_start (2), status (1), reserved (5)
    return struct.pack("<IIHBBBBBB", 
                       FCB_SECTOR_MAGIC, sequence, data_start, 0xFF, 
                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF)

def create_record(data):
    length = len(data)
    # magic (1), length (2), status (1)
    header = struct.pack("<B H B", FCB_RECORD_MAGIC, length, 0xFF)
    crc = calc_crc8(data)
    return header + data + struct.pack("B", crc)

# 1. Initialize empty flash (all 0xFF)
flash = bytearray([0xFF] * FLASH_SIZE)

def write_to_flash(addr, data):
    flash[addr:addr+len(data)] = data

# 2. Setup Sector 0 (Normal valid entries)
write_to_flash(0, create_sector_header(sequence=1))
r1 = create_record(b"Valid Entry 1")
write_to_flash(16, r1)

# 3. Inject Garbage (The Fragmented Scenario)
# We place record 1, then 50 bytes of garbage, then record 2
garbage_offset = 16 + len(r1)
garbage = b"\xEE" * 50
write_to_flash(garbage_offset, garbage)

r2 = create_record(b"Valid Entry 2 found after garbage")
write_to_flash(garbage_offset + len(garbage), r2)

# 4. Setup Sector 1 (Record Spanning Scenario)
# Place a header at the very end of Sector 0 that spills into Sector 1
write_to_flash(SECTOR_SIZE, create_sector_header(sequence=2, data_start=25)) # data_start offset 25

# Create a spanning record: Header at end of S0, Data starts in S1
spanning_data = b"This record starts in Sector 0 and ends in Sector 1"
# Header is 4 bytes. Let's put it at the last 4 bytes of Sector 0.
s0_end = SECTOR_SIZE - 4
header = struct.pack("<B H B", FCB_RECORD_MAGIC, len(spanning_data), 0xFF)
write_to_flash(s0_end, header)

# Data for spanning record starts at Sector 1 Header + 16
# Wait, based on your fcb_verify_record_at, it expects data immediately after header.
# If header is at S0_end, data starts at S1 offset 16.
write_to_flash(SECTOR_SIZE + 16, spanning_data)
crc = calc_crc8(spanning_data)
write_to_flash(SECTOR_SIZE + 16 + len(spanning_data), struct.pack("B", crc))

# 5. Save to file
with open("fcb_test_image.bin", "wb") as f:
    f.write(flash)

print(f"Generated fcb_test_image.bin ({FLASH_SIZE} bytes)")