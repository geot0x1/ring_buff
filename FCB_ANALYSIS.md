# FCB (Flash Circular Buffer) Code Analysis

## Executive Summary

The FCB implementation has **one critical bug and several collision detection logic flaws** that can cause buffer state corruption and false full/empty conditions.

---

## Part 1: Pointer Initialization & Advancement

### 1.1 How Pointers Are Initialized (fcb_init)

**Fresh/Empty Flash Case [Lines 584-656]:**

```c
// All three pointers start at the same position (empty buffer)
fcb->next_sequence = 1;
write_sector_header(fcb, 0, 0);

fcb->delete_ptr_sector = 0;
fcb->delete_ptr_offset = FCB_SECTOR_HDR_SIZE;      // = 16

fcb->read_ptr_sector   = 0;
fcb->read_ptr_offset   = FCB_SECTOR_HDR_SIZE;      // = 16

fcb->write_ptr_sector  = 0;
fcb->write_ptr_offset  = FCB_SECTOR_HDR_SIZE;      // = 16
```

**Initial State:** All three pointers point to sector 0, offset 16 (immediately after sector header). Buffer is EMPTY.

### 1.2 How Write Pointer Advances

**In fcb_write() [Lines 839-868]:**

After writing a record header and data:

```c
// Save original position
uint8_t  wr_sector = fcb->write_ptr_sector;
uint32_t wr_offset = fcb->write_ptr_offset + FCB_RECORD_HDR_SIZE;  // Skip 12 bytes

// If header doesn't fit in sector, wrap
if (wr_offset >= fcb->config.sector_size)
{
    wr_sector = next_sector(fcb, wr_sector);
    wr_offset = FCB_SECTOR_HDR_SIZE;  // Reset to after sector header
}

// program_record_data() then advances wr_offset by payload length
rc = program_record_data(fcb, &wr_sector, &wr_offset, data, (uint16_t)len);

// Update the actual write pointer to position AFTER all data
fcb->write_ptr_sector = wr_sector;
fcb->write_ptr_offset = wr_offset;
```

**Result:** write_ptr always points to the next free byte (where next record will be written).

### 1.3 How Delete & Read Pointers Advance

**In fcb_delete() [Lines 1167-1215]:**

```c
// Save the OLD position of the record being deleted
uint8_t  old_delete_sec = fcb->delete_ptr_sector;
uint32_t old_delete_off = fcb->delete_ptr_offset;

// Advance past the deleted record
uint8_t  new_delete_sec;
uint32_t new_delete_off;
advance_past_record(fcb, fcb->delete_ptr_sector, fcb->delete_ptr_offset,
                    rhdr.length, &new_delete_sec, &new_delete_off);

fcb->delete_ptr_sector = new_delete_sec;
fcb->delete_ptr_offset = new_delete_off;

// If read_ptr pointed to the same record that was deleted,
// and it hasn't been read yet, advance read_ptr too
if (fcb->read_ptr_sector == old_delete_sec &&
    fcb->read_ptr_offset == old_delete_off)
{
    // Skip consumed records in loop...
}
```

**Result:** delete_ptr and read_ptr advance together (in normal flow).

---

## Part 2: Pointer Collision Detection States

### 2.1 Buffer Empty State

```
When: write_ptr == read_ptr == delete_ptr
Example: (0, 16) == (0, 16) == (0, 16)
Meaning: No unconsumed records exist
Free space: Up to (num_sectors - 1) × sector_size
```

### 2.2 Buffer Full State (Theoretical)

```
When: write_ptr catches up to delete_ptr with no wrapping room
Example: write_ptr=(1, 65520) and delete_ptr=(0, 16), can't fit another sector
Meaning: No more writable space available
```

### 2.3 Buffer Near-Full (Wrapping)

```
When: write_ptr in sector N, delete_ptr in sector N
Example: write_ptr=(1, 50000) and delete_ptr=(1, 1000)
Meaning: Both in same sector, trying to wrap to next sector
Critical: COLLISION CHECK HAPPENS HERE
```

---

## Part 3: State Machine - Filling until Full

### Scenario: 2-Sector Buffer, 65536 bytes/sector

**Constants:**
- Sector header: 16 bytes
- Record header: 12 bytes
- Each 1-byte record: 12 + 1 = 13 bytes total
- Usable per sector: 65536 - 16 = 65520 bytes

### Step-by-Step Trace

#### Phase 1: Fill Sector 0

| Record # | write_ptr | Space Left | Status |
|----------|-----------|------------|--------|
| 0 | (0, 16) | 65520 | Empty |
| 1 | (0, 29) | 65507 | First record written |
| 2 | (0, 42) | 65494 | Second record |
| ... | ... | ... | ... |
| 5038 | (0, 65521) | 15 | Almost full |
| 5039 | (0, 65534) | 2 | Only 2 bytes left |

#### Phase 2: Sector 0 Full, Move to Sector 1

**Attempting to write record 5040:**
- write_ptr = (0, 65534)
- remaining_in_sector() = 65536 - 65534 = 2 bytes
- FCB_RECORD_HDR_SIZE = 12 bytes
- 2 < 12 → **Header won't fit in sector 0**

