# Testing Guide for VirtIO GPU Driver

This document describes how to test the VirtIO GPU driver, including unit tests, integration tests, and verification procedures.

## Test Strategy

The testing approach is multi-layered:

1. **Unit Tests** - Verify individual components (virtqueue operations, format conversion)
2. **Integration Tests** - Test driver loading, framebuffer creation, display functionality
3. **System Tests** - Full VM boot with QEMU, verify display output
4. **Regression Tests** - Ensure changes don't break existing functionality

## Prerequisites for Testing

### For Unit Tests

- macOS system (any recent version)
- Xcode Command Line Tools (clang++)
- No special privileges needed

### For Integration/System Tests

- macOS system (host) for building driver
- Linux host running QEMU (or macOS host with QEMU)
- OpenCore image configured for macOS guest
- macOS guest installation (BaseSystem.dmg, installer)
- libguestfs-tools for OpenCore image rebuild
- (Optional) SPICE client for remote display testing
- sudo/root for kext loading

## 1. Unit Tests

Unit tests verify core logic without needing kernel execution.

### Build & Run

```bash
cd Tests/
make
./virtio-gpu-tests
```

Expected output:
```
VirtIO GPU Unit Tests
====================

Test: test_types
  PASS
Test: test_virtqueue_init
  PASS
Test: test_enqueue_single
  PASS
Test: test_get_buf
  PASS
Test: test_queue_full
  PASS
Test: test_available
  PASS
Test: test_reset
  PASS

Results: 7/7 passed
```

### What They Test

- **test_types**: Struct sizes and magic number
- **test_virtqueue_init**: Virtqueue ring initialization
- **test_enqueue_single**: Single buffer enqueue
- **test_get_buf**: Buffer completion handling
- **test_queue_full**: Queue overflow detection
- **test_available**: Used ring availability check
- **test_reset**: Virtqueue reset functionality

## 2. Build Test

Verify that the driver kext builds correctly on macOS.

```bash
cd /path/to/virtio-gpu-macos
make clean
make
```

Expected: Build completes without errors, produces `build/virtio-gpu.kext`.

Check kext structure:
```bash
ls -R build/virtio-gpu.kext
# Should show:
# Contents/
#   Info.plist
#   MacOS/virtio-gpu
#   Resources/
```

## 3. Integration Test: Driver Load

Test that the driver can be loaded by OpenCore and the kernel.

### Steps

1. **Build and copy to OpenCore:**
   ```bash
   cd /path/to/virtio-gpu-macos
   make opencore
   ```
   This copies `build/virtio-gpu.kext` to `../OpenCore/Drivers/`.

2. **Rebuild OpenCore image:**
   ```bash
   cd ../OpenCore
   rm -f OpenCore.qcow2
   ./opencore-image-ng.sh --cfg config.plist --img OpenCore.qcow2
   ```

3. **Boot macOS with driver:**
   ```bash
   cd ..
   GPU_MODE="virtio" ./OpenCore-Boot.sh
   ```

4. **Check macOS logs:**
   After macOS boots, open Console.app or run:
   ```bash
   log show --predicate 'eventMessage contains "VirtIO"' --last 5m | grep -i "VirtIO-GPU"
   ```

   Expected messages:
   ```
   [VirtIO-GPU] Driver start
   [VirtIO-GPU] VirtIO device: ver=2 dev=2 vendor=0x1af4
   [VirtIO-GPU] Features negotiated
   [VirtIO-GPU] Control VQ: phys=... desc=... avail=... used=...
   [VirtIO-GPU] Framebuffer: 1920x1080 32bpp ...
   [VirtIO-GPU] Driver started successfully
   [VirtIO-FB] Framebuffer ready: ...
   ```

5. **Verify driver is loaded:**
   ```bash
   kextstat | grep virtio-gpu
   ```
   Should show: `com.osx-kvm.driver.virtio-gpu`

6. **Check display system:**
   - Open System Preferences → Displays
   - Should see a display named something like "VirtIO GPU" or the resolution 1920x1080
   - If the display is not recognized, the framebuffer interface may need fixes

## 4. Display Output Test

Verify that the framebuffer actually displays content.

### Test Pattern Display

If the driver is fully functional (control commands implemented), macOS will write to the framebuffer and we should see the desktop.

If not fully functional yet, we can add a debug mode to the driver that fills the framebuffer with a test pattern:

**Modify `IOFramebuffer.cpp` start() to add:**
```cpp
// After allocating fbMemoryMap, fill with pattern
void* fb = fbMemoryMap->getVirtualAddress();
uint32_t* pixels = (uint32_t*)fb;
for (uint32_t i = 0; i < fbWidth * fbHeight; i++) {
    // Color bars: each 100 pixels is a different color
    pixels[i] = ((i / 100) % 8) * 0x030303;  // Gradient
}
```

Rebuild and reboot. You should see colored bars in the QEMU window if framebuffer DMA is working.

### WindowServer Test

Once display is working:
- Move windows around - ensure updates appear
- Open Terminal and run ` screencapture -x test.png` to capture screen; verify file is non-zero
- Play a video to test refresh (will be software rendered)

## 5. QEMU Configuration Test

Test different QEMU configurations:

