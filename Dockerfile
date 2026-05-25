# Use Ubuntu 22.04 as the base environment
FROM ubuntu:22.04

# Avoid interactive prompts during installation
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    # 1. Process resource analysis
    procps \
    htop \
    lsof \
    strace \
    # 2. File system health check
    e2fsprogs \
    xfsprogs \
    util-linux \
    smartmontools \
    # 3. Network connection status monitoring
    net-tools \
    iproute2 \
    tcpdump \
    iputils-ping \
    netcat-openbsd \
    # Basic development environment and line ending fixes
    build-essential \
    libncurses5-dev \
    bison \
    flex \
    dos2unix \
    clang-format \
    && rm -rf /var/lib/apt/lists/*

# Remove Ubuntu minimal's man stub and dpkg-divert so man-db installs the real binary
RUN dpkg-divert --remove /usr/bin/man && \
    rm -f /usr/bin/man /etc/dpkg/dpkg.cfg.d/excludes && \
    apt-get update && \
    apt-get install -y --reinstall man-db && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /home/project/busybox

# Start Bash by default
CMD ["/bin/bash"]