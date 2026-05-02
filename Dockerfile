# 基礎環境使用 Ubuntu 22.04
FROM ubuntu:22.04

# 避免安裝過程中的互動式提問
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    # 1. 行程資源分析
    procps \
    htop \
    lsof \
    strace \
    # 2. 檔案系統健康檢測
    e2fsprogs \
    util-linux \
    smartmontools \
    # 3. 網路連線狀態監控
    net-tools \
    iproute2 \
    tcpdump \
    iputils-ping \
    # 基礎開發環境與換行修正
    build-essential \
    libncurses5-dev \
    bison \
    flex \
    dos2unix \
    && rm -rf /var/lib/apt/lists/*


WORKDIR /home/project/busybox

# 預設啟動 Bash
CMD ["/bin/bash"]