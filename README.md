# X-LOAD: XCOPY High-Concurrency Load Generator

A high-performance NVMe initiator client designed to achieve absolute saturation of an NVMe-over-TCP (NVMe/TCP) storage target using exclusively the NVMe Copy Command (Opcode 0x19, XCOPY/SCC). Built with SPDK for kernel bypass and poll-mode operation.

## Features

- **SPDK-Based Architecture**: Uses Storage Performance Development Kit (SPDK) for kernel bypass and user-space NVMe/TCP driver
- **Poll-Mode Operation**: Lockless, non-interrupt-driven I/O with dedicated CPU core polling threads
- **High Concurrency**: Configurable I/O depth and CPU core count for maximum target saturation
- **Dynamic XCOPY Complexity**: Randomized number of source range descriptors per command (1 to max_ranges)
- **Cross-Namespace Copy**: Supports TP4130 cross-namespace copy with randomized source NSIDs
- **Comprehensive Statistics**: High-resolution latency measurements with P99, P99.9, P99.99 percentiles
- **Backpressure Resilience**: Graceful handling of saturation errors (0x807, 0x189) without blocking
- **JSON Output**: Optional structured JSON output for automated reporting

## Prerequisites

### System Requirements

- Linux kernel 5.0+ (for NVMe/TCP support)
- C++17 compiler (GCC 7+ or Clang 5+)
- SPDK library (with NVMe/TCP transport support)
- Hugepages configured
- CPU isolation capabilities (numactl)

### Installing SPDK

1. Clone and build SPDK:
```bash
git clone https://github.com/spdk/spdk.git
cd spdk
git submodule update --init
./configure --with-nvme
make
sudo make install
```

2. Set `SPDK_ROOT` environment variable or update Makefile:
```bash
export SPDK_ROOT=/path/to/spdk
```

### Configuring Hugepages

X-LOAD requires hugepages for SPDK's zero-copy operations:

```bash
# Allocate 2MB hugepages (adjust count based on your needs)
sudo sysctl vm.nr_hugepages=1024

# Or use 1GB hugepages (more efficient for large memory allocations)
sudo sysctl vm.nr_hugepages_1gb=4
```

Verify hugepages:
```bash
cat /proc/meminfo | grep Huge
```

### CPU Isolation (Recommended)

For best performance, isolate CPU cores for X-LOAD:

```bash
# Isolate cores 2-5 (example)
sudo isolcpus=2,3,4,5
```

Then use `numactl` or `taskset` to bind X-LOAD to isolated cores.

## Building

```bash
# Build the executable
make

# Check dependencies
make check-libs

# Clean build artifacts
make clean
```

### Build Configuration

Edit `Makefile` to adjust:
- `SPDK_ROOT`: Path to SPDK installation (default: `/usr/local/spdk`)
- `SPDK_LIB`: Path to SPDK libraries
- JSON library path (if not in standard locations)

## Usage

### Basic Example

```bash
./x-load \
  --traddr 192.168.1.100 \
  --trsvcid 4420 \
  --hostnqn nqn.2014-08.org.nvmexpress:uuid:12345678-1234-1234-1234-123456789abc \
  --runtime 60 \
  --iodepth 256 \
  --num-cores 4 \
  --max-ranges 16
```

### Advanced Example with Cross-Namespace Copy

```bash
./x-load \
  --traddr 192.168.1.100 \
  --trsvcid 4420 \
  --hostnqn nqn.2014-08.org.nvmexpress:uuid:12345678-1234-1234-1234-123456789abc \
  --dst-nsid 2 \
  --src-nsid 1 \
  --src-nsid 2 \
  --src-nsid 3 \
  --runtime 300 \
  --iodepth 512 \
  --num-cores 8 \
  --max-ranges 16 \
  --json
```

### Command-Line Options

**Required:**
- `--traddr ADDR`: Target IP address
- `--trsvcid PORT`: Service ID/port (default: 4420)
- `--hostnqn NQN`: Host NQN

**Workload:**
- `--runtime SEC`: Runtime duration in seconds (0 = infinite, default: 10)
- `--iodepth DEPTH`: Maximum global I/O depth (default: 64)
- `--num-cores CORES`: Number of dedicated CPU cores (default: 1)
- `--max-ranges NUM`: Maximum source ranges per command (1-16, default: 1)
- `--dst-nsid NSID`: Destination namespace ID (default: 1)
- `--src-nsid NSID`: Source namespace ID (can be specified multiple times)
- `--dst-lba-start LBA`: Starting LBA for destination (default: 0)
- `--dst-lba-end LBA`: Ending LBA for destination (default: 0 = use namespace size)

