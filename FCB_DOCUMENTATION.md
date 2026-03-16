# Flash Circular Buffer (FCB) Documentation

The Flash Circular Buffer (FCB) is a robust, power-fail-safe, circular FIFO implementation specifically designed for SPI NOR flash memory. It provides a reliable way to store sequential records (e.g., logs, events, telemetry) where data integrity and recovery from unexpected power loss are critical.

## 1. Design Philosophy

### 1.1 Power-Fail Safety
The FCB is designed to ensure that the buffer state remains recoverable at any point, even if power is lost during a write or erase operation.
*   **Ordered Writes:** Record data is written first, followed by the record header at the end of the sector. Sector headers are written before any records in that sector. This allows the recovery process to identify partially written or corrupted data.
*   **State Transitions:** Sector status transitions from erased (0xFF) → valid (0xAA) → consumed (0x00) only clear bits, exploiting NOR flash's write-once property.
*   **Atomic Operations:** The "consumed" flag in record headers transitions from `0xFF` to `0x00`, which is an atomic operation on NOR flash.
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
| 8 | 1 | `status` | `0xFF` (erased), `0xAA` (valid/active), or `0x00` (consumed). |
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
*   **Record Walk Order:** When walking/scanning FCB records within a sector, start from the top (lowest address) of the record header region and advance toward the sector end. This ensures records are encountered in data write order.
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
Upon initialization, the FCB performs a full recovery scan with two separate walks:

**Find read_ptr (tail):**
1.  **Scan Sector Headers:** Finds all valid FCB sectors and identifies the oldest/newest based on sequence numbers.
2.  **Walk from Oldest Sector:** Starting from the oldest sector, the recovery process scans the FCB records stored at the end of each sector (growing downward from sector end). For each sector, it walks through all record headers from the top (lowest address of the header region) to find each record sequentially, validating each header.
3.  **Skip Consumed Records:** During the record walk, consumed records (consumed flag = `0x00`) are skipped. Only unread records (consumed flag = `0xFF`) are considered as candidates for `read_ptr`.
4.  **Find First Unread:** The process identifies the first record with valid magic (`0xFCBA`), valid CRC-8, and `consumed` flag set to `0xFF` (unread). This becomes the initial `read_ptr`.
5.  **Handle Fragmented Records:** If a record is found to be fragmented or invalid, the algorithm continues reading the next record header to check if there is a valid record following it. Invalid records are skipped in the search for the first unread record.
6.  **All-Consumed Sector:** If the oldest sector contains only consumed records (no unread records found), mark the sector status as `0x00` (consumed) and move to the next sector to continue searching for the first unread record.

**Find write_ptr (head):**
1.  **Scan from Newest Sector:** Starting from the newest sector (highest sequence number), the recovery process walks the FCB records at the end of the sector.
2.  **Find Latest Valid Record:** Scans through record headers from the bottom (highest address in the header region) moving toward the top (lowest address), to locate the most recently written valid record header (valid magic `0xFCBA`, valid CRC-8).
3.  **Position at Next Byte:** Positions `write_ptr` at the next available byte after the latest valid record, which is the erased space where the next record will be written.

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
*   To locate records: begins at the oldest sector and walks the FCB records at the top of the record header region, scanning from lowest to highest address within each sector, until it finds the record at `read_ptr`.
*   Continues reading records sequentially through the circular buffer until it reaches `write_ptr` (the end of written data).
*   Each record must have: valid magic (`0xFCBA`), valid CRC-8, and `consumed` flag = `0xFF` (unread).
*   Validates the record's CRC and magic before returning the data.

### 3.4 Consuming and Deleting (`fcb_delete`)
*   Marks all records that have been read (between `delete_ptr` and `read_ptr`) as consumed by writing `0x00` to their `consumed` flag in the record header at the end of the sector.
*   Advances `delete_ptr` forward until it meets `read_ptr`, so those records are no longer treated as unconsumed.
*   If `read_ptr` is already at `delete_ptr`, there is nothing to delete (returns `FCB_EMPTY`).

### 3.5 Sector Trim (`fcb_trim`)
*   Erases the oldest sector, but only if all records within it are marked as consumed.
*   **Mark as Consumed:** When all records in the sector are consumed, the sector status flag is written to `0x00` to mark the entire sector as consumed before erasure.
*   **Pointer Advancement:** If `read_ptr` or `delete_ptr` are pointing into the sector being erased, they are automatically advanced to point at the first valid FCB record in the next sector. This ensures these pointers remain valid after the sector is erased.

## 4. API Reference

| Function | Description |
| :--- | :--- |
| `fcb_init` | Mounts the FCB and performs recovery scan. |
| `fcb_write` | Appends a new record to the buffer. |
| `fcb_read` | Peeks at the oldest unconsumed record. |
| `fcb_delete` | Marks the oldest unconsumed record as consumed. |
| `fcb_trim` | Erases the oldest sector if fully consumed. |
| `fcb_is_full` | Checks if the buffer has room for a max-size record. |
| `fcb_is_empty` | Checks if there are any unconsumed records. |

## 5. Implementation Notes & Limitations

### Sector Status Lifecycle
The sector status field transitions through states following NOR flash's one-way bit clearing property:
```
0xFF (Erased) 
  ↓ [write sector header]
0xAA (Valid/Active) 
  ↓ [mark all records consumed]
0x00 (Consumed) 
  ↓ [erase sector]
0xFF (Erased)
```

Each transition represents clearing additional bits (1→0 transitions only on NOR flash). The consumed status (`0x00`) is marked before erasure as an optimization flag and recovery aid, allowing the system to quickly identify sectors that contain only fully-consumed data.

> [!IMPORTANT]
> **Header Offset Field:** The `offset` field in each record header specifies the byte position from the sector start where the record data is located. This allows for flexible data layout and efficient record header validation.

> [!IMPORTANT]
> **Record Spanning:** Record data can cross sector boundaries if it extends beyond the current sector. Headers and new sectors are created as needed.

> [!IMPORTANT]
> **New Sector Strategy:** If a record cannot fit in the current sector (including both its data and header), the FCB opens a new sector by writing a fresh sector header and writes the record to the new sector.

> [!IMPORTANT]
> **Thread Safety:** The FCB provides lock/unlock callbacks in its configuration. These should be populated if the FCB will be accessed from multiple threads or interrupts.