**Lines 777-784 in fcb_write():**
```c
uint32_t write_remain = remaining_in_sector(fcb, fcb->write_ptr_offset);

if (write_remain < FCB_RECORD_HDR_SIZE)  // 2 < 12 = TRUE
{
    /* Not enough room for a record header — move to next sector. */
    rc = prepare_next_sector(fcb);
    // ...
    write_remain = remaining_in_sector(fcb, fcb->write_ptr_offset);
}
```

**Inside prepare_next_sector() [Lines 513-545]:**

**CRITICAL CHECK AT LINE 519:**
```c
uint8_t ns = next_sector(fcb, fcb->write_ptr_sector);  // ns = 1

if (ns == fcb->delete_ptr_sector && fcb->write_ptr_sector != fcb->delete_ptr_sector)
{
    return FCB_FULL;
}
// Current state: ns=1, delete_ptr_sector=0, write_ptr_sector=0
// Condition: (1 == 0) && (0 != 0) → FALSE && TRUE = FALSE
// CHECK PASSES - sector 1 is used
```

**Lines 523-536: Check if sector 1 needs erasing:**
```c
fcb_sector_hdr_t shdr;
int rc = read_sector_header(fcb, ns, &shdr);

// If already has a valid sector header with magic/status, can't reuse
if (shdr.magic == FCB_SECTOR_MAGIC && shdr.status == FCB_SECTOR_STATUS_VALID)
{
    return FCB_FULL;  // Sector still in use
}

// Write new sector header to sector 1
rc = write_sector_header(fcb, ns, fcb->next_sequence);
fcb->next_sequence++;
fcb->write_ptr_sector = 1;
fcb->write_ptr_offset = FCB_SECTOR_HDR_SIZE;  // Reset to 16
```

**Result of prepare_next_sector():**
- write_ptr = (1, 16)
- Can now write record in sector 1

#### Phase 3: Fill Sector 1

Same as Phase 1, record by record in sector 1 until:
- write_ptr = (1, 65534)
- remaining = 2 bytes

#### Phase 4: Trying to Move Beyond Sector 1

**Attempting record after filling sector 1:**
- write_ptr = (1, 65534)
- remaining = 2 bytes

**In prepare_next_sector():**
```c
uint8_t ns = next_sector(fcb, fcb->write_ptr_sector);  // ns = 0 (wraps)

if (ns == fcb->delete_ptr_sector && fcb->write_ptr_sector != fcb->delete_ptr_sector)
{
    return FCB_FULL;
}
// Current state: ns=0, delete_ptr_sector=0, write_ptr_sector=1
// Condition: (0 == 0) && (1 != 0) → TRUE && TRUE = TRUE
// COLLISION DETECTED - sector 0 contains unread data
// Returns FCB_FULL ✓ CORRECT
```

### Expected Behavior Summary

| Phase | write_ptr | delete_ptr | Result |
|-------|-----------|------------|--------|
| Empty | (0, 16) | (0, 16) | Can write to sector 0 |
| End S0 | (0, 65534) | (0, 16) | Move to sector 1 |
| End S1 | (1, 65534) | (0, 16) | Try wrap → COLLISION → FCB_FULL ✓ |

---

## Part 4: CRITICAL BUG - prepare_next_sector()

### The Bug [Line 519 in fcb.c]

**Current Code:**
```c
if (ns == fcb->delete_ptr_sector && fcb->write_ptr_sector != fcb->delete_ptr_sector)
{
    return FCB_FULL;
}
```

**Issue:**
The condition has an EXTRA requirement: `fcb->write_ptr_sector != fcb->delete_ptr_sector`

This means the collision check is SKIPPED when:
- Next sector would collide with delete_ptr sector, BUT
- write_ptr and delete_ptr are ALREADY in the same sector

### When This Fails

**Scenario: Both pointers in same sector, preparing to wrap**

```
State:
- write_ptr = (1, 50000)
- delete_ptr = (1, 40000)
- Both in sector 1

Try to write new record:
- Remaining in sector 1 too small
- ns = next_sector(1) = 0
- delete_ptr_sector = 1

Check: if (0 == 1 && 1 != 1)
     → if (FALSE && FALSE)
     → FALSE - check passes!

Result: Proceeds to overwrite sector 0, which might contain consumed records
        that haven't been deleted yet, OR future data block
```

### Comparison with fcb_write() Spanning Logic [Lines 808-817]

**fcb_write() has the SAME pattern:**
```c
uint8_t ns = next_sector(fcb, fcb->write_ptr_sector);

if (ns == fcb->delete_ptr_sector && fcb->write_ptr_sector != fcb->delete_ptr_sector)
{
    /* Would overwrite unread data. */
    fcb_unlock(fcb);
    return FCB_FULL;
}
```

**BUT** in fcb_write(), it's used as a pre-check BEFORE calling prepare_next_sector().
- If spanning check passes, prepare_next_sector() is called
- But prepare_next_sector() has the SAME check again (redundant)
- The redundant check creates the vulnerability

