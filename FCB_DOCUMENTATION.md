# Flash Circular Buffer (FCB) Documentation

The Flash Circular Buffer (FCB) is a robust, power-fail-safe, circular FIFO implementation specifically designed for SPI NOR flash memory. It provides a reliable way to store sequential records (e.g., logs, events, telemetry) where data integrity and recovery from unexpected power loss are critical.

## 1. Design Philosophy

### 1.1 Power-Fail Safety
The FCB is designed to ensure that the buffer state remains recoverable at any point, even if power is lost during a write or erase operation.
*   **Sequential Writes:** Record data is written sequentially (Header → Data → CRC8). Sector headers are written at the beginning of the sector to maintain structure.
*   **State Transitions:** Sector status transitions from erased/valid (`0xFF`) to consumed (`0x00`) only clearing bits, exploiting NOR flash's write-once property.
*   **Atomic Operations:** The status flag in record headers transitions from `0xFF` to `0x00`, which is an atomic operation on NOR flash.
*   **CRC-8 Validation:** A CRC-8 checksum is appended after the record data and validates the data payload integrity.

### 1.2 NOR Flash Optimization
*   **Sector-Based Erasure:** The buffer is divided into multiple sectors. Erasure occurs at the sector level only when all records within that sector have been consumed.
*   **Minimal RAM Usage:** The state is maintained in a small `fcb_t` structure. No large RAM buffers or look-up tables are required.

## 2. Architecture


### 2.1 On-Flash Data Structures

#### Sector Header (16 bytes)
Located at the beginning of every sector.
| Offset | Size | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | 4 | `magic` | `0x0FCBF1F0` ensures the sector is a valid FCB sector. |
| 4 | 4 | `sequence` | Monotonically increasing number used to determine logical order. |
| 8 | 2 | `data_start` | Offset from sector start to the first NEW record header. |
| 10 | 1 | `status` | `0xFF` (erased/valid) or `0x00` (consumed). |
| 11 | 5 | `reserved` | Future use. |

#### Record Header (4 bytes)
Located sequentially within the sector. Each record header is followed by the record data and a CRC-8 byte.

| Offset | Size | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | 1 | `magic` | Identifies a valid record header. |
| 1 | 2 | `length` | Size of the data payload (1–1024 bytes). |
| 3 | 1 | `status` | Active (active != `0x00`) or `0x00` (consumed). |

**Record Entry format:**
`<magic><len1><len2><status><data><data_crc8>`
*   `data_crc8` (1 byte) is appended immediately after the data payload and is calculated **ONLY** over the data payload.

### 2.2 Flash Memory Layout
The FCB treats the assigned flash region as a contiguous array of sectors. Within each sector, records are written sequentially one after another, growing upwards from the Sector Header.

```text
Flash Start Address
|
v
+-----------------------+ <--- Sector 0 Start
| Sector Header (16B)   |   (Status = 0xFF = Valid)
+-----------------------+
| Record 1 Header (4B) |
+-----------------------+
| Record 1 Data (Len)   |
+-----------------------+
| Record 1 CRC8 (1B)    |
+-----------------------+
| Record 2 Header (4B) |
+-----------------------+
| Record 2 Data (Len)   |
+-----------------------+
| ...                   |
+-----------------------+
```

*   **Sequential Placement:** Records grow from the top of the sector space (address increasing order).
*   **Data Splitting:** Record data **can** split and span across sector boundaries. If the data exceeds the space in the current sector, it continues in the next sector (starting immediately after the Sector Header).
*   **Header Integrity:** Record headers **cannot** split between sectors. A header must fit entirely within a single sector.

### 2.3 Memory Representation (RAM)
The `fcb_t` structure maintains the runtime state:
*   **Pointer Architecture:**
    *   `delete_ptr`: Points to the start of the oldest record known to the system.
    *   `read_ptr`: Points to the start of the next record to be returned by `fcb_read`.
    *   `write_ptr`: Points to the exact flash address where the next record header will be written.
*   **Configuration:** Stores flash driver callbacks and geometry.

---

## 3. Core Operations

### 3.1 Mounting and Recovery (`fcb_init`)
Upon initialization, the FCB performs a full recovery scan:

1.  **Scan Sector Headers:** Finds all valid FCB sectors and identifies the oldest/newest based on sequence numbers.
2.  **Sequential Walk:** Starting from the oldest sector, the recovery process scans records sequentially from the beginning of the sector (after the Sector Header).
3.  **Validate Records:** For each record, it reads the 4-byte header. If valid, it skips the `length` bytes of data to find and verify the CRC-8 byte.
4.  **Find read_ptr:** Finds the first record with a `status` of `0xFF` (unread).
5.  **Find write_ptr:** Positions the write pointer at the first available byte after the last valid record in the newest sector.

### 3.2 Appending Records (`fcb_write`)
*   Writes records sequentially into the current `write_ptr` sector.
*   **Write Flow:**
    1.  Writes the 4-byte Record Header.
    2.  Writes the record data (managing cross-sector spans if needed).
    3.  Writes the CRC-8 byte immediately following the data.
*   **Advance Pointer:** Updates `write_ptr` to the next available address byte.

### 3.3 Reading Records (`fcb_read`)
*   Reads the record starting at `read_ptr`.
*   Reconstructs the data (recovering from sector splits if applicable).
*   Validates integrity using the appended CRC-8 byte.

### 3.4 Consuming and Deleting (`fcb_delete`)
*   Marks records as consumed by writing `0x00` into the `status` flag in the Record Header.
*   Marks **all** records from `delete_ptr` up to `read_ptr` in a loop (batch deletion).
*   Advances `delete_ptr` accordingly.

### 3.5 Sector Trim (`fcb_trim`)
*   Erases sectors only when all records within them are marked as consumed.
*   Marks the Sector Header status as `0x00` before erasure to aid recovery scanning.


## 4. API Reference

| Function | Description |
| :--- | :--- |
| `fcb_init` | Mounts the FCB and performs recovery scan. |
| `fcb_write` | Appends a new record to the buffer. |
| `fcb_read` | Peeks at the oldest unconsumed record. |
| `fcb_delete` | Marks all records between delete_ptr and read_ptr as consumed. |
| `fcb_trim` | Erases the oldest sector if fully consumed. |
| `fcb_is_full` | Checks if the buffer has room for a max-size record. |
| `fcb_is_empty` | Checks if there are any unconsumed records. |

## 5. Implementation Notes & Limitations

### Sector Status Lifecycle
The sector status field transitions through states following NOR flash's one-way bit clearing property:
```text
0xFF (Erased/Valid) 
  ↓ [mark all records consumed]
0x00 (Consumed) 
  ↓ [erase sector]
0xFF (Erased/Valid)
```

The consumed status (`0x00`) is marked before erasure as an optimization flag and recovery aid, allowing the system to quickly identify sectors that contain only fully-consumed data.

> [!IMPORTANT]
> **Record Spanning:** Record data can cross sector boundaries if it extends beyond the current sector.

> [!IMPORTANT]
> **Header Fit:** Record headers **cannot** split between sectors. If there is not enough space for the 4-byte header at the end of a sector, it must be written to the next sector.

> [!IMPORTANT]
> **Thread Safety:** The FCB provides lock/unlock callbacks in its configuration. These should be populated if the FCB will be accessed from multiple threads or interrupts.

