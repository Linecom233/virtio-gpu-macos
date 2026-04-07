# Makefile for VirtIO GPU macOS Kext
#
# Build: make
# Clean: make clean
#
# Requirements:
# - macOS with Xcode Command Line Tools
# - macOS SDK
#
# Usage:
#   make              # Build the kext
#   make install      # Copy to OpenCore/Drivers and rebuild OpenCore.qcow2
#   make clean        # Clean build artifacts
#
# Note: This must be built on macOS. Cross-compilation from Linux is not supported.

KEXT_NAME = virtio-gpu
BUNDLE_NAME = $(KEXT_NAME).kext
BUNDLE_ID = com.osx-kvm.driver.virtio-gpu

VERSION = 0.1.0
KEXT_VERSION = $(VERSION)

# Source files
SRC_DIR = Source
INC_DIR = Include
RES_DIR = Resources

SOURCES = $(SRC_DIR)/virtio_gpu.cpp \
          $(SRC_DIR)/virtqueue.cpp \
          $(SRC_DIR)/IOFramebuffer.cpp

PUBLIC_HEADERS = $(INC_DIR)/virtio_gpu.h \
                 $(INC_DIR)/virtio_types.h

# Build directories
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
SYM_DIR = $(BUILD_DIR)/sym
BUNDLE_DIR = $(BUILD_DIR)/$(BUNDLE_NAME)
BUNDLE_CONTENTS = $(BUNDLE_DIR)/Contents
BUNDLE_MACOS = $(BUNDLE_CONTENTS)/MacOS
BUNDLE_RESOURCES = $(BUNDLE_CONTENTS)/Resources

# Object files
OBJS = $(addprefix $(OBJ_DIR)/, $(notdir $(SOURCES:.cpp=.o)))

# Find SDK
SDK ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null || echo /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk)

# Check SDK exists
ifeq ("$(wildcard $(SDK))","")
    $(error SDK not found at $(SDK). Install Xcode or specify SDK=path)
endif

# Compiler flags
CFLAGS = -std=c++17 -Wall -Wextra \
         -fno-exceptions -fno-rtti \
         -mkernel -nostdlib \
         -isysroot $(SDK) \
         -arch x86_64 \
         -O2 -g \
         -I$(INC_DIR) \
         -DDEBUG=1 \
         -Wl,-kext

# Linker flags
LDFLAGS = -Xlinker -kext -nostdlib -lcc_kext

# Target
.PHONY: all clean install debug package test

all: $(BUNDLE_DIR)

test: unit-test

unit-test:
	@echo "Running unit tests..."
	cd Tests && make clean && make && ./virtio-gpu-tests

# Integration test (requires macOS, QEMU, etc.)
integration-test:
	@echo "Running integration test..."
	@if [ -x TestScripts/integration-test.sh ]; then \
		TestScripts/integration-test.sh; \
	else \
		echo "Integration test script not found"; \
		exit 1; \
	fi

# Full test suite
tests: unit-test integration-test

# ... rest of targets

$(BUNDLE_DIR): $(OBJS) $(RES_DIR)/Info.plist
	@echo "Linking kext..."
	@mkdir -p $(BUNDLE_MACOS)
	$(CXX) $(CFLAGS) $(LDFLAGS) -o $(BUNDLE_MACOS)/$(KEXT_NAME) $(OBJS)
	@mkdir -p $(BUNDLE_RESOURCES)
	cp $(RES_DIR)/Info.plist $(BUNDLE_INFO)
	@echo "Built $(BUNDLE_DIR)"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(PUBLIC_HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CFLAGS) -c $< -o $@

$(BUNDLE_INFO): $(RES_DIR)/Info.plist
	@mkdir -p $(BUNDLE_RESOURCES)
	cp $< $@

# Code signing (optional)
sign: $(BUNDLE_DIR)
	@echo "Signing kext..."
	sudo codesign -s - -f -v --entitlement $(RES_DIR)/entitlements.plist $(BUNDLE_DIR) || \
	sudo codesign -s - -f -v $(BUNDLE_DIR)
	@echo "Signed (development)"

# Install to system (requires root)
install: $(BUNDLE_DIR) sign
	@echo "Installing to /Library/Extensions..."
	sudo cp -R $(BUNDLE_DIR) /Library/Extensions/
	sudo kextcache -i /
	@echo "Installed. Load with: sudo kextload /Library/Extensions/$(BUNDLE_NAME)"
	@echo "Or reboot."

# Development: copy to ~/Library/Extensions (SIP must be disabled or signed)
debug: $(BUNDLE_DIR) sign
	rm -rf ~/Library/Extensions/$(BUNDLE_NAME)
	cp -R $(BUNDLE_DIR) ~/Library/Extensions/
	@echo "Copied to ~/Library/Extensions"
	@echo "Load with: sudo kextload ~/Library/Extensions/$(BUNDLE_NAME)"

# Integration with OpenCore (copy driver and rebuild image)
opencore: $(BUNDLE_DIR)
	@echo "Integrating with OpenCore..."
	@if [ -z "$(OPENCORE_ROOT)" ]; then \
	    OPENCORE_ROOT="$(CURDIR)/../.."; \
	fi
	@echo "OpenCore root: $(OPENCORE_ROOT)"
	rm -rf "$(OPENCORE_ROOT)/OpenCore/Drivers/$(KEXT_NAME).kext"
	cp -R $(BUNDLE_DIR) "$(OPENCORE_ROOT)/OpenCore/Drivers/"
	@echo "Copied to OpenCore/Drivers/$(KEXT_NAME).kext"
	@echo ""
	@echo "Next step: Rebuild OpenCore.qcow2"
	@echo "  cd $(OPENCORE_ROOT)/OpenCore"
	@echo "  ./opencore-image-ng.sh --cfg config.plist --img OpenCore.qcow2"
	@echo ""
	@echo "Then boot with GPU_MODE=\"virtio\""

# Package for distribution
package: $(BUNDLE_DIR)
	tar -czf $(KEXT_NAME)-$(VERSION).tar -C $(BUILD_DIR) $(BUNDLE_NAME)
	@echo "Created $(KEXT_NAME)-$(VERSION).tar"

# Debug variables
print-%:
	@echo $* = $($*)

clean:
	rm -rf $(BUILD_DIR)
	@echo "Cleaned"

# Show config
config:
	@echo "Build configuration:"
	@echo "  SDK: $(SDK)"
	@echo "  Sources: $(SOURCES)"
	@echo "  Objects: $(OBJS)"
	@echo "  Bundle: $(BUNDLE_DIR)"

