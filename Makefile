# Makefile for X-LOAD (XCOPY High-Concurrency Load Generator)
# Uses SPDK for kernel bypass and poll-mode operation

CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -g -std=c++17 -march=native
LDFLAGS = -lpthread -lnuma -ldl

# SPDK paths (adjust these based on your SPDK installation)
SPDK_ROOT ?= /usr/local/spdk
SPDK_INCLUDE = $(SPDK_ROOT)/include
# Auto-detect: use build/lib if it exists (for non-installed builds), otherwise use lib (for installed)
SPDK_LIB = $(shell if [ -d "$(SPDK_ROOT)/build/lib" ]; then echo "$(SPDK_ROOT)/build/lib"; else echo "$(SPDK_ROOT)/lib"; fi)

# Include paths
INCLUDES = -I$(SPDK_INCLUDE) -Iinclude -I/usr/include

# Library paths
LIBPATHS = -L$(SPDK_LIB) -L/usr/lib -L/usr/lib64 -L/usr/local/lib -L/usr/local/lib64

# SPDK libraries (link statically for better performance)
# Use --start-group/--end-group to handle circular dependencies
# Use --whole-archive for nvme library to ensure TCP transport constructor runs
# Note: We exclude CUSE (fuse) code as we don't need it
# Order: libraries that need symbols come first, providers come later
# Note: Keyring libraries are optional and may not exist in all SPDK versions

# Detect which optional libraries exist
SPDK_HAS_KEYRING = $(shell test -f $(SPDK_LIB)/libspdk_keyring.a && echo yes || echo no)
SPDK_HAS_DMA = $(shell test -f $(SPDK_LIB)/libspdk_dma.a && echo yes || echo no)

# Build optional library flags
SPDK_KEYRING_LIBS = $(if $(filter yes,$(SPDK_HAS_KEYRING)),-lspdk_keyring -lspdk_keyring_linux,)
SPDK_DMA_LIBS = $(if $(filter yes,$(SPDK_HAS_DMA)),-lspdk_dma,)

SPDK_LIBS = -Wl,--start-group \
            -Wl,--whole-archive -lspdk_nvme_no_cuse -Wl,--no-whole-archive \
            -lspdk_sock -lspdk_sock_posix \
            -lspdk_accel -lspdk_event_sock \
            $(SPDK_DMA_LIBS) \
            -lspdk_thread -lspdk_trace $(SPDK_KEYRING_LIBS) \
            -lspdk_json -lspdk_event -lspdk_log -lspdk_util -lspdk_env_dpdk \
            -Wl,--end-group \
            -lrte_eal -lrte_mempool -lrte_ring -lrte_mbuf \
            -lrte_ethdev -lrte_net -lrte_bus_pci -lrte_pci \
            -lrte_cmdline -lrte_kvargs -lrte_hash -lrte_meter \
            -lisal -lisal_crypto -Wl,--undefined=spdk_nvme_transport_register

# JSON library (nlohmann/json header-only, or use pkg-config if installed)
JSON_INCLUDE = -I/usr/include/nlohmann

# All libraries (note: nlohmann/json is header-only, no linking needed)
# OpenSSL and UUID are required by SPDK
LIBS = $(SPDK_LIBS) -lpthread -lnuma -ldl -lrt -lm -lssl -lcrypto -luuid

# Source files (C++ implementation)
SRCDIR = src
INCDIR = include
SOURCES = $(SRCDIR)/main.cpp \
          $(SRCDIR)/spdk_context.cpp \
          $(SRCDIR)/poll_thread_manager.cpp \
          $(SRCDIR)/xcopy_generator.cpp \
          $(SRCDIR)/statistics.cpp \
          $(SRCDIR)/high_res_timer.cpp \
          $(SRCDIR)/json_reporter.cpp \
          $(SRCDIR)/error_handler.cpp \
          $(SRCDIR)/lba_manager.cpp \
          $(SRCDIR)/config_manager.cpp

