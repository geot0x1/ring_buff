# Flash Circular Buffer (FCB) Documentation

The Flash Circular Buffer (FCB) is a robust, power-fail-safe, circular FIFO implementation specifically designed for SPI NOR flash memory. It provides a reliable way to store sequential records (e.g., logs, events, telemetry) where data integrity and recovery from unexpected power loss are critical.

## 1. Design Philosophy

### 1.1 Power-Fail Safety
The FCB is designed to ensure that the buffer state remains recoverable at any point, even if power is lost during a write or erase operation.
*   **Ordered Writes:** Record data is written first, followed by the record header at the end of the sector. Sector headers are written before any records in that sector. This allows the recovery process to identify partially written or corrupted data.
*   **Atomic State Transitions:** NOR flash's property of only allowing bits to transition from 1 to 0 (unless erased) is exploited. The "consumed" flag transitions from `0xFF` to `0x00`, which is an atomic operation on NOR flash.
*   **CRC-8 Validation:** Every record header includes a CRC-8 of the header itself (excluding the consumed flag). This ensures that header corruption is detected.

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

#### Record Header (8 bytes)
Located at the END of every sector (highest address), growing downward. Each record header stores metadata about a corresponding record's data.

| Offset | Size | Field | Description |
| :--- | :--- | :--- | :--- |
| 0 | 2 | `magic` | `0xFCBA` identifies a valid record header. |
| 2 | 2 | `length` | Size of the data payload (1–1024 bytes). |
| 4 | 2 | `offset` | Byte offset from the sector start where the record data begins. |
| 6 | 1 | `consumed` | `0xFF` (active) or `0x00` (consumed). |
| 7 | 1 | `crc8` | CRC-8 checksum of the header (excluding the consumed flag). |

### 2.2 Flash Memory Layout
The FCB treats the assigned flash region as a contiguous array of sectors, which are mathematically mapped to a circular buffer. Within each sector, record data grows upward from the sector start, while record headers grow downward from the sector end.

```text
Flash Start Address
|
v
+-----------------------+ <--- Sector 0 Start
| Data Payload 1        |
+-----------------------+
| Data Payload 2        |
+-----------------------+
| ...                   |
+-----------------------+
| Record Header 2 (8B)  |  <--- Headers at end, growing down
| Record Header 1 (8B)  |
| Sector Header (16B)   |
+-----------------------+ <--- Sector 0 End (highest address)
|
v
+-----------------------+ <--- Sector 1 Start
| Data Payload N (Part 1) |
+-----------------------+
| ...                   |
+-----------------------+
| Record Header 1 (8B)  |
| Sector Header (16B)   |
+-----------------------+ <--- Sector 1 End (highest address)
|
v
+-----------------------+ <--- Sector 2 Start
| Data Payload N (Part 2) |  (continuation from Sector 1)
+-----------------------+
| Data Payload N+1      |
+-----------------------+
| Record Header 2 (8B)  |
| Record Header 1 (8B)  |
| Sector Header (16B)   |
+-----------------------+ <--- Sector 2 End (highest address)
```

*   **Layout Inversion:** Sector headers and record headers are now located at the END (highest address) of each sector. Data payloads grow upward from the sector start. Record headers contain an `offset` field that points to where each record's data begins within the sector.
*   **Header-Aware Offset:** The `offset` field in each record header is the absolute byte position from the sector start where the record data is located.
*   **Data Spanning:** Record data can cross sector boundaries. When data spans into the next sector, it continues immediately after the next sector's header.
*   **New Sector Opening:** If a record cannot fit in the current sector (not enough space for data + header), a new sector is created with a fresh sector header. The new record is then written to this new sector.

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
*   Writes entries sequentially in circular order, storing data from the current `write_ptr` position.
*   Record headers are appended at the end of the sector (growing downward from the sector end).
1.  **Space Check:** Ensures there is enough room for the record data and header within the current sector.
2.  **Sector Management:** If the record cannot fit in the current sector, it prepares the next one by writing a new sector header with an incremented sequence, then writes the record to the new sector.
3.  **Ordered Program:**
    *   Writes the record data first at `write_ptr`.
    *   Writes the record header at the sector end (with an offset field pointing to the data start).
    *   Updates the `write_ptr` to account for the space used.

### 3.3 Reading Records (`fcb_read`)
*   Reads entries sequentially in circular order starting at `read_ptr`.
*   Continues reading until it meets the `write_ptr` (the end of written data).
*   Validates the record's CRC before returning.

### 3.4 Consuming and Deleting (`fcb_delete`)
*   Marks all records that have been read (between `delete_ptr` and `read_ptr`) as consumed by writing `0x00` to their `consumed` flag in the record header at the end of the sector.
*   Advances `delete_ptr` forward until it meets `read_ptr`, so those records are no longer treated as unconsumed.
*   If `read_ptr` is already at `delete_ptr`, there is nothing to delete (returns `FCB_EMPTY`).

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

> [!IMPORTANT]
> **Header Offset Field:** The `offset` field in each record header specifies the byte position from the sector start where the record data is located. This allows for flexible data layout and efficient record header validation.

> [!IMPORTANT]
> **Record Spanning:** Record data can cross sector boundaries if it extends beyond the current sector. Headers and new sectors are created as needed.

> [!IMPORTANT]
> **New Sector Strategy:** If a record cannot fit in the current sector (including both its data and header), the FCB opens a new sector by writing a fresh sector header and writes the record to the new sector.

> [!IMPORTANT]
> **Thread Safety:** The FCB provides lock/unlock callbacks in its configuration. These should be populated if the FCB will be accessed from multiple threads or interrupts.
