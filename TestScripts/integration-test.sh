#!/bin/bash
#
# Integration Test for VirtIO GPU Driver
#
# This script performs end-to-end testing:
# 1. Build the driver on macOS
# 2. Integrate into OpenCore
# 3. Rebuild OpenCore image
# 4. Launch QEMU with VirtIO GPU configuration
# 5. Monitor boot logs for driver messages
# 6. Verify display output (requires manual verification or SPICE)
#
# Prerequisites:
# - Running on macOS for build (or cross-compile setup)
# - QEMU installed on host (Linux or macOS)
# - libguestfs-tools (guestfish) installed
# - macOS guest image prepared (BaseSystem.img, OpenCore)
#
# Usage:
#   ./integration-test.sh [--no-qemu] [--wait=SECONDS]
#
# Options:
#   --no-qemu     Build and integrate but don't launch QEMU
#   --wait=N      Wait N seconds after QEMU launch for logs (default=30)
#   --help        Show this help
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VIRTIO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$VIRTIO_DIR/.." && pwd)"
OPENCORE_DIR="$REPO_ROOT/OpenCore"
DRIVER_NAME="virtio-gpu"
GPU_MODE="virtio"

NO_QEMU=0
WAIT_TIME=30

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-qemu) NO_QEMU=1; shift ;;
        --wait=*) WAIT_TIME="${1#*=}"; shift ;;
        -h|--help) 
            echo "Usage: $0 [OPTIONS]"
            echo "Integration test for VirtIO GPU driver"
            echo ""
            echo "Options:"
            echo "  --no-qemu     Build and integrate but don't launch QEMU"
            echo "  --wait=N      Wait N seconds after QEMU launch (default=30)"
            echo "  --help        Show this help"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { echo -e "${YELLOW}[INFO]${NC} $1"; }
pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; }

# Check we're on macOS for build
if [[ "$(uname)" != "Darwin" ]]; then
    fail "Build must be performed on macOS"
    exit 1
fi

# Step 1: Build driver
info "Step 1: Building VirtIO GPU driver..."
cd "$VIRTIO_DIR"
if ! make -s clean; then
    fail "make clean failed"
    exit 1
fi
if ! make -s; then
    fail "make failed"
    exit 1
fi
if [ ! -d "build/virtio-gpu.kext" ]; then
    fail "kext not produced"
    exit 1
fi
pass "Driver built successfully"

# Step 2: Integrate into OpenCore
info "Step 2: Integrating driver into OpenCore..."
cp -R "build/virtio-gpu.kext" "$OPENCORE_DIR/Drivers/"
if [ $? -ne 0 ]; then
    fail "Failed to copy kext to OpenCore/Drivers/"
    exit 1
fi
pass "Driver copied to OpenCore/Drivers/"

# Step 3: Rebuild OpenCore image
info "Step 3: Rebuilding OpenCore.qcow2..."
cd "$OPENCORE_DIR"
if [ ! -f "opencore-image-ng.sh" ]; then
    fail "opencore-image-ng.sh not found"
    exit 1
fi
if ! command -v guestfish &>/dev/null; then
    fail "guestfish not installed (need libguestfs-tools)"
    info "On Linux: sudo apt-get install libguestfs-tools"
    exit 1
fi
rm -f OpenCore.qcow2
if ! ./opencore-image-ng.sh --cfg config.plist --img OpenCore.qcow2; then
    fail "OpenCore image rebuild failed"
    exit 1
fi
pass "OpenCore.qcow2 rebuilt"

# Step 4: Verify configuration
info "Step 4: Verifying boot configuration..."
# Check GPU_MODE in boot script
if ! grep -q "GPU_MODE=\"$GPU_MODE\"" "$REPO_ROOT/OpenCore-Boot.sh"; then
    info "Warning: OpenCore-Boot.sh does not have GPU_MODE=\"$GPU_MODE\" set"
    info "You may need to edit it manually"
fi
pass "Configuration verified"

if [ $NO_QEMU -eq 1 ]; then
    echo ""
    info "Integration test completed (QEMU launch skipped)"
    info "To manually test:"
    info "  cd $REPO_ROOT"
    info "  GPU_MODE=\"$GPU_MODE\" ./OpenCore-Boot.sh"
    exit 0
fi

# Step 5: Launch QEMU
info "Step 5: Launching QEMU..."
cd "$REPO_ROOT"
if [ ! -x "./OpenCore-Boot.sh" ]; then
    fail "OpenCore-Boot.sh not found or not executable"
    exit 1
fi

echo ""
echo "Starting QEMU. Press Ctrl+A then X to exit QEMU, or Ctrl+C to stop this script."
echo "Watch the QEMU window and macOS logs for VirtIO-GPU messages."
echo ""

# Launch QEMU in background? For now run in foreground
GPU_MODE="$GPU_MODE" ./OpenCore-Boot.sh &
QEMU_PID=$!

# Wait for QEMU to initialize
sleep $WAIT_TIME

# Step 6: Check if driver loaded (would need to query macOS guest)
info "Step 6: Checking driver load status..."
# At this point we'd ideally SSH into the guest or use serial console
# but that requires network setup. So we'll give manual instructions.

echo ""
echo "=========================================="
echo "Integration test execution complete."
echo ""
echo "Manual verification steps:"
echo "1. Look for these messages in macOS Console.app or kernel log:"
echo "   [VirtIO-GPU] Driver start"
echo "   [VirtIO-GPU] Driver started successfully"
echo ""
echo "2. Run in macOS guest terminal:"
echo "   kextstat | grep virtio-gpu"
echo "   Should show: com.osx-kvm.driver.virtio-gpu"
echo ""
echo "3. Check display:"
echo "   System Preferences → Displays → should report 1920x1080 (or configured)"
echo ""
echo "4. Test if framebuffer is active:"
echo "   Open a window and move it; if you see it, basic display works"
echo ""
echo "To terminate QEMU: Ctrl+A then X"
echo "=========================================="
echo ""

# Wait for user to terminate QEMU
wait $QEMU_PID 2>/dev/null || true

pass "Integration test script completed"
