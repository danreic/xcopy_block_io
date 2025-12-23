# Dockerfile for X-LOAD (XCOPY High-Concurrency Load Generator)
# Multi-stage build: Stage 1 builds SPDK, Stage 2 builds x-load

# =============================================================================
# Stage 1: Build SPDK
# =============================================================================
# Use AWS ECR public mirror to avoid Docker Hub rate limits
FROM public.ecr.aws/ubuntu/ubuntu:22.04 AS spdk-builder

# Avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install SPDK build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    pkg-config \
    python3 \
    python3-pip \
    python3-pyelftools \
    python3-setuptools \
    meson \
    ninja-build \
    libnuma-dev \
    libssl-dev \
    libaio-dev \
    liburing-dev \
    uuid-dev \
    libncurses-dev \
    libcunit1-dev \
    libfuse3-dev \
    libjson-c-dev \
    libcmocka-dev \
    libarchive-dev \
    libibverbs-dev \
    librdmacm-dev \
    nasm \
    autoconf \
    automake \
    libtool \
    wget \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install Python dependencies for SPDK
RUN pip3 install meson ninja pyelftools grpcio grpcio-tools

# Clone SPDK (use a stable release)
WORKDIR /opt
RUN git clone --depth 1 --branch v24.01.x https://github.com/spdk/spdk.git

# Initialize and update submodules (DPDK, isa-l, etc.)
WORKDIR /opt/spdk
RUN git submodule update --init --recursive

# Build isa-l (Intel Storage Acceleration Library) from SPDK submodule
WORKDIR /opt/spdk/isa-l
RUN ./autogen.sh && \
    ./configure --prefix=/usr/local && \
    make -j$(nproc) && \
    make install

# Build isa-l_crypto if present
WORKDIR /opt/spdk
RUN if [ -d "isa-l-crypto" ]; then \
        cd isa-l-crypto && \
        ./autogen.sh && \
        ./configure --prefix=/usr/local && \
        make -j$(nproc) && \
        make install; \
    fi

# Return to SPDK directory
WORKDIR /opt/spdk

# Update library cache
RUN ldconfig

# Configure SPDK - minimal config for NVMe workloads in containers
# Note: Run ./configure --help to see all options
# --without-uring: Avoid io_uring issues in Docker containers
RUN ./configure \
    --disable-tests \
    --disable-unit-tests \
    --disable-examples \
    --without-fuse \
    --without-rbd \
    --without-iscsi-initiator \
    --without-vhost \
    --without-virtio \
    --without-xnvme \
    --without-vfio-user \
    --without-daos \
    --without-ublk \
    --without-nvme-cuse \
    --without-uring \
    --prefix=/usr/local/spdk

# Build SPDK (use available cores)
RUN make -j$(nproc)

# Install SPDK to a staging directory
RUN make install DESTDIR=/spdk-install

# Also copy the build libraries (some may not be installed)
RUN mkdir -p /spdk-install/usr/local/spdk/lib && \
    cp -a build/lib/*.a /spdk-install/usr/local/spdk/lib/ 2>/dev/null || true && \
    cp -a build/lib/*.so* /spdk-install/usr/local/spdk/lib/ 2>/dev/null || true

# =============================================================================
# Stage 2: Build x-load
# =============================================================================
FROM public.ecr.aws/ubuntu/ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install runtime and build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    pkg-config \
    libnuma-dev \
    libssl-dev \
    libaio-dev \
    liburing-dev \
    uuid-dev \
    libarchive-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy SPDK and isa-l from builder stage
COPY --from=spdk-builder /spdk-install/usr/local/spdk /usr/local/spdk
COPY --from=spdk-builder /usr/local/lib/libisal* /usr/local/lib/
COPY --from=spdk-builder /usr/local/include/isa-l* /usr/local/include/

# Update library cache
RUN ldconfig

# Set library paths
ENV LD_LIBRARY_PATH=/usr/local/spdk/lib:/usr/local/lib:$LD_LIBRARY_PATH
ENV PKG_CONFIG_PATH=/usr/local/spdk/lib/pkgconfig:$PKG_CONFIG_PATH
ENV LIBRARY_PATH=/usr/local/spdk/lib:/usr/local/lib:$LIBRARY_PATH

# Create working directory
WORKDIR /app

# Copy source files
COPY Makefile .
COPY include/ include/
COPY src/ src/

# Build x-load with verbose output for debugging
RUN make SPDK_ROOT=/usr/local/spdk || \
    (echo "Build failed. Checking library locations..." && \
     ls -la /usr/local/spdk/lib/ && \
     ls -la /usr/local/lib/ && \
     exit 1)

# =============================================================================
# Stage 3: Runtime image (minimal)
# =============================================================================
FROM public.ecr.aws/ubuntu/ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Install only runtime dependencies
RUN apt-get update && apt-get install -y \
    libnuma1 \
    libssl3 \
    libaio1 \
    liburing2 \
    libuuid1 \
    libarchive13 \
    pciutils \
    && rm -rf /var/lib/apt/lists/*

# Copy SPDK shared libraries
COPY --from=spdk-builder /spdk-install/usr/local/spdk/lib /usr/local/spdk/lib
COPY --from=spdk-builder /usr/local/lib/libisal* /usr/local/lib/

# Copy the built binary
COPY --from=builder /app/x-load /usr/local/bin/x-load

# Update library cache
RUN ldconfig

# Set library path
ENV LD_LIBRARY_PATH=/usr/local/spdk/lib:/usr/local/lib:$LD_LIBRARY_PATH

# SPDK/DPDK environment variables for container operation
ENV SPDK_SOCK_IMPL_DEFAULT=posix

# Create DPDK runtime directory (required for mempools)
RUN mkdir -p /var/run/dpdk && chmod 777 /var/run/dpdk

# Configure hugepages info
RUN echo "========================================" > /etc/motd && \
    echo "  X-LOAD - XCOPY Load Generator" >> /etc/motd && \
    echo "========================================" >> /etc/motd && \
    echo "" >> /etc/motd && \
    echo "REQUIREMENTS:" >> /etc/motd && \
    echo "  - Hugepages must be configured on the host" >> /etc/motd && \
    echo "    echo 1024 | sudo tee /proc/sys/vm/nr_hugepages" >> /etc/motd && \
    echo "  - Container must run with --privileged" >> /etc/motd && \
    echo "  - NVMe devices must be unbound from kernel drivers" >> /etc/motd && \
    echo "" >> /etc/motd

WORKDIR /app

# Default command shows help
ENTRYPOINT ["/usr/local/bin/x-load"]
CMD ["--help"]

