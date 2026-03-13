---
trigger: always_on
---

# C/C++ Coding Standards

## Bracing Style
* **Always** use **Allman style** bracing (braces on a new line).
* **Mandatory Braces:** Braces must be used for all control structures (`if`, `else`, `for`, `while`, `do`), even for single-line statements.
* Do not use K&R style (braces on the same line).

## Examples
### Allman & Mandatory Braces
```cpp
// Good
if (condition) 
{
    do_something();
}

// Good (Single-line logic still requires braces)
if (condition)
{
    return;
}

// Bad (K&R style)
if (condition) {
    do_something();
}

// Bad (Missing braces for single line)
if (condition)
    return;

Indentation Style
Use 4 spaces per indent level.

Do not use tabs.