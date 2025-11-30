# Makefile for NVMe XCOPY I/O Tool
# Uses libnvme (Linux kernel NVMe library) instead of SPDK

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -std=c11
LDFLAGS = -lpthread -lnuma

# Include paths
INCLUDES = -I/usr/include -Iinclude

# Library paths
LIBPATHS = -L/usr/lib -L/usr/lib64 -L/usr/local/lib -L/usr/local/lib64

# Libraries: libnvme and liburing
# libnvme: Linux kernel's user-space NVMe library
# liburing: For async I/O operations
LIBS = -lnvme -luring -lpthread -lnuma -ldl

# Source files
SRCDIR = src
INCDIR = include
SOURCES = $(SRCDIR)/main.c \
          $(SRCDIR)/nvme_wrapper.c \
          $(SRCDIR)/io_uring_wrapper.c \
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

# Check which libraries exist (for debugging)
check-libs:
	@echo "Checking required libraries:"
	@pkg-config --exists libnvme && echo "  ✓ libnvme found" || echo "  ✗ libnvme NOT found (install: apt-get install libnvme-dev)"
	@pkg-config --exists liburing && echo "  ✓ liburing found" || echo "  ✗ liburing NOT found (install: apt-get install liburing-dev)"
	@echo ""
	@echo "Checking kernel NVMe-TCP support:"
	@lsmod | grep -q nvme_tcp && echo "  ✓ nvme_tcp kernel module loaded" || echo "  ✗ nvme_tcp kernel module not loaded (run: modprobe nvme-tcp)"

# Build the main executable
# Add rpath so the binary can find libraries at runtime
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBPATHS) $(LIBS) $(LDFLAGS) \
		-Wl,--as-needed \
		-Wl,-rpath,/usr/lib:/usr/lib64:/usr/local/lib:/usr/local/lib64 \
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
	@echo "Dependencies:"
	@echo "  libnvme - Linux kernel NVMe library (install: apt-get install libnvme-dev)"
	@echo "  liburing - io_uring library (install: apt-get install liburing-dev)"

.PHONY: all clean install uninstall help check-libs

