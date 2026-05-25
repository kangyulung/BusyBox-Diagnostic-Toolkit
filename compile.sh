#!/bin/bash
# Usage: bash compile.sh [--fmt]
#   --fmt   Run clang-format on my_diag/*.c/*.h before compiling

DO_FORMAT=0
for arg in "$@"; do
    case "$arg" in
        --fmt) DO_FORMAT=1 ;;
        *) echo "Unknown parameter: $arg"; exit 1 ;;
    esac
done

# 1. Remove auto-generated Config and Kbuild (to prevent INSERT residue)
echo "Cleaning up generated config files..."
rm -f my_diag/Config.in my_diag/Kbuild

# 2. Remove all object files (.o) to ensure relinking
echo "Removing object files in my_diag..."
#find my_diag -name "*.o" -type f -
# First confirm the list of files to delete
find my_diag -type f ! -name "*.c" ! -name "*.src" ! -name "*.h" ! -name "*.md" ! -name "*.sh" ! -name "*.1" ! -name ".clang-format" -delete


# 3. Format my_diag source code (executes only if --fmt is passed)
if [ "$DO_FORMAT" -eq 1 ]; then
    bash "$(dirname "$0")/format.sh"
fi

echo "Generating build files..."
#make gen_build_files

# 4. Configure (fix the originally duplicated make)
# If you want to reset to default, use defconfig;
# If you want to adjust manually, use menuconfig instead
echo "Configuring busybox..."
make defconfig
# Force static linking (keep consistent with dev.sh)
sed -i 's/CONFIG_STATIC=n/CONFIG_STATIC=y/' .config
# Non-x86/x86_64 environments (like ARM64 colima) cannot compile x86 SHA hardware acceleration, auto-disable
case "$(uname -m)" in
    i386|i686|x86_64) ;;
    *)
        sed -i 's/CONFIG_SHA1_HWACCEL=y/# CONFIG_SHA1_HWACCEL is not set/' .config
        sed -i 's/CONFIG_SHA256_HWACCEL=y/# CONFIG_SHA256_HWACCEL is not set/' .config
        ;;
esac

echo "Starting build with all CPU cores..."
make -j$(nproc)

if [ $? -eq 0 ]; then
    echo "---------------------------------------"
    echo "Build Successful!"
    echo "---------------------------------------"
else
    echo "Build Failed! Please check the errors above."
fi