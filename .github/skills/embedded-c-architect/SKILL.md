---
name: embedded-c-architect
description: "WORKFLOW SKILL — Translate high-level embedded requirements into modular, reliable, bare-metal C code with clear hardware/constraint boundaries. Use when designing drivers, RTOS-free firmware, or low-level platform modules."
---

# Embedded C Architect

This skill turns vague embedded feature requests into a concrete, hardware-aware implementation plan and a deliverable C module (headers + sources + tests + docs). It is not a simple "write a function" prompt: it guides the agent through the design decisions required for safe, portable, and maintainable bare-metal code.

## When to Use
- Building a new peripheral driver, system component, or low-level service (timers, DMA, flash, UART, SPI, etc.)
- Porting code between MCUs, toolchains, or memory maps
- Refactoring unsafe C into a structured, module-based implementation
- Writing firmware for no-OS systems where every byte and cycle counts

## What This Skill Produces
- A **module design** (public API + internal architecture + portability hooks)
- A **header file** with config structs, enums, error codes, and stable API
- A **C implementation** with clear separation of hardware access and logic
- A **test plan** (unit tests + integration / hardware test cases)
- A **documentation summary** (assumptions, constraints, known limitations)

---

## Workflow (Modular Code Architect)

### 1) Capture the Requirements (Inputs)
1. Identify the **target platform** (MCU family, core, toolchain, linker script, RAM/Flash sizes).
2. List the **hardware peripherals** and resources used (pins, timers, DMA, interrupts).
3. Clarify the **behavior** and **success criteria** (throughput, latency, error handling, power modes).
4. Determine non-functional constraints:
   - Memory (stack/heap usage, buffer sizes)
   - Timing (hard real-time deadlines, jitter bounds)
   - Safety/security (fault isolation, safe defaults)

> **Tip:** Ask the user for a minimal system diagram or memory map if not provided.

### 2) Define the Module Boundary (API & Ownership)
1. Decide what the module owns (buffers? hardware resources? global state?).
2. Choose a **configuration model** (compile-time macros + structs vs runtime init).
3. Define the public API surface: init/deinit, start/stop, read/write, status.
4. Specify **failure modes** and return codes (use `enum` or `typedef` for `error_t`).

### 3) Design for Portability and Safety
1. Use fixed-width integer types (`uint8_t`, `int32_t`, etc.) and avoid implicit casts.
2. Isolate **hardware access** behind a portability layer (register maps, HAL macros).
3. Avoid dynamic memory (`malloc/free`) unless explicitly allowed; prefer static or caller-provided buffers.
4. Account for concurrency:
   - Is the module used from ISRs and normal code?
   - Use `volatile`, `atomic`, or critical sections as needed.
5. Document assumptions (interrupt priorities, reentrancy, stack sizes).

### 4) Implement in Layers
1. **Platform layer** (optional): `platform_*` or `hw_*` functions/macros that abstract registers and peripherals.
2. **Core logic layer**: state machines, data formatting, error checking.
3. **Public API layer**: validated input, error mapping, and stable interface.

> **Quality criteria:** Each C file should have a single responsibility; avoid mixing ISR-level code with high-level logic.

### 5) Plan and Write Tests
1. Create **unit tests** with a host-side harness (Unity, CMock, etc.) by stubbing hardware access.
2. Identify **integration tests** for real hardware, including edge cases (power loss, bus contention).
3. Define **acceptance criteria**: expected traces, register values, behavior under stress.

### 6) Document and Review
1. Write a short **README** / doc section describing:
   - What the module provides
   - Required hardware setup (pins, clocks, interrupts)
   - Configuration steps and typical usage
   - Known limitations and gotchas
2. Run static analysis / linting (MISRA, cppcheck, etc.) where applicable.
3. Validate against the development board/SoC with a simple example.

---

## Example Prompts to Try
- "/embedded-c-architect: Design a robust SPI master driver for STM32F1 using DMA and ISR-driven transfer completion."
- "/embedded-c-architect: Create a flash circular buffer module with wear leveling and power-loss safety for a small MCU."
- "/embedded-c-architect: Refactor this timing-critical sensor sampling loop into a modular, testable driver."

---

## Next Customizations (Optional)
- Add a matching `*.prompt.md` for quick, single-step requests (e.g., `embedded-c-architect.prompt.md`).
- Add a `*.instructions.md` file that enforces code style and safety rules for this repo (MISRA, naming, etc.).
- Add CI hooks to run unit tests and static analysis on each PR.