### Free Space Calculation - Hidden Issue [Lines 347-389]

The `free_space()` function has complex branching logic for calculating available space:

```c
static uint32_t free_space(const fcb_t *fcb)
{
    // ... complex logic to calculate free space ...
    // Uses multiple branches based on sector ordering
}
```

This is NOT called by any public API function and is NOT used in the fullness check.  
The check relies solely on:
- `fcb_is_full()` [Lines 1361-1410]
- `prepare_next_sector()` collision check [Line 519]
- `fcb_write()` spanning check [Line 808]

---

## Part 5: Issue #2 - fcb_is_full() Condition

**Location:** Lines 1379-1388 in fcb.c

**Current Code:**
```c
if (ns == fcb->delete_ptr_sector && fcb->write_ptr_sector != fcb->delete_ptr_sector)
{
    fcb_unlock((fcb_t *)fcb);
    return true;  /* Blocked by delete_ptr - full. */
}
```

**Same Issue:** Extra condition `fcb->write_ptr_sector != fcb->delete_ptr_sector` causes collision check to fail when both pointers in same sector.

**Also**:
- fcb_is_full() is called AFTER attempting prepare_next_sector()
- It's NOT called BEFORE in fcb_write()
- So fcb_write() can bypass it

---

## Part 6: Issue #3 - No Pre-check in fcb_write() for Non-Spanning Writes

**Lines 777-784 in fcb_write():**

```c
if (write_remain < FCB_RECORD_HDR_SIZE)
{
    /* Not enough room for a record header — move to next sector. */
    rc = prepare_next_sector(fcb);
    // ...
}
```

**Gap:** This PREPARES the next sector but doesn't return FCB_FULL if prepare_next_sector() fails.

Wait, looking at the code again [Lines 777-785]:
```c
if (write_remain < FCB_RECORD_HDR_SIZE)
{
    /* Not enough room for a record header — move to next sector. */
    rc = prepare_next_sector(fcb);
    if (rc != FCB_OK)
    {
        fcb_unlock(fcb);
        return rc;  // ✓ Returns error (including FCB_FULL)
    }
```

This is CORRECT - error is propagated.

---

## Part 7: Issue #4 - Wrap-Around WITHOUT Deletion

**Scenario: Fill both sectors, NO records deleted**

```
After filling sectors 0 and 1:
- write_ptr = (1, 65534)
- read_ptr = (0, 16)      <- Still pointing to first record in sector 0
- delete_ptr = (0, 16)    <- Same as read_ptr

Try to write one more record:
- prepare_next_sector() calculates ns = 0
- Check: (0 == 0) && (1 != 0) → TRUE
- Returns FCB_FULL ✓ CORRECT

Now delete some records in sector 0:
- fcb_delete() marks records as consumed
- delete_ptr advances within sector 0 to (0, 1000)
- read_ptr might be at (0, 1000) too

Now try to write again:
- write_ptr = (1, 65535)
- prepare_next_sector() calculates ns = 0
- delete_ptr_sector = 0
- write_ptr_sector = 1
- Check: (0 == 0) && (1 != 0) → TRUE
- Returns FCB_FULL ✓ STILL CORRECT

When WOULD the condition fail?
- write_ptr = (1, 65535)
- delete_ptr = (1, 500)    <- Wrapped to sector 1
- prepare_next_sector() calculates ns = 0
- Check: (0 == 1) && (1 != 1) → (FALSE) && (FALSE) = FALSE
- Falls through and tries to write sector 0!
- **This is a collision - sector 0 now being reused while writes in progress**
```

---

## Summary of Issues

| Issue | Location | Problem | Impact |
|-------|----------|---------|--------|
| **#1 - Primary Bug** | Line 519 in prepare_next_sector() | Extra condition `&& fcb->write_ptr_sector != fcb->delete_ptr_sector` allows collision when both pointers in same sector | Buffer corruption when write_ptr and delete_ptr wrap to same sector |
| **#2** | Line 1379 in fcb_is_full() | Same extra condition on collision check | Returns false positive (buffer not full when it is) |
| **#3** | Lines 808 in fcb_write() | Redundant check with prepare_next_sector() | Spanning records might pass both checks but still collide with delete sector |
| **#4** | Overall design | No atomic check-then-write sequence | Race condition possible between fullness check and actual write |

---

## Recommended Fixes

1. **Remove the extra condition in prepare_next_sector() [Line 519]:**
   ```c
   // BEFORE:
   if (ns == fcb->delete_ptr_sector && fcb->write_ptr_sector != fcb->delete_ptr_sector)
   
   // AFTER:
   if (ns == fcb->delete_ptr_sector)
   ```

2. **Do the same in fcb_is_full() [Line 1379]:**
   ```c
   // BEFORE:
   if (ns == fcb->delete_ptr_sector && fcb->write_ptr_sector != fcb->delete_ptr_sector)
   
   // AFTER:
   if (ns == fcb->delete_ptr_sector)
   ```

3. **Consider:** The check in fcb_write() [Line 808] might become redundant after fixing prepare_next_sector(), but keep it for defense-in-depth.

