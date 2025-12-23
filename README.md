# X-LOAD: XCOPY High-Concurrency Load Generator

A high-performance NVMe initiator client designed to achieve absolute saturation of an NVMe-over-TCP (NVMe/TCP) storage target using exclusively the NVMe Copy Command (Opcode 0x19, XCOPY/SCC). Built with SPDK for kernel bypass and poll-mode operation.

## Features

- **SPDK-Based Architecture**: Uses Storage Performance Development Kit (SPDK) for kernel bypass and user-space NVMe/TCP driver
- **Poll-Mode Operation**: Lockless, non-interrupt-driven I/O with dedicated CPU core polling threads
- **High Concurrency**: Configurable I/O depth and CPU core count for maximum target saturation
- **Multi-Path Support**: Connect to multiple target IPs simultaneously for increased throughput
- **Dynamic XCOPY Complexity**: Randomized number of source range descriptors per command (1 to max_ranges)
- **Cross-Namespace Copy**: Supports TP4130 cross-namespace copy with randomized source NSIDs
- **Live Monitoring**: Real-time performance display with IOPS, throughput, and latency statistics
- **Comprehensive Statistics**: High-resolution latency measurements with P99, P99.9, P99.99 percentiles
- **Backpressure Resilience**: Graceful handling of saturation errors (0x807, 0x189) without blocking
- **JSON Output**: Optional structured JSON output for automated reporting

## Quick Start with Docker

### Prerequisites

Configure hugepages on the host:

```bash
# Allocate 2MB hugepages (1024 pages = 2GB)
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages

# Verify hugepages
cat /proc/meminfo | grep HugePages
```

### Option 1: Development Image (Recommended for Testing)

The dev image includes a shell for debugging and iterative testing:

```bash
# Build the development image
docker build -f Dockerfile.dev -t x-load:dev .

# Run a test
docker run --rm --privileged -v /dev/hugepages:/dev/hugepages x-load:dev ./x-load \
  --traddr 192.168.1.100 \
  --trsvcid 4420 \
  --hostnqn nqn.2014-08.org.nvmexpress:myhost \
  --subnqn nqn.2024-08.com.example:subsystem \
  --dst-nsid 1 \
  --runtime 60 \
  --num-cores 8 \
  --iodepth 256 \
  --range-size 8192
```

### Option 2: Production Image (Smaller, Optimized)

The production image is ~3x smaller with no development tools:

```bash
# Build the production image
docker build -t x-load:prod .

# Run a test (no ./x-load prefix needed - uses ENTRYPOINT)
docker run --rm --privileged -v /dev/hugepages:/dev/hugepages x-load:prod \
  --traddr 192.168.1.100 \
  --trsvcid 4420 \
  --hostnqn nqn.2014-08.org.nvmexpress:myhost \
  --subnqn nqn.2024-08.com.example:subsystem \
  --dst-nsid 1 \
  --runtime 60 \
  --num-cores 8 \
  --iodepth 256 \
  --range-size 8192
```

### Multi-Path Example (Multiple IPs)

Connect to the same subsystem via multiple IPs for increased throughput:

```bash
docker run --rm --privileged -v /dev/hugepages:/dev/hugepages x-load:dev ./x-load \
  --traddr 192.168.1.100 \
  --traddr 192.168.1.101 \
  --traddr 192.168.1.102 \
  --traddr 192.168.1.103 \
  --trsvcid 4420 \
  --hostnqn nqn.2014-08.org.nvmexpress:myhost \
  --subnqn nqn.2024-08.com.example:subsystem \
  --dst-nsid 1 \
  --runtime 300 \
  --num-cores 16 \
  --iodepth 256 \
  --range-size 8192
```

### Interactive Shell (Development)

```bash
docker run -it --rm --privileged \
  -v /dev/hugepages:/dev/hugepages \
  -v $(pwd):/app \
  x-load:dev bash
```

## Command-Line Options

### Required

| Option | Description |
|--------|-------------|
| `--traddr ADDR` | Target IP address (can be specified multiple times for multi-path) |

### Connection

| Option | Default | Description |
|--------|---------|-------------|
| `--trsvcid PORT` | 4420 | NVMe-oF service port |
| `--hostnqn NQN` | auto | Host NQN identifier |
| `--subnqn NQN` | (empty) | Subsystem NQN |

