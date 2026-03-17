import sys

def search_magic(filename, magic_byte):
    with open(filename, 'rb') as f:
        data = f.read()
        print(f"Searching for {magic_byte:02X}...")
        count = 0
        for i, b in enumerate(data):
            if b == magic_byte:
                print(f"Found at offset {i} (0x{i:X})")
                count += 1
                if count > 10:
                    print("Too many matches, stopping.")
                    break

if __name__ == '__main__':
    search_magic('fcb_test_image.bin', 0x5A)
