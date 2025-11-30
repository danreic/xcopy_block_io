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

# Check if we can use SPDK's build system
# SPDK typically uses a Makefile that includes their build system
# Check if SPDK_ROOT/mk exists (SPDK build system)
SPDK_MK = $(shell if [ -f "$(SPDK_ROOT)/mk/spdk.common.mk" ]; then echo "$(SPDK_ROOT)/mk/spdk.common.mk"; else echo ""; fi)

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
# For TCP transport support: In some SPDK versions, TCP is built into libspdk_nvme
# In others, separate libraries exist: spdk_nvme_tcp, spdk_sock, spdk_sock_posix
# Check if TCP libraries exist and add them conditionally
SPDK_TCP_LIBS = $(shell \
	if [ -f "$(SPDK_LIB)/libspdk_nvme_tcp.a" ]; then \
		echo "-lspdk_nvme_tcp"; \
	fi \
) $(shell \
	if [ -f "$(SPDK_LIB)/libspdk_sock.a" ]; then \
		echo "-lspdk_sock"; \
	fi \
) $(shell \
	if [ -f "$(SPDK_LIB)/libspdk_sock_posix.a" ]; then \
		echo "-lspdk_sock_posix"; \
	fi \
)

SPDK_LIBS = -lspdk_log -lspdk_env_dpdk -lspdk_nvme \
            $(SPDK_TCP_LIBS) \
            -lspdk_nvmf -lspdk_event_nvmf \
            -lspdk_util -lspdk_ioat \
            -lspdk_accel -lspdk_thread -lspdk_trace \
            -lspdk_keyring -lspdk_json

# DPDK libraries
# If DPDK is built as static libraries, we need to link statically
# Use -Bstatic to force static linking for DPDK libraries
# Use --start-group and --end-group to handle circular dependencies
# Note: rte_log must come early as other DPDK libraries depend on it
# rte_malloc, rte_memzone functions are in librte_eal, not separate libraries
# rte_intr functions might be in librte_eal or a separate library
DPDK_LIBS = -Wl,-Bstatic \
            -Wl,--start-group \
            -lrte_log -lrte_eal -lrte_mempool -lrte_ring -lrte_mbuf \
            -lrte_net -lrte_ethdev -lrte_pci -lrte_bus_pci \
            -lrte_kvargs -lrte_hash -lrte_cmdline -lrte_timer \
            -lrte_telemetry \
            -Wl,--end-group \
            -Wl,-Bdynamic

# System libraries (OpenSSL for crypto, etc.)
SYSTEM_LIBS = -lssl -lcrypto -ljson-c -luuid -ldl

# Link order is critical:
# Put ALL libraries (SPDK and DPDK) in the SAME group to handle circular dependencies
# Use --whole-archive for libspdk_env_dpdk to ensure rte_log symbols are included
# SPDK libraries come FIRST so rte_log is available when DPDK libraries are processed
# DPDK libraries need rte_log (provided by SPDK), SPDK env_dpdk needs DPDK functions
# The linker will iterate through all libraries in the group until all symbols are resolved
# System libraries go last (outside the group, dynamic)
LIBS = -Wl,-Bstatic \
       -Wl,--start-group \
       -Wl,--whole-archive \
       -lspdk_env_dpdk \
       -Wl,--no-whole-archive \
       -lspdk_log -lspdk_nvme \
       $(SPDK_TCP_LIBS) \
       -lspdk_nvmf -lspdk_event_nvmf \
       -lspdk_util -lspdk_ioat \
       -lspdk_accel -lspdk_thread -lspdk_trace \
       -lspdk_keyring -lspdk_json \
       -lrte_log -lrte_eal -lrte_mempool -lrte_ring -lrte_mbuf \
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
	@echo ""
	@echo "Checking for TCP transport libraries:"
	@for lib in spdk_nvme_tcp spdk_sock spdk_sock_posix; do \
		if [ -f "$(SPDK_LIB)/lib$$lib.a" ]; then \
			echo "  ✓ lib$$lib.a found"; \
		else \
			echo "  ✗ lib$$lib.a NOT found"; \
		fi \
	done