### Workload

| Option | Default | Description |
|--------|---------|-------------|
| `--runtime SEC` | 10 | Runtime duration in seconds (0 = infinite) |
| `--iodepth DEPTH` | 64 | Maximum global I/O depth |
| `--num-cores CORES` | 1 | Number of worker threads/QPairs |
| `--range-size BLOCKS` | 2048 | Blocks per XCOPY range (max: 8192 recommended) |
| `--max-ranges NUM` | 1 | Source ranges per command (1-16) |

### Namespace Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `--dst-nsid NSID` | 1 | Destination namespace ID |
| `--src-nsid NSID` | (same as dst) | Source namespace ID (can be specified multiple times) |
| `--dst-lba-start LBA` | 0 | Starting LBA for destination |
| `--dst-lba-end LBA` | 0 | Ending LBA for destination (0 = use namespace size) |

### Output

| Option | Description |
|--------|-------------|
| `--json` | Output statistics in JSON format |
| `-v, --verbose` | Enable verbose debug output |
| `-h, --help` | Show help message |

## Live Monitor Output

During execution, X-LOAD displays real-time statistics:

```
[00:15] IOPS: 45,230 | BW: 176.68 MB/s | Lat(avg): 178.2 µs | Outstanding: 256 | Err: 0
```

| Field | Description |
|-------|-------------|
| `[MM:SS]` | Elapsed time |
| `IOPS` | Operations per second |
| `BW` | Bandwidth in MB/s |
| `Lat(avg)` | Average latency in microseconds |
| `Outstanding` | Currently in-flight I/O operations |
| `Err` | Total errors encountered |

## Architecture

### Poll-Mode Threads

- Each worker thread runs a dedicated SPDK poll loop
- One QPair per thread (lockless design)
- Continuous CPU polling (no interrupts, no context switches)
- Threads distributed round-robin across connected controllers (multi-path)

### Multi-Path Operation

When multiple `--traddr` options are provided:
1. X-LOAD connects to each IP as a separate NVMe controller
2. Worker threads are distributed across controllers
3. Each controller provides its maximum QPairs
4. Aggregate throughput scales with number of paths

### Memory Management

- All I/O structures allocated via SPDK DMA memory
- Zero-copy operations where possible
- Hugepage-backed memory for performance

## Performance Tuning

### I/O Depth

Start with `--iodepth 64` and increase based on target capabilities:
- High-end targets: 256-512
- Low-latency targets: 32-128

### CPU Cores / Threads

`--num-cores` controls the number of worker threads:
- Each thread gets its own QPair
- More threads = higher aggregate throughput
- Limited by target's max QPairs per connection

### Range Size

`--range-size` controls blocks per XCOPY operation:
- Maximum practical: 8192 blocks (4MB with 512B blocks)
- NVMe spec limit: 65536 blocks (but target may limit lower)
- Higher values = better throughput, fewer IOPS

### Multi-Path

For maximum throughput:
- Use multiple `--traddr` options
- Each path provides additional QPairs
- Threads automatically distribute across paths

## Troubleshooting


### SPDK Initialization Fails

```
Failed to initialize SPDK environment
Hint: Check hugepages availability
```

**Fix**: Configure hugepages on the host:
```bash
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages
```

### No Free I/O Queue IDs

```
*ERROR* No free I/O queue IDs
```

This means the target has reached its maximum QPairs. X-LOAD handles this gracefully by using fewer threads.

### XCOPY Command Failures (SC=2)

```
XCOPY command failed: SC=2 (Invalid Field)
```

**Fix**: Reduce `--range-size`. The target has a maximum copy size limit smaller than requested.

### Connection Issues

- Verify target is reachable: `ping <traddr>`
- Check NVMe/TCP port: `nc -zv <traddr> <trsvcid>`
- Verify NQN format matches target configuration

## Building from Source

### Native Build

```bash
# Set SPDK path
export SPDK_ROOT=/path/to/spdk

# Build
make

# Run
./x-load --help
```

### Build Requirements

- Linux kernel 5.0+
- C++17 compiler (GCC 7+ or Clang 5+)
- SPDK library with NVMe/TCP transport
- nlohmann-json3-dev

## License

This tool is provided as-is for testing and benchmarking NVMe XCOPY operations.
