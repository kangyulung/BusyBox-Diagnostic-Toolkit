# Contributing Guide (Contributing to my_diag)

Thank you for considering contributing to the `my_diag` module! To ensure project quality, please follow these guidelines:

## 1. Coding Style
This project follows the official BusyBox development style:
- **Indentation**: Use Tabs or 4 spaces (refer to `vi: set sw=4 ts=4:`).
- **Function Naming**: Use the `diag_` prefix for internal utilities/libraries.
- **Variable Naming**: Keep it concise; use `snake_case` (avoid camelCase).
- **Dependencies**: Use APIs provided by `libbb.h` (e.g., `xmalloc`, `xopen`) instead of standard libc whenever possible.

## 2. Directory Structure and Requirements
- `libdiag.h/c`: Contains shared resource parsing utilities. These can be modified, but ensure backward compatibility with existing features!
- `my_diag`: The project directory. All source files must be placed here.
- **Scope**: Do not modify files outside the `my_diag` directory. This is an official BusyBox source tree where `my_diag` has been **integrated**.
- **Modularity**: New features should be modular; avoid hardcoding logic directly within the `main` function.
- **Line Endings**: Use **LF** (Unix) format; **CRLF** is strictly prohibited.
- **Documentation**: Remember to write `man-pages` and Bash test scripts for new features.
- `Config.src / Kbuild.src`: Configuration and build templates for this directory.
    - **Do not modify the automatically generated `Config.in` or `Kbuild` files directly.**
    - When adding new Applets or source files, ensure the metadata comments (see Section 4) are correct, and run `bash ./compile.sh` in the development environment to update build settings.

## 3. Pull Request Process
1. **Environment Setup**: For first-time setup, run `bash ./dev.sh` from the BusyBox root directory. This creates the dev environment, **mounts** the project, and performs the initial build.
2. **Compilation Test**: Run `bash ./compile.sh` in the development environment and verify that it compiles and runs correctly.
3. **Unit Testing**: If you modify `libdiag.h/c`, ensure it does not break existing functionalities.
4. **No Direct Push**: Even with administrative privileges, do not push directly to the `master` branch.
5. **Feature Branches**: Use branches for all development (e.g., `feature/xxx` or `fix/xxx`).
6. **Create a PR**: Submit a Pull Request (PR) once development is complete.
7. **Review**: Maintainers must review and verify the PR. Merge only after confirmation and successful testing.

## 4. Annotation Standards (Metadata)
Ensure the metadata blocks at the top of your source files (`config`, `usage`, `kbuild`, and `applet`) are correct. These are critical for BusyBox's build system:
- `//config:`: Defines the menu entry.
- `//applet:`: Defines the Applet name and attributes.
- `//usage:`: Defines the command-line help message.
- `//kbuild:`: Defines the compilation rules.

---
*Our mission is to keep the code small, efficient, and memory-leak-free.*