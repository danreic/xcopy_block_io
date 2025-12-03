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
# Order: libraries that need symbols come first, providers come later
SPDK_LIBS = -Wl,--start-group \
            -Wl,--whole-archive -lspdk_nvme -Wl,--no-whole-archive \
            -lspdk_thread -lspdk_trace -lspdk_keyring -lspdk_keyring_linux \
            -lspdk_json -lspdk_event -lspdk_log -lspdk_util -lspdk_env_dpdk \
            -Wl,--end-group \
            -lrte_eal -lrte_mempool -lrte_ring -lrte_mbuf \
            -lrte_ethdev -lrte_net -lrte_bus_pci -lrte_pci \
            -lrte_cmdline -lrte_kvargs -lrte_hash -lrte_meter

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

# Build the main executable
# Link with SPDK libraries (static linking preferred)
$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS) $(LIBPATHS) $(LIBS) $(LDFLAGS) \
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

