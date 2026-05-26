# FCB (Flash Circular Buffer) - Comprehensive Technical Documentation

**Objective:** Power-fail-safe circular FIFO buffer on SPI NOR flash memory for embedded systems. This document provides complete byte-level details, memory layout, and implementation specifics for AI agent parsing.

---

## Table of Contents

1. [Overview & Fundamentals](#overview--fundamentals)
2. [On-Flash Memory Layout](#on-flash-memory-layout)
3. [In-RAM Structure (Fcb)](#in-ram-structure-fcb)
4. [Configuration Structure (FcbConfig)](#configuration-structure-fbcconfig)
5. [Three-Pointer FIFO Architecture](#three-pointer-fifo-architecture)
6. [Sector Structure - On Flash](#sector-structure---on-flash)
7. [Record Structure - On Flash](#record-structure---on-flash)
8. [Core Operations](#core-operations)
9. [Recovery Mechanisms](#recovery-mechanisms)
10. [Thread Safety](#thread-safety)
11. [Error Codes](#error-codes)
12. [Constants & Limits](#constants--limits)
13. [Examples](#examples)

---

## Overview & Fundamentals

### Purpose
- Stores telemetry/event data on SPI NOR flash using a circular buffer
- Survives arbitrary power losses during read/write/erase operations
- Provides FIFO semantics with batch deletion capabilities
- Thread-safe with optional mutex support

### Key Design Principles

#### 1. Power-Fail Safety via NOR Flash Properties
- **Bit Transition:** NOR flash bits only transition 1→0 via programming; erase restores all bits to 1
- **Atomic Operations:** Single-byte writes (0xFF→0x00) are guaranteed atomic, even during power loss
- **Safe Ordering:** Operations ordered to leave recoverable state after ANY power loss
  - Sector headers written FIRST (if power lost here, sector treated as erased)
  - Record headers written BEFORE data (if power lost during data write, CRC catches corruption)
  - CRC covers data only (partial writes detectable via CRC mismatch)

#### 2. Recovery Strategy
- **Full Sector Scan:** During `fcb_init()`, all sectors scanned to rebuild pointers
- **Monotonic Sequences:** Sectors numbered with forever-increasing sequence IDs for chronological ordering
- **CRC Validation:** Every record includes CRC-8 for data integrity; corrupted records trigger truncation
- **Graceful Truncation:** Power loss mid-write detected by invalid CRC → truncation stops recovery walk

### Target Use Cases
- Telemetry logging (sent to server later)
- Event recording (GPS traces, sensor readings, fault logs)
- Persistent data buffering during connectivity loss
- Crash dump/black box storage

---

## On-Flash Memory Layout

### Overall Structure

```
┌─────────────────────────────────────────────────────────────┐
│  Flash Memory (Typically SPI NOR)                           │
├─────────────────────────────────────────────────────────────┤
│  [Sector 0 (size S)]                                        │
│  ├─ Sector Header (16 bytes)                                │
│  ├─ Record 1 (header + data + CRC)                          │
│  ├─ Record 2 (header + data + CRC)                          │
│  └─ ... free space ...                                      │
├─────────────────────────────────────────────────────────────┤
│  [Sector 1 (size S)]                                        │
│  ├─ Sector Header (16 bytes)                                │
│  ├─ Records...                                              │
├─────────────────────────────────────────────────────────────┤
│  ...                                                        │
├─────────────────────────────────────────────────────────────┤
│  [Sector N (size S)]                                        │
│  ├─ Sector Header (16 bytes)                                │
│  ├─ Records...                                              │
└─────────────────────────────────────────────────────────────┘
```

### Addressing Formula

```
Absolute Flash Address = start_addr + (sector_index * sector_size) + offset_in_sector
```

**Example:** For sector 2, offset 50 bytes, with start_addr=0x00100000, sector_size=65536:
```
Address = 0x00100000 + (2 * 65536) + 50 = 0x00120032
```

---

## In-RAM Structure (Fcb)

### Definition: `typedef struct Fcb` (48 bytes)

```c
typedef struct {
    FcbConfig config;              // Full config copy (~56 bytes)
    
    uint32_t delete_sector;        // Sector index of delete pointer (0..num_sectors-1)
    uint32_t delete_offset;        // Byte offset within delete sector (after sector header)
    
    uint32_t read_sector;          // Sector index of read pointer (0..num_sectors-1)
    uint32_t read_offset;          // Byte offset within read sector (after sector header)
    
    uint32_t write_sector;         // Sector index where next record will be written
    uint32_t write_offset;         // Byte offset within write sector (next free byte)
    
    uint32_t next_sequence;        // Next monotonic sequence number to assign
    
    uint32_t magic;                // Internal canary: 0xFCB0FCB0 (set after successful init)
    
    bool     is_mounted;           // Indicates FCB initialized and ready for use
} Fcb;
```

### Field Semantics

| Field | Type | Size | Description |
|-------|------|------|-------------|
| `config` | `FcbConfig` | Variable | Static config: flash callbacks, sector geometry, mutex context |
| `delete_sector` | `uint32_t` | 4 | Index (0-based) of sector containing first unconsumed record |
| `delete_offset` | `uint32_t` | 4 | Byte offset from sector start (includes 16-byte header) |
| `read_sector` | `uint32_t` | 4 | Index of sector containing next unread record |
| `read_offset` | `uint32_t` | 4 | Byte offset from sector start |
| `write_sector` | `uint32_t` | 4 | Index of sector where next write will occur |
| `write_offset` | `uint32_t` | 4 | Byte offset in write sector (next free byte) |
| `next_sequence` | `uint32_t` | 4 | Next sequence number to assign to new sectors |
| `magic` | `uint32_t` | 4 | Canary value for validity check (0xFCB0FCB0) |
| `is_mounted` | `bool` | 1 | True if FCB initialized successfully |

### Pointer Semantics

**Circular ordering invariant (conceptual):**
```
delete_ptr → read_ptr → write_ptr (circular, wraps at num_sectors)
```

**In terms of consumed/unread/unwritten records:**
- **Consumed records:** From `delete_ptr` back to start of oldest unconsumed (moving backwards in circular order)
- **Unread records:** From `read_ptr` to `delete_ptr` (moving forwards)
- **Unwritten space:** From `write_ptr` to `read_ptr` (moving forwards)

---

## Configuration Structure (FcbConfig)

### Definition: `typedef struct FcbConfig` (~56 bytes)

```c
typedef struct {
    uint32_t start_addr;              // Absolute flash address of first sector
    uint32_t num_sectors;             // Number of sectors, 1..64
    uint32_t sector_size;             // Size of each sector in bytes (e.g., 65536)
    
    void *flash_ctx;                  // Opaque context passed to flash callbacks
    
    // Flash driver callbacks (function pointers)
    int (*flash_read)(void *ctx, uint32_t addr, uint8_t *buf, size_t len);
    int (*flash_program)(void *ctx, uint32_t addr, const uint8_t *data, size_t len);
    int (*flash_erase_sector)(void *ctx, uint32_t addr);
    
    // Thread safety callbacks (optional, may be NULL)
    void (*lock)(void *mutex_ctx);
    void (*unlock)(void *mutex_ctx);
    
    void *mutex_ctx;                  // Opaque context passed to lock/unlock
} FcbConfig;
```

### Field Semantics

| Field | Type | Valid Range | Description |
|-------|------|-------------|-------------|
| `start_addr` | `uint32_t` | Any | Absolute flash address of first byte of sector 0 |
| `num_sectors` | `uint32_t` | 1–64 | Total number of sectors in the buffer |
| `sector_size` | `uint32_t` | ≥32 | Bytes per sector (typical: 4096, 65536) |
| `flash_ctx` | `void*` | Any | Driver-specific context, forwarded to callbacks |
| `flash_read` | Function ptr | Non-NULL | Return 0 on success, non-zero on error |
| `flash_write` | Function ptr | Non-NULL | Program up to 256 bytes per call; return 0 on success |
| `flash_erase` | Function ptr | Non-NULL | Erase sector containing address; return 0 on success |
| `lock` | Function ptr | NULL or valid | If non-NULL, called before critical sections |
| `unlock` | Function ptr | NULL or valid | If non-NULL, called after critical sections |
| `mutex_ctx` | `void*` | Any | Driver-specific mutex context, ignored if lock/unlock are NULL |

### Validation Rules (enforced in `fcb_init_validate_config`)

1. `cfg` must not be NULL
2. `flash_read`, `flash_write`, `flash_erase` must not be NULL
3. `num_sectors` must be 1–64 (inclusive)
4. `sector_size` must be ≥ 32 bytes (enough for header + one minimal record)

---

## Three-Pointer FIFO Architecture

### Overview

FCB uses three sector/offset pairs to manage a circular buffer:

```
Physical Memory:
┌──────────────┬──────────────┬──────────────┬──────────────┐
│  Sector 0    │  Sector 1    │  Sector 2    │  Sector 3    │
│              │              │              │              │
└──────────────┴──────────────┴──────────────┴──────────────┘
     │               │              ▲             │
  delete_ptr     read_ptr      write_ptr      ...
  (oldest)    (next unread)  (next free)

Circular order (conceptual):
delete_ptr → read_ptr → write_ptr (→ delete_ptr, wraps)
```

### Pointer Definitions

| Pointer | Records Ahead | Behavior | Typical State After... |
|---------|----------------|----------|------------------------|
| `delete_ptr` | Unread records from here to `read_ptr` | Points to first unconsumed record; batch-advanced by `fcb_delete()` | `fcb_delete()`: equals `read_ptr` |
| `read_ptr` | New records from here to `write_ptr` | Points to next unread record; advanced by `fcb_read()` after each read | `fcb_read()`: points to next record |
| `write_ptr` | Free space from here to `delete_ptr` (wraps) | Points to next free byte; advanced by `fcb_write()` after each write | `fcb_write()`: points past new record |

### Memory Regions

In a circular buffer with pointers at positions D, R, W:

```
1. CONSUMED (D→R, going backwards in circular order):
   - Records marked with status=0x00 (consumed flag)
   - No longer readable; space will be reclaimed via fcb_trim()

2. UNREAD (R→W, going forwards):
   - Active records (status=0xFF) not yet read
   - Read via fcb_read(), which advances R

3. UNWRITTEN (W→D, going forwards, wraps at sector boundary):
   - All-0xFF erased space
   - Available for new writes via fcb_write()

Example with 4 sectors, each 256 bytes:
D=Sector1:100,  R=Sector2:50,  W=Sector3:200

CONSUMED:     Sector1:100 → Sector2:50
  (moving backwards through Sector1, wrapping to Sector3, Sector2, Sector1:100)
UNREAD:       Sector2:50 → Sector3:200
UNWRITTEN:    Sector3:200 → Sector1:100 (wraps: Sector3:200→256, Sector0, Sector1:0→100)
```

### State Transitions

```
Initial state (after fcb_init on empty buffer):
  D=R=W, magic set, is_mounted=true

After fcb_write():
  W advances past new record
  D, R unchanged (no new data to read yet)

After fcb_read():
  R advances to next record
  D, W unchanged

After fcb_delete():
  D advances to match R
  R, W unchanged

After fcb_trim():
  Oldest sector erased
  D, R advanced if they pointed into erased sector
  W unchanged
```

---

## Sector Structure - On Flash

### Sector Header (16 bytes, always at offset 0)

Located at absolute address: `start_addr + (sector_num * sector_size)`

**Memory Layout (little-endian, native byte order):**

```
Offset  Size  Field Name      Type       Hex Value (Valid)   Description
────────────────────────────────────────────────────────────────────
0       4     magic           uint32_t   0x0FCBF1F0          Identifies valid sector header
4       4     sequence        uint32_t   0x00000001+         Monotonic ID (increases forever)
8       2     data_start      uint16_t   0x0010              Offset to first record (usually FCB_SECTOR_HDR_SIZE=16)
10      1     status          uint8_t    0xFF or 0x00        0xFF=valid, 0x00=consumed (for trim operations)
11      5     reserved        uint8_t[]  0xFF,0xFF...        Always 0xFF (padding to 16 bytes)
```

### Interpretation

```c
struct FcbSectorHdr {
    uint32_t magic;      // 0x0FCBF1F0 → "0FCB F1F0" (FCB FIFO backwards, FCB FIFO forwards)
    uint32_t sequence;   // Sector ID: older sectors have smaller values (considering wrap-around)
    uint16_t data_start; // Start offset of first valid record; usually 16 bytes (after header)
    uint8_t  status;     // 0xFF=valid (not yet trimmed), 0x00=consumed (safe to erase)
    uint8_t  reserved[5];// All 0xFF in valid state
};
```

### Sequence Number Ordering

Sequences are monotonically increasing: 1, 2, 3, ... They wrap at 2^32 but can be compared using signed distance math:

```c
// seq_a is newer than seq_b if:
(int32_t)(seq_a - seq_b) > 0
```

This correctly handles wrap-around:
- seq_a=2, seq_b=1 → (2-1)=1 > 0 ✓ (seq_a newer)
- seq_a=1, seq_b=0xFFFFFFFF → (1-0xFFFFFFFF)=2 > 0 ✓ (seq_a newer, wraps)
- seq_a=0xFFFFFFFE, seq_b=0xFFFFFFFF → (0xFFFFFFFE-0xFFFFFFFF)=-1 < 0 ✗ (seq_b newer)

### Example Sector Header (hexdump)

```
Offset 00: F0 F1 CB 0F 03 00 00 00 10 00 FF FF FF FF FF FF
           ^^^^^^^^^^^^^ ^^^^^^^^^^^^^ ^^ ^^ ^^^^^^^^^^^^^^
           magic=0x0FCBF1F0  sequence=3  ds status reserved(5)

Interpretation:
  magic: 0x0FCBF1F0 (valid)
  sequence: 3 (3rd sector in chronological order)
  data_start: 0x0010 (16 bytes, standard)
  status: 0xFF (valid, not consumed)
  reserved: all 0xFF
```

---

## Record Structure - On Flash

### Record Header (4 bytes)

Located at offset `record_header_offset` within a sector (minimum 16, after sector header).

**Absolute address:** `start_addr + (sector_num * sector_size) + record_header_offset`

**Memory Layout (little-endian, native byte order):**

```
Offset  Size  Field Name      Type       Hex Value (Valid)   Description
────────────────────────────────────────────────────────────────────
0       1     magic           uint8_t    0x5A                Identifies valid record header
1       2     length          uint16_t   1–1024              Payload size in bytes
3       1     status          uint8_t    0xFF or 0x00        0xFF=active, 0x00=consumed
```

### Interpretation

```c
struct FcbRecordHdr {
    uint8_t  magic;   // 0x5A (single-byte marker)
    uint16_t length;  // Payload length: 1–1024 bytes (enforced at write-time)
    uint8_t  status;  // 0xFF=active (unread or read but not deleted), 0x00=consumed (marked deleted)
};
```

### Complete Record Layout (On Flash)

After the 4-byte header comes the data payload and a 1-byte CRC:

```
┌─────────────┬──────────────────┬─────┐
│   Header    │   Data Payload   │ CRC │
│  (4 bytes)  │   (1–1024 bytes) │ (1) │
└─────────────┴──────────────────┴─────┘
Offset:  0         4                  length+4

Total record size = 4 + length + 1 = 5 + length bytes
```

### CRC-8 Calculation

- **Algorithm:** Custom CRC-8 with polynomial 0x31
- **Input:** Data payload only (NOT header, NOT status byte)
- **Seed:** 0xFF (starting CRC value)
- **Method:** Bit-serial, MSB-first

```c
uint8_t fcb_calc_crc8(uint8_t start_crc, const uint8_t *data, uint32_t len) {
    uint8_t crc = start_crc;  // Start with 0xFF
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if ((crc & 0x80) != 0) {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}
```

### Example Record (10-byte payload)

```
Sector 1, offset 16 (first record)

Hexdump:
BA 0A 00 FF  48 65 6C 6C  6F 57 6F 72  6C 64 7E
^^ ^^^^ ^^   ^^^^^^^^^^^^^ (10 bytes)   ^^
│  │    │    Data                       CRC
│  │    status=0xFF (active)
│  length=0x000A (10 bytes)
magic=0x5A

Interpretation:
  Header:  5A 0A 00 FF
    magic: 0x5A ✓
    length: 10 bytes ✓
    status: 0xFF (active) ✓
  Payload: 48 65... (10 bytes)
  CRC: 0x7E (calculated from payload)
```

### Record Spanning Across Sectors

If a record's payload cannot fit in the remaining space of the current sector, it **spans** into the next sector:

```
Sector boundary at offset 256 (64 bytes remaining after header at offset 200):

Sector N (offset 200):
┌─────────────────────────────────────────────────┬──────────┐
│ Record Header (4)  │  Data Part 1 (60 bytes)   │ Free(0)  │
└─────────────────────────────────────────────────┴──────────┘
                      
                      (64 bytes left = 4 header + 60 data)
                      Full payload = 100 bytes

Sector N+1 (offset 16, after sector header):
┌─────────────────────────────────────────────────┬──────┐
│  Data Part 2 (40 bytes)  │  CRC (1 byte)   │ X... │
└─────────────────────────────────────────────────┴──────┘

Data split: 60 bytes in Sector N, 40 bytes in Sector N+1
CRC written after data spill in Sector N+1
```

---

## Core Operations

### 1. fcb_init() - Mount and Recovery

**Signature:**
```c
int fcb_init(Fcb *fcb, const FcbConfig *cfg);
```

**Return codes:** `FCB_OK`, `FCB_INVALID_ARG`, `FCB_ERR_FLASH`, `FCB_CORRUPTED`

#### Algorithm (High-level)

```
1. Validate configuration
2. Scan all sector headers to find oldest and newest valid sectors
3. Determine chronological order via sequence numbers
4. Walk records from oldest→newest sector
5. Recover pointers (delete_ptr, read_ptr, write_ptr)
6. Detect partial-sector-erase cases (power loss during erase)
7. Set magic and is_mounted flags
```

#### Detailed Steps

```
Step 1: Config Validation
  - Verify cfg is not NULL
  - Check flash_read, flash_program, flash_erase_sector are non-NULL
  - Check 1 ≤ num_sectors ≤ 64
  - Check sector_size ≥ 32 bytes

Step 2: Scan All Sectors
  For each sector 0..num_sectors-1:
    Read sector header
    If (magic == 0x0FCBF1F0 AND status != 0x00):
      Mark sector as VALID
      Track oldest and newest by sequence number comparison using: (int32_t)(seq_a - seq_b)

Step 3: Handle Recovery Scenarios

  Scenario A: Zero valid sectors → FORMAT INITIAL
    Initialize FCB to empty state
    Erase sector 0
    Write sector 0 header with sequence=1, data_start=16, status=0xFF
    Set delete_ptr = read_ptr = write_ptr = (sector 0, offset 16)
    Set next_sequence = 2
    Return FCB_OK

  Scenario B: One valid sector → RECOVER SINGLE
    Walk records sequentially from data_start offset
    For each record:
      Read header at current offset
      If magic != 0x5A: STOP (end of valid records)
      If length > 1024: STOP (corruption detected)
      If status == 0xFF (active/unread): set read_ptr here (only first occurrence)
      Calculate total_len = 4 + length + 1
      Check if record spans: offset + total_len > sector_size
        If spans: break (stop walking this sector)
      Advance offset past record
    Set write_ptr to offset of next available free space
    If no unread records found: set read_ptr = write_ptr

  Scenario C: Multiple valid sectors → RECOVER CHAIN
    Determine logical sector order: oldest_seq → newest_seq
    Start walking from oldest_sector
    For each sector in chronological order:
      Walk records as in Scenario B
      Track read_ptr, delete_ptr, write_ptr as we go
    When reaching newest_sector:
      Handle any spanning records (data extends into next sector)
      Set write_ptr to position after last valid byte

Step 4: Detect Half-Erased Sectors
  If write_sector NOT fully erased (contains some 0xFF, some data):
    Try to read sector header
    If header magic != 0x0FCBF1F0:
      → Sector is half-erased (power loss during erase)
      → Re-erase it
      → Write fresh header with new sequence number
      → Reset write_offset to 16

Step 5: Finalization
  Set magic = 0xFCB0FCB0
  Set is_mounted = true
  Return FCB_OK
```

#### Recovery Guarantees

- **Power loss during header write:** New sector not yet in valid state (magic mismatch) → treated as erased ✓
- **Power loss during data write:** CRC mismatch detected → record truncated ✓
- **Power loss during erase:** Sector re-erased on next init ✓
- **Monotonic sequences:** Guarantees correct oldest→newest sector ordering ✓

---

### 2. fcb_write() - Append Record

**Signature:**
```c
int fcb_write(Fcb *fcb, const uint8_t *data, size_t len);
```

**Parameters:**
- `fcb`: Initialized FCB instance
- `data`: Pointer to payload bytes
- `len`: Payload length, 1–1024 bytes

**Return codes:** `FCB_OK`, `FCB_FULL`, `FCB_INVALID_ARG`, `FCB_ERR_FLASH`

**Locking:** Acquires mutex if configured

#### Algorithm

```
Preconditions:
  - fcb magic must be 0xFCB0FCB0
  - fcb->is_mounted must be true
  - len must be 1–1024
  - data must be non-NULL

Total record size = 4 (header) + len + 1 (CRC) = 5 + len bytes

Step 1: Check if record header fits in current sector
  remaining_in_sector = sector_size - write_offset
  if header_size (4) > remaining_in_sector:
    → Need to move to next sector
    next_sector = (write_sector + 1) % num_sectors
    if next_sector == read_sector:
      → BUFFER FULL (can't overwrite unread data)
      return FCB_FULL
    Erase next_sector
    Write sector header (sequence++, data_start=16, status=0xFF)
    write_sector = next_sector
    write_offset = 16

Step 2: Write Record Header at current write_offset
  header.magic = 0x5A
  header.length = len
  header.status = 0xFF (active)
  Program header to flash at absolute address:
    start_addr + (write_sector * sector_size) + write_offset

Step 3: Write Data Payload
  Calculate CRC-8 of data (polynomial 0x31, seed 0xFF)
  data_offset = write_offset + 4 (after header)
  remaining_in_sector = sector_size - data_offset

  If len > remaining_in_sector: (spanning case)
    Write first part (remaining_in_sector bytes)
    Erase next sector
    remaining_len = len - remaining_in_sector
    overflow = remaining_len + 1 (for CRC)
    Write next sector header with data_start = 16 + overflow
    write_sector = next_sector
    data_offset = 16
    Write second part (remaining_len bytes)
  Else:
    Write all len bytes at data_offset in current sector

Step 4: Write CRC-8
  crc = fcb_calc_crc8(0xFF, data, len)
  Program 1 byte to flash at data_offset + len
  (After the data payload)

Step 5: Advance write_ptr
  write_offset = (final crc byte offset) + 1
  if write_offset >= sector_size:
    write_sector = (write_sector + 1) % num_sectors
    write_offset -= sector_size (wrap)

Return FCB_OK
```

#### Full-Buffer Detection

Buffer is considered FULL if:
```
next_sector = (write_sector + 1) % num_sectors
AND next_sector == read_sector (about to wrap into unread space)
AND remaining space < max record size (4 + 1024 + 1)
```

#### Power-Fail Safety

- **Sector header written first:** If power lost here, new sector seen as erased ✓
- **Record header written before data:** If power lost during data, CRC invalid ✓
- **CRC written last:** Data integrity verified on read; partial data detected ✓
- **Sequence numbers:** Chronological order recovered via monotonic IDs ✓

---

### 3. fcb_read() - Retrieve Next Unread Record

**Signature:**
```c
int fcb_read(Fcb *fcb, uint8_t *buf, size_t buf_len, size_t *len_out);
```

**Parameters:**
- `fcb`: Initialized FCB instance
- `buf`: Destination buffer (must be ≥ 1024 bytes)
- `buf_len`: Size of destination buffer
- `len_out`: Output parameter, set to actual record length on success

**Return codes:** `FCB_OK`, `FCB_EMPTY`, `FCB_CORRUPTED`, `FCB_INVALID_ARG`, `FCB_ERR_FLASH`

**Locking:** Acquires mutex if configured

#### Algorithm

```
Preconditions:
  - fcb must be initialized and mounted
  - buf must be non-NULL, buf_len > 0
  - len_out must be non-NULL

Step 1: Check if buffer has any unread records
  if read_offset == write_offset (all in same sector):
    if read_sector == write_sector:
      return FCB_EMPTY

Step 2: Read Record Header
  addr = start_addr + (read_sector * sector_size) + read_offset
  Read 4 bytes from flash into record header
  if header.magic != 0x5A:
    return FCB_CORRUPTED (invalid record marker)
  if header.length > 1024:
    return FCB_CORRUPTED (invalid length)

Step 3: Validate Buffer Size
  if header.length > buf_len:
    return FCB_INVALID_ARG (destination buffer too small)

Step 4: Read Data Payload
  data_offset = read_offset + 4 (after header)
  remaining_in_sector = sector_size - data_offset

  if header.length > remaining_in_sector: (spanning case)
    bytes_to_read_1 = remaining_in_sector
    Read bytes_to_read_1 from flash into buf[0..bytes_to_read_1-1]
    
    next_sector = (read_sector + 1) % num_sectors
    bytes_to_read_2 = header.length - bytes_to_read_1
    addr_2 = start_addr + (next_sector * sector_size) + 16 (skip sector header)
    Read bytes_to_read_2 from flash into buf[bytes_to_read_1..end]
    
    crc_offset_sector = next_sector
    crc_offset = 16 + bytes_to_read_2
  Else:
    Read header.length bytes from flash into buf
    crc_offset_sector = read_sector
    crc_offset = data_offset + header.length

Step 4b: Handle CRC-only spill
  If crc_offset >= sector_size:
    crc_offset_sector = (crc_offset_sector + 1) % num_sectors
    crc_offset = 16 (FCB_SECTOR_HDR_SIZE)
  (When data exactly fills remaining sector space, only the CRC byte
   spills into the next sector at offset 16, past the sector header.)

Step 5: Read and Verify CRC-8
  addr_crc = start_addr + (crc_offset_sector * sector_size) + crc_offset
  Read 1 byte (stored CRC) from flash
  calculated_crc = fcb_calc_crc8(0xFF, buf, header.length)
  if stored_crc != calculated_crc:
    Advance read_ptr past the corrupted record (crc_offset + 1)
    Log CRC mismatch warning
    return FCB_CORRUPTED
    (read_ptr is advanced so next fcb_read() returns the following
     record instead of retrying the same broken one forever)

Step 6: Advance read_ptr
  read_sector = crc_offset_sector
  read_offset = crc_offset + 1
  if read_offset >= sector_size:
    read_sector = (read_sector + 1) % num_sectors
    read_offset = sector header data_start (or 16 if header invalid)

Step 7: Output and Return
  *len_out = header.length
  return FCB_OK
```

#### State After Read

- `read_ptr` advanced to next record (or to end if last in sector)
- `delete_ptr` unchanged (records still considered "unconsumed" for fcb_delete())
- `write_ptr` unchanged

---

### 4. fcb_delete() - Mark Records as Consumed

**Signature:**
```c
int fcb_delete(Fcb *fcb);
```

**Return codes:** `FCB_OK`, `FCB_EMPTY`, `FCB_CORRUPTED`, `FCB_ERR_FLASH`

**Locking:** Acquires mutex if configured

**Purpose:** Batch-mark all records from `delete_ptr` to `read_ptr` as consumed (status byte = 0x00).

#### Algorithm

```
Preconditions:
  - fcb must be initialized and mounted

If delete_ptr == read_ptr:
  return FCB_EMPTY (no new records to delete)

While delete_ptr != read_ptr:
  Step 1: Read record header at delete_ptr
    addr = start_addr + (delete_sector * sector_size) + delete_offset
    Read 4 bytes (header)
    if magic != 0x5A:
      return FCB_CORRUPTED

  Step 2: Mark record as consumed (if not already)
    consumed_addr = start_addr + (delete_sector * sector_size) + delete_offset + 3
    (offset +3 = status byte within 4-byte header: BA LL LL SS)
    if current_status == 0xFF:
      Program 1 byte (0x00) to flash at consumed_addr
    (NOR flash allows 0xFF→0x00 without erase)

  Step 3: Advance delete_ptr past this record
    total_record_size = 4 + header.length + 1 (header + data + CRC)
    remaining_in_sector = sector_size - delete_offset

    if total_record_size > remaining_in_sector: (spanning)
      overflow = total_record_size - remaining_in_sector
      delete_sector = (delete_sector + 1) % num_sectors
      delete_offset = 16 + overflow (skip header, start at spill)
    else:
      delete_offset += total_record_size

Return FCB_OK
```

#### Key Characteristics

- **Atomic:** Each consumed-flag write (0xFF→0x00) is atomic on NOR flash
- **Batch-safe:** Even if power lost mid-delete, "partially deleted" records still readable
- **Typical workflow:** Call fcb_read n times in a loop, then fcb_delete once

#### Example Usage Loop

```c
uint8_t buf[1024];
size_t len;

// Read all available records
while (fcb_read(&fcb, buf, sizeof(buf), &len) == FCB_OK) {
    process_record(buf, len);
}

// Mark all as consumed in one operation
fcb_delete(&fcb);
```

---

### 5. fcb_trim() - Erase Oldest Sector

**Signature:**
```c
int fcb_trim(Fcb *fcb);
```

**Return codes:** `FCB_OK`, `FCB_ERR_FLASH`

**Locking:** Acquires mutex if configured

**Purpose:** Erase the oldest sector to reclaim space. Advances delete and read pointers automatically if they point into the erased sector, regardless of consumption state.

#### Algorithm

```
Preconditions:
  - fcb must be initialized and mounted

oldest_sector = delete_sector

Step 1: Erase the oldest sector
  sector_addr = start_addr + (oldest_sector * sector_size)
  Call flash_erase_sector(sector_addr)
  if error: return FCB_ERR_FLASH

Step 2: Advance pointers to point past the erased sector
  next_sector = (oldest_sector + 1) % num_sectors
  
  delete_sector = next_sector
  delete_offset = 16 (after sector header)
  
  if read_sector == oldest_sector:
    read_sector = next_sector
    read_offset = 16

Return FCB_OK
```

#### After Erase

```
Before:
  Sector0 [oldest, may have unread/unconsumed data]
  Sector1 [newer, delete_ptr here after previous trim]
  Sector2 [newer, read_ptr here (unread records exist)]
  Sector3 [newest, write_ptr here]

After fcb_trim() erases Sector0:
  Sector0 [erased, ready for new writes]
  (delete_ptr automatically advanced to Sector1 if it was in Sector0)
  (read_ptr automatically advanced to Sector1 if it was in Sector0)
  Sector1 [may now contain delete_ptr and/or read_ptr]
  Sector2 [newer]
  Sector3 [write_ptr can now wrap around and reuse Sector0]

Note: Any records that were in the erased sector (read or unread) are lost.
      The caller is responsible for ensuring they have processed all needed records before calling fcb_trim().
```

---

### 6. fcb_is_full() - Check if Buffer is Full

**Signature:**
```c
bool fcb_is_full(const Fcb *fcb);
```

**Return:** `true` if buffer cannot accept another max-size record, `false` otherwise.

**Logic:**
```c
next_sector = (write_sector + 1) % num_sectors;
if (next_sector == read_sector) {
    // Next write would wrap into unread space; check if room for max record
    free_space_in_current = sector_size - write_offset;
    if (free_space_in_current < 4 + 1024 + 1) {
        return true;  // Not enough room for max record
    }
}
return false;  // Room available
```

---

### 7. fcb_is_empty() - Check if Buffer is Empty

**Signature:**
```c
bool fcb_is_empty(const Fcb *fcb);
```

**Return:** `true` if no unread records, `false` otherwise.

**Logic:**
```c
return (read_sector == write_sector) && (read_offset == write_offset);
```

---

## Recovery Mechanisms

### Power-Loss Scenarios

#### Scenario 1: Power Loss During Header Write

```
Before:
  Sector N status: 0xFF (valid)
  write_offset points to free space

Operation:
  Program sector header (start_addr + N*sector_size)
  Power loss DURING write → partial bytes written

Result:
  Sector N contains partial/corrupted magic or fields
  On next init, read_sector_header sees:
    - magic != 0x0FCBF1F0 OR
    - status field not properly set
  → Sector treated as invalid/erased
  → Recovery continues from previous valid sector
  → Sector will be re-initialized on next write
```

**Recovery:** Automatic; sector re-formatted on write.

---

#### Scenario 2: Power Loss During Data Write

```
Before:
  Record header written
  CRC pre-calculated as 0x7E

Operation:
  Program data bytes
  Power loss DURING write → partial data written

Result:
  On read, CRC calculated from partial data ≠ 0x7E
  fcb_read detects CRC mismatch
  → Advance read_ptr past the corrupted record
  → Return FCB_CORRUPTED

Usage Pattern:
  If fcb_read returns FCB_CORRUPTED:
    - read_ptr has already been advanced past the bad record
    - Call fcb_read again → FCB_EMPTY or next valid record
    - Only the corrupted record is lost; remaining records accessible
```

**Recovery:** Partial record detected via CRC. read_ptr auto-advanced to prevent stall.

---

#### Scenario 3: Power Loss During Erase

```
Before:
  erase_sector(Sector 0)
  Erase BEGINS but doesn't complete → sector half-erased

After Power Restore:
  Sector 0 contains mix of 0xFF and data
  Sector 0 header read:
    - May see corrupted magic (partial erase)
    - OR header already erased (0xFF values)

On next fcb_init:
  fcb_is_sector_erased checks every byte → finds mixed state
  Sector 0 header read fails (not all 0xFF, not valid magic)
  → Trigger re-erase of Sector 0
  → Write fresh header with new sequence number
  → Sector now safe for use
```

**Recovery:** Automatic re-erase on init; detected by checking sector header validity.

---

#### Scenario 4: Power Loss During Sector Spanning

```
Before:
  Record payload spans Sector N → Sector N+1
  Part 1 (60 bytes) written to Sector N
  Power loss BEFORE Sector N+1 erased

Result:
  Sector N contains partial record (incomplete picture)
  Sector N+1 erased (untouched)

On next init:
  Walk Sector N: read header → length=100 bytes
  Try to read 100 bytes:
    - 60 bytes from Sector N (partial) ✓
    - 40 bytes expected from Sector N+1 → read 0xFF (erased)
  CRC of (60 bytes data + 0xFF padding) ≠ expected CRC
  → CRC mismatch detected
  → Recovery STOPS at this sector
  → write_ptr placed at Sector N offset (no spanning record)
```

**Recovery:** Automatic truncation; CRC detects incomplete spanning.

---

### Monotonic Sequence Recovery

When multiple sectors exist, their chronological order is determined via sequence numbers:

```
Sector 0: sequence=1003
Sector 1: sequence=1004
Sector 2: sequence=1002
Sector 3: sequence=1005 (newest)

Chronological order (oldest→newest, using signed distance):
  (1005 - 1002) = 3 > 0 → 1005 is newer ✓
  (1003 - 1002) = 1 > 0 → 1003 is newer than 1002 ✓
  (1005 - 1003) = 2 > 0 → 1005 is newer ✓

Recovery walks: Sector 2 → Sector 0 → Sector 1 → Sector 3
```

This also handles wrap-around:
```
Sector 0: sequence=0xFFFFFFFF
Sector 1: sequence=0x00000001

(1 - 0xFFFFFFFF) = 2 > 0 → sequence 1 is newer ✓
```

---

## Thread Safety

### Mutex Callbacks

If provided, FCB acquires a mutex before critical sections:

```c
// In fcb_write() / fcb_read() / fcb_delete()
fcb_lock(fcb);           // Calls config.lock(config.mutex_ctx)
  // ... perform operation ...
fcb_unlock(fcb);         // Calls config.unlock(config.mutex_ctx)
```

### Thread-Safe Operations

- `fcb_write()`: Serializes all writes; multiple threads cannot write simultaneously ✓
- `fcb_read()`: Serializes all reads; multiple threads cannot read simultaneously ✓
- `fcb_delete()`: Serializes deletion; no concurrent read/delete ✓
- `fcb_init()`: NOT locked (should be called once during initialization only)

### Typical Multi-Threaded Usage

```c
// Initialization (single-threaded, before spawning worker threads)
Fcb fcb;
FcbConfig cfg = { ... lock, unlock callbacks ... };
fcb_init(&fcb, &cfg);

// Worker Thread 1: Write telemetry
while (true) {
    uint8_t telemetry[100];
    read_sensor(telemetry);
    fcb_write(&fcb, telemetry, sizeof(telemetry));  // Mutex acquired internally
    sleep_ms(100);
}

// Worker Thread 2: Read and transmit
while (true) {
    uint8_t buf[1024];
    size_t len;
    while (fcb_read(&fcb, buf, sizeof(buf), &len) == FCB_OK) {  // Mutex acquired
        tx_to_server(buf, len);
    }
    fcb_delete(&fcb);  // Mark all as consumed
    sleep_s(60);
}
```

---

## Error Codes

### Enumeration

```c
typedef enum {
    FCB_OK               = 0,   // Operation successful
    FCB_FULL             = 1,   // Buffer full; no room for new write
    FCB_EMPTY            = 2,   // No records to read; no records to delete
    FCB_NOT_CONSUMED     = 3,   // (Deprecated; no longer used)
    FCB_CORRUPTED        = 4,   // CRC mismatch; record integrity check failed
    FCB_INVALID_ARG      = 5,   // Invalid argument to API function
    FCB_POWER_LOSS_DETECTED = 6,// (Deprecated; now uses FCB_CORRUPTED)
    FCB_ERR_FLASH        = 7,   // Flash driver error (read/program/erase failed)
} FcbError;
```

### Semantics

| Code | Meaning | Typical Action |
|------|---------|-----------------|
| `FCB_OK` | Operation completed successfully | Continue |
| `FCB_FULL` | Buffer cannot accept more writes | Wait for reads/deletes to free space |
| `FCB_EMPTY` | No records available for read or delete | Retry later; check availability |
| `FCB_NOT_CONSUMED` | (Deprecated; no longer used) | — |
| `FCB_CORRUPTED` | CRC mismatch detected; data integrity compromised | Skip record; may indicate power loss |
| `FCB_INVALID_ARG` | Invalid parameter to function | Debug argument values; check initialization state |
| `FCB_ERR_FLASH` | Flash driver callback returned error | Verify flash hardware; check driver implementation |

---

## Constants & Limits

### On-Flash Constants

```c
#define FCB_SECTOR_MAGIC        0x0FCBF1F0U      // Sector header magic
#define FCB_RECORD_MAGIC        0x5AU            // Record header magic

#define FCB_SECTOR_STATUS_VALID    0xFFU         // Sector valid (not yet consumed)
#define FCB_SECTOR_STATUS_CONSUMED 0x00U         // Sector consumed (safe to erase)

#define FCB_RECORD_ACTIVE       0xFFU            // Record active (not yet deleted)
#define FCB_RECORD_CONSUMED     0x00U            // Record consumed (marked deleted)
```

### In-Memory Constants

```c
#define FCB_INIT_MAGIC          0xFCB0FCB0U      // Canary set after successful init

#define FCB_MAX_SECTORS         64U              // Maximum number of sectors
#define FCB_MAX_RECORD_SIZE     1024U            // Maximum payload per record

#define FCB_SECTOR_HDR_SIZE     16U              // Bytes: sector header
#define FCB_RECORD_HDR_SIZE     4U               // Bytes: record header
```

### Derived Limits

```
Min buffer size:       num_sectors=1, sector_size=32
Min payload+header:    4 + 1 + 1 = 6 bytes
Absolute min space:    32 bytes/sector

Max buffer capacity:   64 sectors * 65536 bytes/sector = 4 MB (typical)
Max record payload:    1024 bytes

Typical configuration:
  num_sectors = 8
  sector_size = 65536 bytes (64 KB, common NOR flash sector)
  Total buffer = 512 KB
```

---

## Examples

### Example 1: Basic Initialization and Write

```c
#include "fcb.h"
#include <stdint.h>
#include <string.h>

// Flash driver callbacks (simplified)
int my_flash_read(void *ctx, uint32_t addr, uint8_t *buf, size_t len) {
    // Read len bytes from SPI NOR flash at addr
    return spi_nor_read(addr, buf, len);
}

int my_flash_program(void *ctx, uint32_t addr, const uint8_t *data, size_t len) {
    // Write len bytes to SPI NOR flash at addr (up to 256 bytes)
    return spi_nor_write(addr, data, len);
}

int my_flash_erase_sector(void *ctx, uint32_t addr) {
    // Erase 64 KB sector containing addr
    return spi_nor_erase(addr);
}

void main(void) {
    // Configuration
    FcbConfig cfg = {
        .start_addr = 0x00100000,           // FCB starts at 1 MB in flash
        .num_sectors = 8,
        .sector_size = 65536,               // 64 KB sectors
        .flash_ctx = NULL,
        .flash_read = my_flash_read,
        .flash_write = my_flash_program,
        .flash_erase = my_flash_erase_sector,
        .lock = NULL,                       // No thread safety needed
        .unlock = NULL,
        .mutex_ctx = NULL,
    };

    // Initialize FCB
    Fcb fcb;
    int rc = fcb_init(&fcb, &cfg);
    if (rc != FCB_OK) {
        printf("FCB init failed: %d\n", rc);
        return;
    }
    printf("FCB initialized: %u sectors, %u bytes/sector\n", 
           cfg.num_sectors, cfg.sector_size);

    // Write a record
    uint8_t data[] = "Hello, FCB!";
    rc = fcb_write(&fcb, data, sizeof(data) - 1);  // Exclude null terminator
    if (rc != FCB_OK) {
        printf("FCB write failed: %d\n", rc);
        return;
    }
    printf("Wrote %zu bytes\n", sizeof(data) - 1);

    // Write another record
    uint8_t data2[] = "Record 2";
    rc = fcb_write(&fcb, data2, sizeof(data2) - 1);
    if (rc != FCB_OK) {
        printf("FCB write failed: %d\n", rc);
        return;
    }
    printf("Wrote %zu bytes\n", sizeof(data2) - 1);
}
```

### Example 2: Read and Delete Workflow

```c
void telemetry_workflow(Fcb *fcb) {
    uint8_t buf[1024];
    size_t len;
    int record_count = 0;

    // Read all unread records
    while (fcb_read(fcb, buf, sizeof(buf), &len) == FCB_OK) {
        // Process record
        printf("Record %d: %u bytes\n", record_count++, len);
        
        // Example: print as string if null-terminated
        if (len < sizeof(buf)) {
            buf[len] = '\0';
            printf("  Data: %s\n", (char *)buf);
        }
    }

    printf("Read %d records\n", record_count);

    // Mark all read records as consumed (published to server)
    if (fcb_delete(fcb) != FCB_OK) {
        printf("FCB delete failed\n");
        return;
    }
    printf("Marked %d records as consumed\n", record_count);

    // Trim oldest sector to reclaim space (automatically advances pointers)
    int trim_rc = fcb_trim(fcb);
    if (trim_rc == FCB_OK) {
        printf("Trimmed oldest sector\n");
    } else if (trim_rc == FCB_ERR_FLASH) {
        printf("Trim failed: flash driver error\n");
    }
}
```

### Example 3: Continuous Logging with Full-Buffer Handling

```c
void continuous_logging(Fcb *fcb) {
    uint32_t log_id = 0;

    while (1) {
        // Create a log record
        uint8_t log_record[256];
        uint16_t len = snprintf((char *)log_record, sizeof(log_record),
                                "LOG_%u: timestamp=0x%08x, event=SENSOR_READ",
                                log_id++, get_timestamp());

        // Try to write
        int rc = fcb_write(fcb, log_record, len);

        if (rc == FCB_OK) {
            printf("Logged record %u\n", log_id - 1);
        } else if (rc == FCB_FULL) {
            printf("Buffer full, cannot write more\n");
            
            // In production, you would:
            // 1. Stop logging temporarily
            // 2. Read all records
            // 3. Upload to server
            // 4. Delete consumed records
            // 5. Trim if needed
            // 6. Resume logging
            break;  // Stop for now
        } else {
            printf("Write error: %d\n", rc);
            break;
        }

        sleep_ms(100);  // Log rate: 10 records/sec
    }
}
```

### Example 4: Recovery After Power Loss

```c
void power_restored_handler(void) {
    FcbConfig cfg = { ... };  // Same config as before
    Fcb fcb;

    // fcb_init scans all sectors and recovers state
    int rc = fcb_init(&fcb, &cfg);

    if (rc == FCB_OK) {
        printf("FCB recovered after power loss\n");
        printf("Write pointer: Sector %u, offset %u\n", 
               fcb.write_sector, fcb.write_offset);
        printf("Read pointer: Sector %u, offset %u\n", 
               fcb.read_sector, fcb.read_offset);
        printf("Unread records available for upload\n");

        // Existing unread records are preserved
        // Partially-written records (CRC fail) are skipped
        // Can resume normal operation immediately
    } else {
        printf("FCB recovery failed: %d\n", rc);
        // Handle error (factory reset, etc.)
    }
}
```

### Example 5: Memory Layout Calculation

```c
void print_fcb_layout(const Fcb *fcb) {
    uint32_t total_flash = fcb->config.num_sectors * fcb->config.sector_size;
    uint32_t header_overhead = fcb->config.num_sectors * FCB_SECTOR_HDR_SIZE;
    uint32_t usable = total_flash - header_overhead;

    printf("=== FCB Memory Layout ===\n");
    printf("Start address:    0x%08x\n", fcb->config.start_addr);
    printf("Number of sectors: %u\n", fcb->config.num_sectors);
    printf("Sector size:      %u bytes\n", fcb->config.sector_size);
    printf("\n");
    printf("Total buffer:     %u bytes (%.1f KB)\n", total_flash, total_flash / 1024.0);
    printf("Header overhead:  %u bytes\n", header_overhead);
    printf("Usable for data:  %u bytes (%.1f KB)\n", usable, usable / 1024.0);
    printf("\n");

    // Pointer state
    uint32_t delete_addr = fcb->config.start_addr + 
                           (fcb->delete_sector * fcb->config.sector_size) + 
                           fcb->delete_offset;
    uint32_t read_addr = fcb->config.start_addr + 
                         (fcb->read_sector * fcb->config.sector_size) + 
                         fcb->read_offset;
    uint32_t write_addr = fcb->config.start_addr + 
                          (fcb->write_sector * fcb->config.sector_size) + 
                          fcb->write_offset;

    printf("Pointer Locations:\n");
    printf("Delete ptr: Sector %u, offset %u (addr 0x%08x)\n", 
           fcb->delete_sector, fcb->delete_offset, delete_addr);
    printf("Read ptr:   Sector %u, offset %u (addr 0x%08x)\n", 
           fcb->read_sector, fcb->read_offset, read_addr);
    printf("Write ptr:  Sector %u, offset %u (addr 0x%08x)\n", 
           fcb->write_sector, fcb->write_offset, write_addr);
    printf("\n");

    // Check state
    if (fcb_is_full(fcb)) {
        printf("State: FULL (cannot accept new writes)\n");
    } else if (fcb_is_empty(fcb)) {
        printf("State: EMPTY (no records to read)\n");
    } else {
        printf("State: NORMAL (has unread records, space available)\n");
    }
}
```

### Example 6: Byte-Level Hexdump Interpretation

```
Sector 1, Offset 16 (first record after sector header):

Hexdump:
+0000: 5A 0A 00 FF  48 65 6C 6C 6F 57 6F 72 6C 64 7E
       ^^^^^^^^^^^^  ^^^^^^^^^^^^^^ (10 bytes)  ^^
       Record        Payload        CRC

Parsing:
  5a              = Record magic (0x5A) ✓
  0a 00           = Length: 0x000A = 10 bytes ✓
  ff              = Status: 0xFF (active/unread) ✓
  48 65...6C 64   = Payload: "HelloWorld" (10 bytes) ✓
  7e              = CRC-8 (polynomial 0x31, seed 0xFF)

Verification:
  calc_crc8(0xFF, "HelloWorld", 10) = 0x7E ✓
  Matches stored CRC → record integrity verified

If reading into buf[1024]:
  buf[0..9] = "HelloWorld"
  len_out = 10
  Return FCB_OK
```

---

## Implementation Notes for AI Agent Integration

### Key Parsing Points

1. **Initialization State:**
   - Check `fcb->magic == 0xFCB0FCB0` before any operation
   - Check `fcb->is_mounted == true`
   - If either is false, initialization failed

2. **Pointer Arithmetic:**
   - All addresses use modulo arithmetic: `(x + 1) % num_sectors`
   - Sector offsets start at 16 (FCB_SECTOR_HDR_SIZE), not 0
   - Record offsets are relative to sector base, NOT absolute flash addresses

3. **Memory Layout Calculation:**
   ```
   Absolute address = config.start_addr + (sector_index * config.sector_size) + offset_in_sector
   ```

4. **CRC-8 Polynomial:** `0x31` with MSB-first bit processing

5. **Record Spanning:**
   - Header (4 bytes) always fits in sector; data may split
   - Split records require pre-erasing and pre-writing next sector header
   - CRC location depends on which sector contains data end

6. **Error Recovery:**
   - CRC mismatch → corrupted record, skip it
   - Magic mismatch → invalid sector, stop recovery walk
   - Half-erased sector → auto-re-erase on init

7. **Thread Safety:** Mutex acquired at function entry (fcb_write/read/delete), released at exit

### New Public Helper Function: `fcb_seq_diff()`

**Signature:**
```c
int32_t fcb_seq_diff(uint32_t a, uint32_t b);
```

**Purpose:** Calculate signed distance between two monotonic sequence numbers, accounting for 32-bit wrap-around.

**Usage:** Returns positive if `a` is newer than `b`, negative if older, zero if equal.

**Implementation:**
```c
return (int32_t)(a - b);
```

**Examples:**
```
fcb_seq_diff(5, 3) → 2 (5 is 2 newer than 3) ✓
fcb_seq_diff(3, 5) → -2 (3 is 2 older than 5) ✓
fcb_seq_diff(0x00000001, 0xFFFFFFFF) → 2 (wraps: 1 is 2 newer) ✓
fcb_seq_diff(0xFFFFFFFF, 0x00000001) → -2 (wraps: 0xFFFFFFFF is 2 older) ✓
```

Used internally in `fcb_find_oldest_newest()` to determine sector chronological order.

---

### Pseudo-Code Templates for AI Integration

```
// Template: Traverse all unread records
```c
while (fcb_is_empty(fcb) == false) {
    uint8_t buf[1024];
    size_t len;
    int rc = fcb_read(fcb, buf, sizeof(buf), &len);
    if (rc != FCB_OK) break;  // Corrupted record or other error
    process(buf, len);
}
```

```
// Template: Check flash address validity
```c
bool is_valid_addr(const Fcb *fcb, uint32_t addr) {
    uint32_t end_addr = fcb->config.start_addr + 
                        (fcb->config.num_sectors * fcb->config.sector_size);
    return addr >= fcb->config.start_addr && addr < end_addr;
}
```

```
// Template: Calculate record overhead
```c
uint32_t record_overhead(uint32_t payload_len) {
    return 4 + payload_len + 1;  // header + data + CRC
}
```

---

**Document End**

This document is optimized for:
- **AI agent parsing:** Structured byte-level details, clear data layouts, explicit address calculations
- **Implementation reference:** Complete algorithm pseudocode, error handling, state transitions
- **Integration guidance:** Memory layout calculations, pointer arithmetic, recovery scenarios
