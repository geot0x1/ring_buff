# NOR Flash Circular Buffer (FCB)

A robust, power-loss safe circular buffer implementation for NOR flash memory. This project provides a sequence-aware logging system that handles hardware-level constraints like sector erasure and bit-clearing state transitions.

## 🚀 Quick Start

### Installation
Integrate the following directories into your project:
- `/fcb`: Core logistics engine.
- `/crc32`: Integrity verification module.
- `/flash_mem`: Flash abstraction layer (or simulation).

### Basic Usage
```c
#include "fcb/fcb.h"

Fcb my_fcb = {
    .first_sector = 0,
    .last_sector = 15,
    .sector_size = 64 * 1024
};

// 1. Mount the buffer (recovers head/tail pointers)
fcb_mount(&my_fcb);

// 2. Append data
const char* msg = "Log entry 1";
fcb_append(&my_fcb, msg, strlen(msg));
```

## 🏗️ Architecture

The system is built on three main layers:
1.  **Logistics Layer (`fcb.c`)**: Manages head/tail pointers across sectors.
2.  **Integrity Layer (`crc32.c`)**: Ensures data and header consistency.
3.  **Flash Layer (`flash_mem.c`)**: Simulates/Abstracts raw flash IO.

### Features
- **Zero Dynamic Allocation**: 100% stack/static memory usage.
- **Power-Loss Recovery**: Automatic recovery of buffer state on mount.
- **Data Integrity**: CRC32 checks for every sector and every item.
- **Circular Design**: Seamlessly wraps across sectors, erasing the oldest data as needed.

## 🛠️ Testing

The project includes a comprehensive test suite in `/tests` and `main.c`.

### Running Tests
To run the included simulation test suite:
1.  Ensure you have a C compiler (e.g., GCC).
2.  Compile all source files: `gcc main.c fcb/fcb.c flash_mem/flash_mem.c crc32/crc32.c tests/test_fcb.c -o fcb_tests`
3.  Execute the binary: `./fcb_tests`

## 📄 Documentation

Detailed documentation artifacts are generated in the `.gemini/antigravity/brain/` directory:
- [Comprehensive Technical Manual](file:///C:/Users/George/.gemini/antigravity/brain/560ec004-8e37-47f8-b5bf-81e47c01e2c9/comprehensive_documentation.md)
- [C-Code Architect Summary](file:///C:/Users/George/.gemini/antigravity/brain/560ec004-8e37-47f8-b5bf-81e47c01e2c9/program_documentation.md)

---
*Maintained by C-Code Architect*
