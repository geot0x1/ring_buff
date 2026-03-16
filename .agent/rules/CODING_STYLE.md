---
trigger: always_on
---

# C/C++ Coding Standards

> [!NOTE]
> Existing code that does not follow these coding style rules should be left as is.

## Bracing Style
* **Always** put both braces (opening {, closing }) on a new line.  
* **Mandatory Braces:** Braces must be used for all statements (`if`, `else`, `for`, `while`, `do`) etc, even for single-line statements.

## Indentation Style
Use 4 spaces per indent level.


Do not use tabs.

## Naming Conventions
* **User types** (structs, enums, unions, typedefs): `PascalCase` (e.g., `MyType`). Note: `_t` suffix is **not** permitted.
* **Global objects** (project scope): `camelCase` (e.g., `myGlobalVariable`)
* **Static local objects and function scoped variables**: `snake_case` (e.g., `static_local_var`, `local_var`)
* **Functions**: `snake_case` (e.g., `my_function_name`)

## File Structure and Order
Source files should follow this organization:
1. Include lists first
2. Definitions (`#define`)
3. Typedefs
4. Function declarations (public/external functions)
5. Project globals
6. Static globals
7. **Static function declarations** (forward declarations of all static functions, placed at the top of the implementation section)
8. Function implementations