# Check if libspdk_env_dpdk provides rte_log (for debugging)
check-rte-log:
	@echo "Checking for rte_log in libspdk_env_dpdk:"
	@nm $(SPDK_LIB)/libspdk_env_dpdk.a 2>/dev/null | grep -E "rte_log|rte_vlog" | head -5 || echo "rte_log not found in libspdk_env_dpdk"
	@echo ""
	@echo "Checking for rte_log in libspdk_log:"
	@nm $(SPDK_LIB)/libspdk_log.a 2>/dev/null | grep -E "rte_log|rte_vlog" | head -5 || echo "rte_log not found in libspdk_log"

# Check if librte_log exists in DPDK library paths
check-dpdk-log:
	@echo "Checking for librte_log in DPDK library paths:"
	@echo "  $(DPDK_LIB):"
	@ls -1 $(DPDK_LIB)/librte_log.a 2>/dev/null || echo "    Not found"
	@echo "  /usr/local/lib64:"
	@ls -1 /usr/local/lib64/librte_log.a 2>/dev/null || echo "    Not found"
	@echo "  /usr/local/lib:"
	@ls -1 /usr/local/lib/librte_log.a 2>/dev/null || echo "    Not found"

# Check if SPDK was built with NVMe-TCP support
check-spdk-tcp:
	@echo "Checking SPDK NVMe-TCP transport support:"
	@echo ""
	@echo "1. Checking for TCP transport libraries:"
	@if [ -f "$(SPDK_LIB)/libspdk_nvme_tcp.a" ]; then \
		echo "  ✓ libspdk_nvme_tcp.a found"; \
	else \
		echo "  ✗ libspdk_nvme_tcp.a NOT found (may be built into libspdk_nvme.a)"; \
	fi
	@if [ -f "$(SPDK_LIB)/libspdk_sock.a" ]; then \
		echo "  ✓ libspdk_sock.a found"; \
	else \
		echo "  ✗ libspdk_sock.a NOT found"; \
	fi
	@if [ -f "$(SPDK_LIB)/libspdk_sock_posix.a" ]; then \
		echo "  ✓ libspdk_sock_posix.a found"; \
	else \
		echo "  ✗ libspdk_sock_posix.a NOT found"; \
	fi
	@echo ""
	@echo "2. Checking for TCP transport symbols in libspdk_nvme.a:"
	@if [ -f "$(SPDK_LIB)/libspdk_nvme.a" ]; then \
		echo "  Searching for TCP-related symbols..."; \
		nm $(SPDK_LIB)/libspdk_nvme.a 2>/dev/null | grep -i "tcp\|transport" | head -10 || echo "    No TCP symbols found"; \
	else \
		echo "  ✗ libspdk_nvme.a not found"; \
	fi
	@echo ""
	@echo "3. Checking SPDK build configuration:"
	@if [ -f "$(SPDK_ROOT)/config.log" ]; then \
		echo "  Checking config.log for TCP-related settings..."; \
		grep -i "tcp\|transport" $(SPDK_ROOT)/config.log 2>/dev/null | head -5 || echo "    No TCP config found in config.log"; \
	else \
		echo "  ✗ config.log not found at $(SPDK_ROOT)/config.log"; \
	fi
	@if [ -f "$(SPDK_ROOT)/build/include/spdk/config.h" ]; then \
		echo "  Checking config.h for TCP defines..."; \
		grep -i "TCP\|TRANSPORT" $(SPDK_ROOT)/build/include/spdk/config.h 2>/dev/null | head -5 || echo "    No TCP defines found"; \
	fi
	@echo ""
	@echo "4. Checking for NVMe-oF libraries (may be needed for TCP):"
	@if [ -f "$(SPDK_LIB)/libspdk_nvmf.a" ]; then \
		echo "  ✓ libspdk_nvmf.a found"; \
	else \
		echo "  ✗ libspdk_nvmf.a NOT found"; \
	fi

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

