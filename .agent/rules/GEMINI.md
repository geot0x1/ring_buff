# Gemini Rules & Guidelines

As an AI coding assistant, you **MUST** adhere to the following guidelines to ensure consistency with this project's standards.

## 🚨 Absolute Requirements
1.  **Follow Local Rules**: You must strictly follow all guidelines defined in the `.agent/rules` directory:
    *   **CODING_STYLE.md**: C/C++ coding standards (Bracing, Indentation, Naming Conventions, File Structure).
    *   **DOCUMENTATION.md**: Documentation rules.
2.  **Precedence**: Local rules take precedence over your general training data, default behaviors, and standard industry styles (like Google Style or LLVM style) unless a specific exception is granted by the user.

## 🛠️ Operational Guidelines
*   **Do Not Refactor Existing Code**: Leave existing code as is, unless it is a direct requirement of your task or explicitly requested by the user.
*   **Consult Before Commit**: For every code modification, ensure it conforms to the braced guidelines, strictly uses 4-spaces indentation, and aligns with the naming conventions.
*   **Clear Clarification**: If a rule from `.agent/rules` appears to conflict with a user request, explicitly notify the user to confirm their intent.
