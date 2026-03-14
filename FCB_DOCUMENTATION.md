# Flash Circular Buffer (FCB) Documentation

The Flash Circular Buffer (FCB) is a robust, power-fail-safe, circular FIFO implementation specifically designed for SPI NOR flash memory. It provides a reliable way to store sequential records (e.g., logs, events, telemetry) where data integrity and recovery from unexpected power loss are critical.

## 1. Design Philosophy

### 1.1 Power-Fail Safety
The FCB is designed to ensure that the buffer state remains recoverable at any point, even if power is lost during a write or erase operation.
*   **Ordered Writes:** Sector headers are written before any records. Record headers are written before the record data. This allows the recovery process to identify partially written or corrupted data.
*   **Atomic State Transitions:** NOR flash's property of only allowing bits to transition from 1 to 0 (unless erased) is exploited. The "consumed" flag transitions from `0xFF` to `0x00`, which is an atomic operation on NOR flash.
*   **CRC Validation:** Every record includes a CRC-32 of its data payload. This ensures that any data corruption (e.g., from an interrupted write) is detected.

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
| 8 | 1 | `status` | `0xFF` (erased) or `0xAA` (valid). |
| 9 | 7 | `reserved` | Future use. |

#### Record Header (12 bytes)
Placed before every record payload.
| Offset | Size | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | 4 | `magic` | `0x0FCBDA7A` identifies a valid record header. |
| 4 | 2 | `length` | Size of the data payload (1–1024 bytes). |
| 6 | 4 | `crc32` | CRC-32 checksum of the payload data. |
| 10 | 1 | `consumed` | `0xFF` (active) or `0x00` (consumed). |
| 11 | 1 | `reserved` | Future use. |

### 2.2 Flash Memory Layout
The FCB treats the assigned flash region as a contiguous array of sectors, which are mathematically mapped to a circular buffer.

```text
Flash Start Address
|
v
+-----------------------+ <--- Sector 0
| Sector Header (16B)   |
+-----------------------+
| Record 1 Header (12B) |
+-----------------------+
| Record 1 Data         |
+-----------------------+
| ...                   |
+-----------------------+ <--- Sector 1
| Sector Header (16B)   |
+-----------------------+
| Record N Header (12B) |
+-----------------------+
| Record N Data (Part 1)|
+-----------------------+ <--- Sector 2 (Wrap/Span example)
| Sector Header (16B)   |
+-----------------------+
| Record N Data (Part 2)|
+-----------------------+
| Record N+1 Header     |
+-----------------------+
| ...                   |
+-----------------------+
```

*   **Logical vs. Physical:** While sectors are physical contiguous blocks, the `sequence` number in the header determines the logical "oldest" to "newest" order.
*   **Data Spanning:** Record data can cross sector boundaries. In such cases, the data continues immediately after the next sector's header.

### 2.3 Memory Representation (RAM)
The `fcb_t` structure maintains the runtime state:
*   **Three-Pointer Architecture:**
    *   `delete_ptr`: (Sector index, Offset). Points to the start of the oldest record known to the system.
    *   `read_ptr`: (Sector index, Offset). Points to the start of the next record to be returned by `fcb_read`.
    *   `write_ptr`: (Sector index, Offset). Points to the exact flash address where the next write operation will begin.
*   **Configuration:** Stores flash driver callbacks (`read`, `program`, `erase_sector`) and geometry (size, address).

## 3. Core Operations

### 3.1 Mounting and Recovery (`fcb_init`)
Upon initialization, the FCB performs a full recovery scan:
1.  **Scan Sector Headers:** Finds all valid FCB sectors and identifies the oldest/newest based on sequence numbers.
2.  **Walk Records:** Iterates from the oldest sector to find the current `read_ptr` and `write_ptr`.
3.  **Validate Integrity:** Checks record magic and CRC. Invalid records are treated as the end of valid data.

### 3.2 Appending Records (`fcb_write`)
1.  **Space Check:** Ensures there is enough room for the header and data.
2.  **Sector Management:** If the current sector is full, it prepares the next one by writing a new header with an incremented sequence.
3.  **Ordered Program:**
    *   Writes the record header first.
    *   Writes the data payload (which can span into the next sector).
    *   Updates the `write_ptr`.

### 3.3 Reading Records (`fcb_read`)
*   Provides a non-destructive peek at the record pointed to by `read_ptr`.
*   Validates the record's CRC before returning.

### 3.4 Consuming and Deleting (`fcb_delete`)
*   Marks the record at `delete_ptr` as consumed by writing `0x00` to its `consumed` flag.
*   Advances `delete_ptr` to the next record.
*   If `read_ptr` was at the same position, it also advances (skipping the consumed record).

### 3.5 Sector Discard (`fcb_discard_oldest_sector`)
*   Erases the oldest sector, but only if all records within it are marked as consumed.
*   The `read_ptr` must have moved into a subsequent sector before the oldest can be erased.

## 4. API Reference

| Function | Description |
| :--- | :--- |
| `fcb_init` | Mounts the FCB and performs recovery scan. |
| `fcb_write` | Appends a new record to the buffer. |
| `fcb_read` | Peeks at the oldest unconsumed record. |
| `fcb_delete` | Marks the oldest unconsumed record as consumed. |
| `fcb_discard_oldest_sector` | Erases the oldest sector if fully consumed. |
| `fcb_is_full` | Checks if the buffer has room for a max-size record. |
| `fcb_is_empty` | Checks if there are any unconsumed records. |

## 5. Implementation Notes & Limitations

> [!WARNING]
> **Record Spanning:** While record headers must fit entirely within a single sector, data payloads *can* span into the very next sector. This ensures reliable header reads.

> [!IMPORTANT]
> **Thread Safety:** The FCB provides lock/unlock callbacks in its configuration. These should be populated if the FCB will be accessed from multiple threads or interrupts.

### Known Issues
*   **Collision Detection Bug:** There is a known bug in `is_full_nolock` and `prepare_next_sector` where collision checks are conditionally skipped if both pointers are in the same sector. This can lead to buffer state corruption under specific wrap-around conditions.
