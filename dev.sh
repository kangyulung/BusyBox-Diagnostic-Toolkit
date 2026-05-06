#!/bin/bash

# 1. 自動建置環境 Image (如果已存在會跳過)
echo "正在確認開發環境..."
docker build -t busybox-dev-env .

# 2. 啟動容器並自動執行編譯初始化
echo "正在進入開發環境並同步代碼..."
docker run -it --privileged \
    -v $(pwd):/home/project/busybox \
    busybox-dev-env \
    bash -c "bash ./compile.sh; bash"