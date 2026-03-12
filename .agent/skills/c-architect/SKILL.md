---
name: c-code-architect
description: Automatically triggers when the user provides C source code or asks to document a C program. Use this to generate professional, memory-aware Markdown documentation.
---

# Goal
To provide high-quality documentation for C programs, focusing on memory safety, pointer logic, and module architecture.

# Instructions
Whenever C code is detected or documentation is requested:
1. **Analyze** the `#include` headers and explain their purpose.
2. **Document** every function with its prototype, parameter roles (Input/Output), and return values.
3. **Highlight** all memory allocations (`malloc`, `calloc`, `realloc`) and verify matching `free` calls.
4. **Output** the result in structured Markdown using the "C-Code Architect" branding.

# Constraints
- Do not truncate code snippets.
- Use LaTeX for algorithmic complexity (e.g., $O(n)$).
- Always warn if a pointer is used without a NULL check.