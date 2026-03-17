import struct
import os

SECTOR_SIZE = 65536
NUM_SECTORS = 4
FLASH_SIZE = SECTOR_SIZE * NUM_SECTORS

FCB_SECTOR_MAGIC = 0x0FCBF1F0
FCB_RECORD_MAGIC = 0x5A
FCB_SECTOR_HDR_SIZE = 16
FCB_RECORD_HDR_SIZE = 4

FCB_SECTOR_STATUS_VALID = 0xFF
FCB_SECTOR_STATUS_CONSUMED = 0x00

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

def create_sector_header(sequence, data_start=16, status=FCB_SECTOR_STATUS_VALID):
    return struct.pack("<IIHBBBBBB", 
                       FCB_SECTOR_MAGIC, sequence, data_start, status, 
                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF)

def create_record(data, status=0xFF):
    length = len(data)
    header = struct.pack("<B H B", FCB_RECORD_MAGIC, length, status)
    crc = calc_crc8(data)
    return header + data + struct.pack("B", crc)

def save_image(flash, filename):
    os.makedirs("simulation_images", exist_ok=True)
    filepath = os.path.join("simulation_images", filename)
    with open(filepath, "wb") as f:
        f.write(flash)
    print(f"Generated {filepath} ({len(flash)} bytes)")

def gen_cold_start():
    flash = bytearray([0xFF] * FLASH_SIZE)
    save_image(flash, "cold_start.bin")

def gen_single_sector_normal():
    flash = bytearray([0xFF] * FLASH_SIZE)
    flash[0:16] = create_sector_header(sequence=1)
    r1 = create_record(b"Valid Record 1")
    r2 = create_record(b"Valid Record 2")
    flash[16:16+len(r1)] = r1
    offset = 16 + len(r1)
    flash[offset:offset+len(r2)] = r2
    save_image(flash, "single_sector_normal.bin")

def gen_multi_sector_chain():
    flash = bytearray([0xFF] * FLASH_SIZE)
    # Sector 0
    flash[0:16] = create_sector_header(sequence=1)
    r1 = create_record(b"Record 1 In S0")
    flash[16:16+len(r1)] = r1
    
    # Sector 1
    addr1 = SECTOR_SIZE
    flash[addr1:addr1+16] = create_sector_header(sequence=2)
    r2 = create_record(b"Record 2 In S1")
    flash[addr1+16:addr1+16+len(r2)] = r2

    # Sector 2
    addr2 = SECTOR_SIZE * 2
    flash[addr2:addr2+16] = create_sector_header(sequence=3)
    r3 = create_record(b"Record 3 In S2")
    flash[addr2+16:addr2+16+len(r3)] = r3

    save_image(flash, "multi_sector_chain.bin")

def gen_wrap_around():
    flash = bytearray([0xFF] * FLASH_SIZE)
    # Sector 0: Seq 0xFFFFFFFF
    flash[0:16] = create_sector_header(sequence=0xFFFFFFFF)
    flash[16:16+len(create_record(b"S0"))] = create_record(b"S0")

    # Sector 1: Seq 0x0
    addr1 = SECTOR_SIZE
    flash[addr1:addr1+16] = create_sector_header(sequence=0x0)
    flash[addr1+16:addr1+16+len(create_record(b"S1"))] = create_record(b"S1")

    # Sector 2: Seq 0x1
    addr2 = SECTOR_SIZE * 2
    flash[addr2:addr2+16] = create_sector_header(sequence=0x1)
    flash[addr2+16:addr2+16+len(create_record(b"S2"))] = create_record(b"S2")

    save_image(flash, "wrap_around_chain.bin")

def gen_interrupted_erase():
    flash = bytearray([0xFF] * FLASH_SIZE)
    flash[0:16] = create_sector_header(sequence=10)
    addr1 = SECTOR_SIZE
    corrupt_hdr = struct.pack("<IIHBBBBBB", 0xDEADBEEF, 11, 16, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF)
    flash[addr1:addr1+16] = corrupt_hdr
    flash[addr1+16:addr1+32] = b"\xAA" * 16
    save_image(flash, "interrupted_erase.bin")

def gen_corrupted_scavenge():
    flash = bytearray([0xFF] * FLASH_SIZE)
    flash[0:16] = create_sector_header(sequence=1)
    r1 = create_record(b"Correct 1")
    flash[16:16+len(r1)] = r1
    
    offset = 16 + len(r1)
    # Corrupt record: wrong CRC or length
    flash[offset:offset+10] = b"\x5A\x0A\x00\xFF\xCC\xCC\xCC\xCC\xCC\xCC" 
    offset += 10
    
    r2 = create_record(b"Correct 2 follows corruption")
    flash[offset:offset+len(r2)] = r2
    save_image(flash, "corrupt_scavenge.bin")

def gen_spanning_record():
    flash = bytearray([0xFF] * FLASH_SIZE)
    flash[0:16] = create_sector_header(sequence=1)
    flash[SECTOR_SIZE:SECTOR_SIZE+16] = create_sector_header(sequence=2, data_start=25)

    s0_end = SECTOR_SIZE - 4
    spanning_data = b"Spanning Record Content Starts S0 Ends S1"
    length = len(spanning_data)
    header = struct.pack("<B H B", FCB_RECORD_MAGIC, length, 0xFF)
    
    flash[s0_end:s0_end+4] = header
    flash[SECTOR_SIZE+16:SECTOR_SIZE+16+length] = spanning_data
    crc = calc_crc8(spanning_data)
    flash[SECTOR_SIZE+16+length] = crc

    save_image(flash, "spanning_record.bin")

if __name__ == '__main__':
    gen_cold_start()
    gen_single_sector_normal()
    gen_multi_sector_chain()
    gen_wrap_around()
    gen_interrupted_erase()
    gen_corrupted_scavenge()
    gen_spanning_record()