OBJECTS = $(SOURCES:.cpp=.o)
TARGET = x-load

# Default target
all: $(TARGET)

# Check which libraries exist (for debugging)
check-libs:
	@echo "Checking required libraries:"
	@test -d $(SPDK_ROOT) && echo "  ✓ SPDK found at $(SPDK_ROOT)" || echo "  ✗ SPDK NOT found at $(SPDK_ROOT) (set SPDK_ROOT)"
	@echo "  Using SPDK_LIB: $(SPDK_LIB)"
	@test -f $(SPDK_LIB)/libspdk_nvme.a && echo "  ✓ SPDK NVMe library found at $(SPDK_LIB)/libspdk_nvme.a" || echo "  ✗ SPDK NVMe library NOT found at $(SPDK_LIB)/libspdk_nvme.a"
	@echo ""
	@echo "Checking hugepages:"
	@grep -q Hugepages /proc/meminfo && grep Hugepages /proc/meminfo | head -2 || echo "  ✗ Hugepages not configured"

# Create a temporary nvme library without CUSE (fuse) code
NVME_LIB_TMP = /tmp/libspdk_nvme_no_cuse.a
NVME_LIB_SRC = $(shell if [ -f "$(SPDK_ROOT)/build/lib/libspdk_nvme.a" ]; then echo "$(SPDK_ROOT)/build/lib/libspdk_nvme.a"; elif [ -f "$(SPDK_ROOT)/lib/libspdk_nvme.a" ]; then echo "$(SPDK_ROOT)/lib/libspdk_nvme.a"; else echo ""; fi)

$(NVME_LIB_TMP):
	@if [ -z "$(NVME_LIB_SRC)" ] || [ ! -f "$(NVME_LIB_SRC)" ]; then \
		echo "Error: Cannot find libspdk_nvme.a. Check SPDK_ROOT (currently: $(SPDK_ROOT))"; \
		exit 1; \
	fi
	@echo "Creating nvme library without CUSE from $(NVME_LIB_SRC)..."
	@mkdir -p /tmp/spdk_nvme_extract
	@cd /tmp/spdk_nvme_extract && ar x $(NVME_LIB_SRC)
	@cd /tmp/spdk_nvme_extract && rm -f nvme_cuse.o 2>/dev/null || true
	@ar rcs $(NVME_LIB_TMP) /tmp/spdk_nvme_extract/*.o
	@rm -rf /tmp/spdk_nvme_extract
	@echo "Created $(NVME_LIB_TMP)"

# Build the main executable
# Link with SPDK libraries (static linking preferred)
$(TARGET): $(OBJECTS) $(NVME_LIB_TMP)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS) $(LIBPATHS) -L/tmp $(LIBS) $(LDFLAGS) \
		-Wl,--no-as-needed \
		-Wl,-rpath,$(SPDK_LIB):/usr/lib:/usr/lib64:/usr/local/lib:/usr/local/lib64 \
		-Wl,--disable-new-dtags

# Compile C++ source files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(JSON_INCLUDE) -c $< -o $@

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
	@echo "  all       - Build the x-load executable (default)"
	@echo "  clean     - Remove build artifacts"
	@echo "  install   - Install to /usr/local/bin"
	@echo "  uninstall - Remove from /usr/local/bin"
	@echo "  check-libs - Check for required libraries and system configuration"
	@echo ""
	@echo "Dependencies:"
	@echo "  SPDK - Storage Performance Development Kit (set SPDK_ROOT=/path/to/spdk)"
	@echo "  nlohmann/json - JSON library (header-only, install: apt-get install nlohmann-json3-dev)"
	@echo "  Hugepages - Must be configured (see README.md)"
	@echo ""
	@echo "Environment:"
	@echo "  SPDK_ROOT - Path to SPDK installation (default: /usr/local/spdk)"

.PHONY: all clean install uninstall help check-libs

