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
LIBPATHS = -L$(SPDK_LIB) -L$(DPDK_LIB) -L/usr/local/lib

# SPDK libraries - need many more dependencies
# Some libraries may not exist in all SPDK builds, removed missing ones
# Note: spdk_keyring requires spdk_json, and spdk_nvme requires both for authentication
# spdk_key functions are in spdk_keyring, not a separate library
SPDK_LIBS = -lspdk_nvme -lspdk_env_dpdk -lspdk_log \
            -lspdk_util -lspdk_ioat \
            -lspdk_accel -lspdk_thread -lspdk_trace \
            -lspdk_json -lspdk_keyring

# DPDK libraries
DPDK_LIBS = -lrte_eal -lrte_mempool -lrte_ring -lrte_mbuf \
            -lrte_net -lrte_ethdev -lrte_pci -lrte_bus_pci \
            -lrte_kvargs -lrte_hash -lrte_cmdline -lrte_timer \
            -lrte_telemetry

# System libraries (OpenSSL for crypto, etc.)
SYSTEM_LIBS = -lssl -lcrypto -ljson-c -luuid -ldl

LIBS = $(SPDK_LIBS) $(DPDK_LIBS) $(SYSTEM_LIBS)

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

# Build the main executable
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBPATHS) $(LIBS) $(LDFLAGS) -Wl,--no-as-needed

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

