# Makefile for NVMe XCOPY I/O Tool
# Compatible with most Linux distributions

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -std=c11
LDFLAGS = -lpthread -lnuma

# SPDK paths - adjust these based on your SPDK installation
SPDK_ROOT ?= /usr/local
SPDK_INC = $(SPDK_ROOT)/include
# Check for build/lib first (if SPDK was built but not installed), then lib
SPDK_LIB = $(shell if [ -d "$(SPDK_ROOT)/build/lib" ]; then echo "$(SPDK_ROOT)/build/lib"; else echo "$(SPDK_ROOT)/lib"; fi)

# DPDK paths - adjust these based on your DPDK installation
DPDK_ROOT ?= /usr/local
DPDK_INC = $(DPDK_ROOT)/include
# Check for build/lib first (if DPDK was built but not installed), then lib
DPDK_LIB = $(shell if [ -d "$(DPDK_ROOT)/build/lib" ]; then echo "$(DPDK_ROOT)/build/lib"; else echo "$(DPDK_ROOT)/lib"; fi)

# Include paths
INCLUDES = -I$(SPDK_INC) -I$(DPDK_INC) -Iinclude

# Library paths and libraries
# Check both build/lib and /usr/local/lib64 for DPDK libraries
LIBPATHS = -L$(SPDK_LIB) -L$(DPDK_LIB) -L/usr/local/lib -L/usr/local/lib64

# SPDK libraries - need many more dependencies
# Some libraries may not exist in all SPDK builds, removed missing ones
# Note: spdk_keyring requires spdk_json, and spdk_nvme requires both for authentication
# spdk_key functions are in spdk_keyring, not a separate library
# Link order matters: libspdk_log should come before libspdk_env_dpdk
# as it may provide rte_log functions that env_dpdk needs
SPDK_LIBS = -lspdk_log -lspdk_env_dpdk -lspdk_nvme \
            -lspdk_util -lspdk_ioat \
            -lspdk_accel -lspdk_thread -lspdk_trace \
            -lspdk_keyring -lspdk_json

# DPDK libraries
# If DPDK is built as static libraries, we need to link statically
# Use -Bstatic to force static linking for DPDK libraries
# Use --start-group and --end-group to handle circular dependencies
# Note: rte_log functions should be provided by SPDK's env_dpdk
# rte_malloc, rte_memzone functions are in librte_eal, not separate libraries
# rte_intr functions might be in librte_eal or a separate library
DPDK_LIBS = -Wl,-Bstatic \
            -Wl,--start-group \
            -lrte_eal -lrte_mempool -lrte_ring -lrte_mbuf \
            -lrte_net -lrte_ethdev -lrte_pci -lrte_bus_pci \
            -lrte_kvargs -lrte_hash -lrte_cmdline -lrte_timer \
            -lrte_telemetry \
            -Wl,--end-group \
            -Wl,-Bdynamic

# System libraries (OpenSSL for crypto, etc.)
SYSTEM_LIBS = -lssl -lcrypto -ljson-c -luuid -ldl

# Link order is critical:
# Put ALL libraries (SPDK and DPDK) in the SAME group to handle circular dependencies
# SPDK libraries come FIRST so rte_log is available when DPDK libraries are processed
# DPDK libraries need rte_log (provided by SPDK), SPDK env_dpdk needs DPDK functions
# The linker will iterate through all libraries in the group until all symbols are resolved
# System libraries go last (outside the group, dynamic)
LIBS = -Wl,-Bstatic \
       -Wl,--start-group \
       $(SPDK_LIBS) \
       -lrte_eal -lrte_mempool -lrte_ring -lrte_mbuf \
       -lrte_net -lrte_ethdev -lrte_pci -lrte_bus_pci \
       -lrte_kvargs -lrte_hash -lrte_cmdline -lrte_timer -lrte_telemetry \
       -Wl,--end-group \
       -Wl,-Bdynamic \
       $(SYSTEM_LIBS)

# Source files
SRCDIR = src
INCDIR = include
SOURCES = $(SRCDIR)/main.c \
          $(SRCDIR)/spdk_wrapper.c \
          $(SRCDIR)/xcopy_cmd.c \
          $(SRCDIR)/range_manager.c \
          $(SRCDIR)/volume_manager.c \
          $(SRCDIR)/concurrency_manager.c \
          $(SRCDIR)/statistics.c \
          $(SRCDIR)/device_parser.c

OBJECTS = $(SOURCES:.c=.o)
TARGET = xcopy_tool

# Default target
all: $(TARGET)

# Check which SPDK libraries exist (for debugging)
check-spdk-libs:
	@echo "Checking SPDK libraries in $(SPDK_LIB):"
	@ls -1 $(SPDK_LIB)/libspdk*.a 2>/dev/null | sed 's|.*/lib||; s|\.a$$||' | sort || echo "No libraries found"

# Check if libspdk_env_dpdk provides rte_log (for debugging)
check-rte-log:
	@echo "Checking for rte_log in libspdk_env_dpdk:"
	@nm $(SPDK_LIB)/libspdk_env_dpdk.a 2>/dev/null | grep -E "rte_log|rte_vlog" | head -5 || echo "rte_log not found in libspdk_env_dpdk"
	@echo ""
	@echo "Checking for rte_log in libspdk_log:"
	@nm $(SPDK_LIB)/libspdk_log.a 2>/dev/null | grep -E "rte_log|rte_vlog" | head -5 || echo "rte_log not found in libspdk_log"

# Build the main executable
# Add rpath so the binary can find libraries at runtime
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBPATHS) $(LIBS) $(LDFLAGS) \
		-Wl,--as-needed \
		-Wl,-rpath,$(SPDK_LIB):$(DPDK_LIB):/usr/local/lib:/usr/local/lib64 \
		-Wl,--disable-new-dtags

# Compile source files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(TARGET)

# Install (optional)
install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

# Uninstall
uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)

# Help target
help:
	@echo "Available targets:"
	@echo "  all       - Build the xcopy_tool executable (default)"
	@echo "  clean     - Remove build artifacts"
	@echo "  install   - Install to /usr/local/bin"
	@echo "  uninstall - Remove from /usr/local/bin"
	@echo ""
	@echo "Environment variables:"
	@echo "  SPDK_ROOT - SPDK installation root (default: /usr/local)"
	@echo "  DPDK_ROOT - DPDK installation root (default: /usr/local)"

.PHONY: all clean install uninstall help

