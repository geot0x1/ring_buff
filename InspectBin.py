import struct

SECTOR_SIZE = 65536
NUM_SECTORS = 4

def inspect_bin(filename):
    with open(filename, 'rb') as f:
        data = f.read()
    
    print(f"Inspecting {filename}:")
    for i in range(NUM_SECTORS):
        addr = i * SECTOR_SIZE
        if addr + 16 > len(data):
            break
        header = data[addr:addr+16]
        # magic (4), seq (4), data_start (2), status (1)
        magic, seq, data_start, status = struct.unpack("<IIHB", header[:11])
        print(f"Sector {i} at 0x{addr:X}:")
        print(f"  Magic:      0x{magic:08X}")
        print(f"  Seq:        {seq}")
        print(f"  Data Start: {data_start}")
        print(f"  Status:     0x{status:02X}")

if __name__ == '__main__':
    inspect_bin('simulation_images/spanning_record.bin')
