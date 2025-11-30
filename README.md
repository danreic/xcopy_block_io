# NVMe XCOPY I/O Tool

A low-level C tool for generating high-volume NVMe XCOPY (Copy command) block load using libnvme (Linux kernel NVMe library). Supports multiple concurrent copy ranges and cross-volume operations.

## Features

- **Multiple Transport Support**: NVMe-TCP (primary), NVMe-RDMA, and PCIe
- **Multi-Range Operations**: Up to 16 ranges per NVMe Copy command
- **Cross-Volume Support**: Copy between different namespaces/volumes
- **High Performance**: Multi-threaded with configurable queue depths
- **Statistics**: Comprehensive performance metrics (throughput, latency)

## Building

### Prerequisites

- **libnvme**: Linux kernel's user-space NVMe library
  - Install: `sudo apt-get install libnvme-dev` (Ubuntu/Debian)
  - Or: `sudo yum install libnvme-devel` (RHEL/CentOS)
- **liburing**: io_uring library for async I/O
  - Install: `sudo apt-get install liburing-dev` (Ubuntu/Debian)
  - Or: `sudo yum install liburing-devel` (RHEL/CentOS)
- GCC compiler
- pthread and libnuma development libraries
- Linux kernel 5.0+ with NVMe-TCP support (kernel module: `nvme-tcp`)

### Compilation

```bash
# Build
make

# Check if required libraries are installed
make check-libs
```

## Usage

### Using Device Paths (Recommended)

The easiest way is to use device paths - the tool will auto-detect transport details and namespace IDs:

```bash
# Copy from /dev/nvme1n1 to /dev/nvme1n2 (same controller, different namespaces)
./xcopy_tool --src-device /dev/nvme1n1 --dst-device /dev/nvme1n2

# With performance tuning for 100GB copy
./xcopy_tool \
  --src-device /dev/nvme1n1 \
  --dst-device /dev/nvme1n2 \
  --range-size 16384 \
  --num-ranges 16 \
  --threads $(nproc) \
  --queue-depth 512 \
  -c 800 \
  -v
```

### Manual Transport Configuration

If you prefer to specify transport details manually:

```bash
# NVMe-TCP example
./xcopy_tool \
  -t tcp \
  -a 192.168.1.100 \
  -n nqn.2016-06.io.spdk:cnode1 \
  --src-nsid 1 \
  --dst-nsid 2

# PCIe example
./xcopy_tool -t pcie -a 0000:01:00.0 --src-nsid 1 --dst-nsid 2
```

### Command Line Options

**Transport Options:**
- `-t, --transport TYPE`: Transport type (tcp, pcie, rdma, default: tcp)
- `-a, --traddr ADDR`: Transport address (IP for TCP, PCIe address for PCIe)
- `-s, --trsvcid PORT`: Transport service ID (port for TCP, default: 4420)
- `-n, --subnqn NQN`: Subsystem NQN (required for TCP/RDMA)

**Workload Options:**
- `--src-device PATH`: Source device path (e.g., /dev/nvme1n1) - auto-detects transport and namespace ID
- `--dst-device PATH`: Destination device path (e.g., /dev/nvme1n2) - auto-detects transport and namespace ID
- `--src-nsid NSID`: Source namespace ID (default: 1, auto-detected from --src-device)
- `--dst-nsid NSID`: Destination namespace ID (default: 1, auto-detected from --dst-device)
- `--src-lba LBA`: Starting LBA for source (default: 0)
- `--dst-lba LBA`: Starting LBA for destination (default: 0)
- `--range-size SIZE`: Size of each range in blocks (default: 8)
- `--num-ranges NUM`: Number of concurrent ranges (default: 1, max: 16)

**Concurrency Options:**
- `--threads NUM`: Number of worker threads (default: 1)
- `--queue-depth DEPTH`: Queue depth per thread (default: 64)

**Runtime Options:**
- `-d, --duration SEC`: Test duration in seconds (0 = infinite, default: 10)
- `-c, --count NUM`: Number of operations to perform (0 = infinite)
- `-v, --verbose`: Verbose output
- `-h, --help`: Show help message

## Architecture

The tool consists of several core modules:

1. **NVMe Wrapper** (`nvme_wrapper.c`): Handles libnvme initialization and transport abstraction
2. **io_uring Wrapper** (`io_uring_wrapper.c`): Provides async I/O layer using io_uring
3. **Command Builder** (`xcopy_cmd.c`): Constructs NVMe Copy commands with range descriptors
4. **Range Manager** (`range_manager.c`): Manages workload distribution across ranges
5. **Volume Manager** (`volume_manager.c`): Handles namespace/volume mapping
6. **Concurrency Manager** (`concurrency_manager.c`): Manages multi-threaded operation submission with io_uring
7. **Statistics** (`statistics.c`): Collects and reports performance metrics

## License

This tool is provided as-is for testing and benchmarking NVMe XCOPY operations.

