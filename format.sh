#!/bin/bash

echo "Formatting my_diag source files with clang-format..."
find my_diag -maxdepth 1 \( -name "*.c" -o -name "*.h" \) -exec clang-format -i {} +