**Output:**
- `--json`: Output statistics in JSON format
- `-v, --verbose`: Verbose output
- `-h, --help`: Show help message

## Architecture

X-LOAD is architected for maximum performance:

### Poll-Mode Threads

- Each CPU core runs a dedicated SPDK poll-mode thread
- One QPair per thread (enforced lockless design)
- Continuous CPU polling (no interrupts, no context switches)
- Continuation-passing style (CSP) callbacks for non-blocking I/O

### I/O Flow

1. **Submission**: Polling thread continuously submits XCOPY commands to maintain target I/O depth
2. **Completion**: Completion callbacks immediately trigger next submission (CSP pattern)
3. **Backpressure**: Saturation errors (0x807, 0x189) are deferred without blocking
4. **Statistics**: High-resolution timing captures submission-to-completion latency

### Memory Management

- All I/O structures allocated via SPDK DMA memory (`spdk_dma_malloc`)
- Zero-copy operations where possible
- Hugepage-backed memory for performance

## Performance Tuning

### I/O Depth

Start with `--iodepth 64` and increase based on target capabilities:
- High-end targets: 256-512
- Low-latency targets: 32-128

### CPU Cores

Match `--num-cores` to available isolated cores:
- Each core handles `iodepth / num_cores` outstanding I/O
- More cores = higher aggregate throughput (if target can handle it)

### Range Complexity

`--max-ranges` controls XCOPY command complexity:
- Higher values = more descriptor processing on target
- Test with values 1, 4, 8, 16 to find target's sweet spot

### CPU Affinity

For best performance, bind X-LOAD to isolated cores:

```bash
numactl --cpunodebind=0 --membind=0 ./x-load [options]
```

Or use `taskset`:

```bash
taskset -c 2-5 ./x-load [options]
```

## Output Format

### Human-Readable (Default)

```
=== X-LOAD Workload Complete ===
Duration: 60.00 seconds
Operations completed: 1234567
Operations failed: 0
Bytes copied: 1234567890 (1177.37 MB)

Throughput:
  IOPs: 20576.12
  MB/s: 19.62

Latency (microseconds):
  Average: 12.34
  Min: 5.67
  Max: 1234.56
  Std Dev: 23.45
  P99: 45.67
  P99.9: 123.45
  P99.99: 456.78
==================================
```

### JSON Format (`--json`)

```json
{
  "configuration": {
    "traddr": "192.168.1.100",
    "trsvcid": "4420",
    "hostnqn": "...",
    "runtime_sec": 60,
    "iodepth": 256,
    "num_cores": 4,
    "max_ranges": 16
  },
  "runtime": {
    "elapsed_sec": 60.000
  },
  "statistics": {
    "operations_completed": 1234567,
    "operations_failed": 0,
    "bytes_copied": 1234567890,
    "throughput": {
      "iops": 20576.12,
      "mbps": 19.62
    },
    "latency": {
      "avg_us": 12.34,
      "min_us": 5.67,
      "max_us": 1234.56,
      "std_dev_us": 23.45,
      "p99_us": 45.67,
      "p99_9_us": 123.45,
      "p99_99_us": 456.78
    },
    "errors": {
      "total": 0,
      "saturation": 0,
      "insufficient_resources": 0
    }
  }
}
```

## Troubleshooting

### SPDK Initialization Fails

- Check hugepages: `cat /proc/meminfo | grep Huge`
- Verify SPDK installation: `ls $SPDK_ROOT/lib/libspdk_nvme.a`
- Check permissions for hugepage access

### Connection Fails

- Verify target is reachable: `ping <traddr>`
- Check NVMe/TCP port: `telnet <traddr> <trsvcid>`
- Verify NQN format matches target configuration

### Low Performance

- Increase `--iodepth` (if target can handle it)
- Add more CPU cores with `--num-cores`
- Verify CPU affinity (use `taskset` or `numactl`)
- Check for CPU throttling: `cat /proc/cpuinfo | grep MHz`

### Saturation Errors (0x807, 0x189)

These are expected when saturating the target:
- 0x807: Queue Full - target's submission queue is full
- 0x189: Insufficient Resources - target is resource-constrained

X-LOAD handles these gracefully by deferring retry without blocking.

## License

This tool is provided as-is for testing and benchmarking NVMe XCOPY operations.
