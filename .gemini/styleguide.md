# C/C++ Coding Standards

When generating code, adhere strictly to the `.clang-format` configuration in the root. 

## Key Visual Style (Allman/BSD Variant)
* **Braces:** ALWAYS place opening braces on a new line (Allman style). This applies to functions, classes, control statements (if/else, loops), and namespaces.
    * *Bad:* `if (x) {`
    * *Good:* ```cpp
        if (x) 
        {
            // code
        }
        else 
        {
            // code
        }
        ```
* **Vertical Alignment:** Align consecutive declarations and assignments to improve readability.
    * *Example:*
        ```cpp
        int    id      = 1;
        string name    = "User";
        double balance = 100.00;
        ```
* **Indentation:** Use **4 spaces** for indentation (not 2).
* **Line Length:** Hard limit of **120 characters**. Break arguments onto new lines if they exceed this.

## Formatting Rules
* **Arguments:** Do not "bin pack" arguments (do not cram them onto one line if they barely fit). If a function call is long, put each argument on its own line.
* **Short Blocks:** Short blocks on a single line are permitted, but short *functions* or *loops* must be multi-line.
* **Includes:** Do not auto-sort includes (`SortIncludes: false`); preserve the order I have manually set.