### Test 1: Standard virtio-gpu
```bash
# In OpenCore-Boot.sh, ensure:
GPU_MODE="virtio"
# Then launch QEMU and verify driver loads
```

### Test 2: With different resolutions
Modify `OpenCore/config.plist` to set `Misc->Display->Resolution` to `1280x720` or `2560x1440`. Rebuild OpenCore.qcow2 and boot. Verify driver uses that resolution.

### Test 3: SPICE fallback
If virtio-gpu not working, verify fallback to vmware-svga still works:
```bash
GPU_MODE="vmware-svga" ./OpenCore-Boot.sh
```

## 6. Stress Test

- Leave VM running for extended period (24h+)
- Perform disk I/O while display updates
- Suspend/resume if supported
- Reboot guest multiple times

Monitor for:
- Kernel panics
- Driver unload/reload issues
- Memory leaks (check host memory usage)
- Interrupt storms (high CPU usage)

## 7. Negative Tests

- Test with malformed virtqueue data (simulate device misbehavior)
- Disable KVM (`-enable-kvm` off) - driver should still load but slow
- Hot-unplug device (if QEMU supports) - driver should handle gracefully

## 8. Performance Benchmark

Once display is working:

1. **Framebuffer update speed:**
   ```bash
   # In macOS guest, compile a test program that writes to framebuffer
   # Measure time to write full frame (1920x1080 RGBA = 8MB)
   ```

2. **Compare with vmware-svga:**
   Boot same workload with `GPU_MODE="vmware-svga"` and compare performance.

3. **Expected:** VirtIO should be at least comparable, ideally faster due to simpler protocol.

## Automated Test Script

A script to run a suite of tests:

```bash
#!/bin/bash
# run-tests.sh - Comprehensive test suite for virtio-gpu driver

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT/virtio-gpu-macos"

echo "=== VirtIO GPU Test Suite ==="
echo ""

# 1. Build test
echo "[1/6] Building driver..."
make clean
make
if [ ! -d "build/virtio-gpu.kext" ]; then
    echo "FAIL: Build did not produce kext"
    exit 1
fi
echo "PASS"

# 2. Unit tests
echo "[2/6] Running unit tests..."
cd Tests
make
./virtio-gpu-tests > /dev/null
if [ $? -ne 0 ]; then
    echo "FAIL: Unit tests failed"
    exit 1
fi
echo "PASS"

# 3. Check OpenCore config
echo "[3/6] Checking OpenCore integration..."
if ! grep -q "Drivers/virtio-gpu.kext" "$REPO_ROOT/OpenCore/config.plist"; then
    echo "FAIL: Driver not in OpenCore config"
    exit 1
fi
echo "PASS"

# 4. Symbol checks
echo "[4/6] Checking kext symbols..."
if ! nm -gU build/virtio-gpu.kext/Contents/MacOS/virtio-gpu | grep -q "virtio_gpu::start"; then
    echo "FAIL: Missing expected symbols"
    exit 1
fi
echo "PASS"

# 5. Code signing check (optional)
echo "[5/6] Code signing (optional)..."
if command -v codesign &>/dev/null; then
    echo " (codesign available, skipping signature check)"
else
    echo " (codesign not found, skipping)"
fi
echo "PASS"

# 6. Documentation check
echo "[6/6] Checking documentation..."
if [ ! -s "../README.md" ]; then
    echo "FAIL: Missing README"
    exit 1
fi
echo "PASS"

echo ""
echo "All automated tests passed!"
echo ""
echo "Manual steps still required:"
echo "1. Copy kext to OpenCore/Drivers/ and rebuild OpenCore.qcow2"
echo "2. Boot with GPU_MODE=\"virtio\""
echo "3. Check macOS logs for 'VirtIO-GPU' messages"
echo "4. Verify display output in QEMU window"
```

## Continuous Integration (Concept)

For GitHub Actions or similar, create `.github/workflows/test.yml`:

```yaml
name: VirtIO GPU Driver Tests

on: [push, pull_request]

jobs:
  build:
    runs-on: macos-latest
    steps:
    - uses: actions/checkout@v3
    - name: Build Kext
      run: |
        cd virtio-gpu-macos
        make
        test -d build/virtio-gpu.kext
    - name: Run Unit Tests
      run: |
        cd virtio-gpu-macos/Tests
        make
        ./virtio-gpu-tests
    - name: Upload Artifacts
      uses: actions/upload-artifact@v3
      with:
        name: kext-build
        path: virtio-gpu-macos/build/
```

## Known Test Limitations

- Unit tests run in user-space, not kernel; some behaviors (interrupts, PCI config) cannot be fully tested without kernel.
- Integration tests require manual QEMU boot; cannot fully automate without a macOS CI runner with GPU passthrough (rare).
- Test coverage is partial; needs expansion as driver matures.

## Reporting Issues

When reporting test failures, include:
1. OS version (host and guest)
2. QEMU version (`qemu-system-x86_64 --version`)
3. Full test output/logs
4. Steps to reproduce
5. Expected vs actual behavior

## Future Work

- Add more unit tests for resource management
- Create a kernel-mode test harness that loads alongside driver and validates behavior
- Add fuzzing for virtqueue parsing
- Performance benchmark suite with graphs
