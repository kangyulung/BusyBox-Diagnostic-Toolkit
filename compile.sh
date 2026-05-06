#!/bin/bash

# 1. 移除自動生成的 Config 與 Kbuild (避免 INSERT 殘留)
echo "Cleaning up generated config files..."
rm -f my_diag/Config.in my_diag/Kbuild

# 2. 移除所有物件檔 (.o) 確保重新連結
echo "Removing object files in my_diag..."
#find my_diag -name "*.o" -type f -
# 先確認清單
find my_diag -type f ! -name "*.c" ! -name "*.src" ! -name "*.h" ! -name "*.md" ! -name "*.sh" -delete


echo "Generating build files..."
#make gen_build_files

# 4. 進行配置 (修正原本重複的 make)
# 如果你想重置為預設值，用 defconfig；
# 如果你想手動調整，改用 menuconfig
echo "Configuring busybox..."
make defconfig
# 強制靜態連結（與 dev.sh 保持一致）
sed -i 's/CONFIG_STATIC=n/CONFIG_STATIC=y/' .config
# 非 x86/x86_64 環境（如 ARM64 colima）無法編譯 x86 SHA 硬體加速，自動停用
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