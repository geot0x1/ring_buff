import sys

def dump_hex(filename, start, count):
    with open(filename, 'rb') as f:
        f.seek(start)
        data = f.read(count)
        print(f"Offset {start}:")
        # print in hex rows of 16 bytes
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            hex_str = ' '.join(f'{b:02X}' for b in chunk)
            ascii_str = ''.join(chr(b) if 32 <= b <= 126 else '.' for b in chunk)
            print(f"{start + i:08X}  {hex_str:<47}  |{ascii_str}|")

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: python DumpHex.py <filename> <offset> <count>")
        sys.exit(1)
    dump_hex(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))
