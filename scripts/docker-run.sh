#!/bin/bash
# Run script for x-load Docker container

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Check if hugepages are configured
HUGEPAGES=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo "0")
if [ "$HUGEPAGES" -lt 256 ]; then
    echo "WARNING: Hugepages may not be configured (currently: $HUGEPAGES)"
    echo "SPDK requires hugepages. Configure with:"
    echo "  echo 1024 | sudo tee /proc/sys/vm/nr_hugepages"
    echo ""
    read -p "Continue anyway? [y/N] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Create results directory
mkdir -p "$PROJECT_DIR/results"

# Run the container
docker run --rm \
    --privileged \
    --cap-add=SYS_ADMIN \
    --cap-add=IPC_LOCK \
    --cap-add=NET_ADMIN \
    --shm-size=4g \
    --ulimit memlock=-1:-1 \
    -v /dev/hugepages:/dev/hugepages \
    -v "$PROJECT_DIR/results:/app/results" \
    --network host \
    x-load:latest \
    "$@"

