#!/bin/bash
# Build script for x-load Docker container

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_DIR"

echo "=========================================="
echo "  X-LOAD Docker Build"
echo "=========================================="

# Parse arguments
BUILD_TARGET="runtime"
NO_CACHE=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --dev)
            BUILD_TARGET="builder"
            shift
            ;;
        --no-cache)
            NO_CACHE="--no-cache"
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  --dev       Build development image (includes build tools)"
            echo "  --no-cache  Build without Docker cache"
            echo "  --help      Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo "Building target: $BUILD_TARGET"
echo ""

# Build the image
if [ "$BUILD_TARGET" = "runtime" ]; then
    docker build $NO_CACHE -t x-load:latest .
    echo ""
    echo "✓ Built x-load:latest (runtime image)"
    echo ""
    echo "Run with:"
    echo "  docker run --privileged -v /dev/hugepages:/dev/hugepages x-load:latest --help"
else
    docker build $NO_CACHE --target builder -t x-load:dev .
    echo ""
    echo "✓ Built x-load:dev (development image)"
    echo ""
    echo "Run interactive shell:"
    echo "  docker run -it --privileged -v /dev/hugepages:/dev/hugepages x-load:dev bash"
fi

