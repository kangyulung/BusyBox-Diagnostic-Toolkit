#!/bin/bash

# 1. 自動建置環境 Image (如果已存在會跳過)
echo "正在確認開發環境..."
docker build -t busybox-dev-env .

current_path=$(pwd)
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
    # 如果是在 Git Bash 底下，將 /d/path 轉換成 d:/path，Windows Docker 才能看懂
    current_path=$(cygpath -m "$current_path")
fi

# 2. 啟動容器並自動執行編譯初始化
echo "正在進入開發環境並同步代碼..."
docker run -it --privileged \
    -v $(pwd):/home/project/busybox \
    busybox-dev-env \
    bash -c "bash ./compile.sh; bash"