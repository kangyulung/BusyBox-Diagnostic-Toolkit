#!/bin/bash

# 1. Auto-build the environment image (skipped if it already exists)
echo "Checking development environment..."
docker build -t busybox-dev-env .

current_path=$(pwd)
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
    # If running under Git Bash, convert /d/path to d:/path so Windows Docker can understand it
    current_path=$(cygpath -m "$current_path")
fi

# 2. Start the container and automatically run the compilation initialization
echo "Entering the development environment and syncing code..."
docker run -it --privileged \
    -v $(pwd):/home/project/busybox \
    busybox-dev-env \
    bash -c "bash ./compile.sh; bash"