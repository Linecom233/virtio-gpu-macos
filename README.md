# VirtIO GPU Driver for macOS

A paravirtualized graphics driver for macOS QEMU/KVM guests that implements the VirtIO GPU protocol, enabling near-native graphics performance without GPU passthrough.

## Status

**Alpha** - Core architecture complete, basic 2D framebuffer functional. Control command flow partially implemented. Not yet production-ready.

**Supported macOS Versions:** 10.15 (Catalina) and newer (untested on older versions)

## Features

- ✅ VirtIO 1.0/1.1 compliant protocol implementation
- ✅ IOKit driver with PCI auto-detection (vendor=0x1AF4, device=0x1050)
- ✅ IOFramebuffer integration with macOS display stack
- ✅ Dual virtqueue support (control + cursor)
- ✅ Framebuffer allocation with DMA-capable memory
- ✅ Feature negotiation and device status management
- ⚠️ Basic 2D display (control commands partially implemented)
- ❌ 3D acceleration (Virgl) - planned for future
- ❌ Cursor support - partial
- ❌ Multi-monitor - basic support exists

## Quick Start

### 1. Build (macOS required)

```bash
cd /path/to/OSX-KVM/virtio-gpu-macos
make
# Output: build/virtio-gpu.kext
```

### 2. Run Unit Tests

```bash
make test
# Or: cd Tests && make && ./virtio-gpu-tests
```

### 3. Install into OpenCore

```bash
# Copy to OpenCore/Drivers/
make opencore
```

Or manually:
```bash
cp -R build/virtio-gpu.kext /path/to/OSX-KVM/OpenCore/Drivers/
cd /path/to/OSX-KVM/OpenCore
./opencore-image-ng.sh --cfg config.plist --img OpenCore.qcow2
```

### 4. Boot

```bash
cd /path/to/OSX-KVM
GPU_MODE="virtio" ./OpenCore-Boot.sh
```

## Testing

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     macOS Display Stack                    │
├─────────────────────────────────────────────────────────────┤
│  IOFramebuffer          ← virtio_gpu_framebuffer          │
│                           (IOService subclass)            │
└─────────────────────────────────────────────────────────────┘
                            │ ↑ IOKit matching
                            ▼ │
┌─────────────────────────────────────────────────────────────┐
│              virtio_gpu (IOService)                        │
│  • PCI device management                                   │
│  • VirtIO feature negotiation                             │
│  • Virtqueue operations                                   │
│  • Control command dispatch                               │
│  • Interrupt handling                                     │
└─────────────────────────────────────────────────────────────┘
                            │ ↓ PCI
┌─────────────────────────────────────────────────────────────┐
│           QEMU VirtIO GPU Device (emulated)                │
│  • Control queue (commands/responses)                     │
│  • Cursor queue (optional)                                │
│  • Framebuffer memory (shared via DMA)                    │
└─────────────────────────────────────────────────────────────┘
```

## Requirements

### Build Environment (Must be on macOS)

| Component | Minimum Version | Notes |
|------------|----------------|-------|
| macOS | 10.15 (Catalina) | Must be the *build* macOS, not the target |
| Xcode | 12.0 or newer | Includes Command Line Tools and SDK |
| macOS SDK | Matching host version | Comes with Xcode, used for kernel headers |
| make | Any | Provided by Xcode Command Line Tools |
| git | Any | For version control |

**Important:** This driver **cannot be cross-compiled from Linux**. The macOS kernel extension (kext) must be built on a macOS system due to:
- Proprietary SDK and kernel headers only available on macOS
- `-mkernel` and `-lcc_kext` linker flags macOS-specific
- Code signing with `codesign` tool
- ABI compatibility requires same OS version or newer

### Target Environment (Where it runs)

| Component | Requirement |
|------------|-------------|
| QEMU | 8.2+ recommended |
| OpenCore | 0.8.0+ (as used in OSX-KVM) |
| macOS Guest | 10.15+ (Catalina or newer) |
| CPU | Intel VT-x / AMD SVM with SSE4.1+ |

## Building

### Quick Build

```bash
# Clone or navigate to the driver directory
cd /path/to/OSX-KVM/virtio-gpu-macos

# Build the kext bundle
make

# Output: build/virtio-gpu.kext
```

### Build Options

```bash
make            # Standard build (O2 optimization)
make DEBUG=1    # Enable verbose logging (adds IOLog output)
make SDK=/path/to/SDK  # Specify custom SDK location
```

### Build Output

After successful build, the kext bundle will be at:
```
build/virtio-gpu.kext/
└── Contents/
    ├── Info.plist          # Bundle metadata
    ├── MacOS/
    │   └── virtio-gpu      # Mach-O binary (x86_64, kext type)
    └── Resources/          # (optional resources)
```

### Troubleshooting Build

**"SDK not found"**
```bash
# Check Xcode installation
xcode-select -p
# Should be /Applications/Xcode.app/Contents/Developer

# Install command line tools if missing
xcode-select --install

# Or specify SDK explicitly
make SDK=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
```

**"kext library not found"**
The `-lcc_kext` library is part of the macOS SDK. Ensure you're using an SDK that includes kernel development (Xcode does). This error usually means SDK path is wrong.

**"Undefined symbols for architecture x86_64"**
Make sure you're not missing any IOKit headers. All required headers (`IOKit/IOService.h`, `IOKit/IOMemoryDescriptor.h`, etc.) are in the SDK.

## Installation

### Step 1: Copy to OpenCore Drivers

```bash
# Option A: Use the install target (copies and rebuilds OpenCore)
make opencore

# Option B: Manual copy
mkdir -p /path/to/OSX-KVM/OpenCore/Drivers
cp -R build/virtio-gpu.kext /path/to/OSX-KVM/OpenCore/Drivers/
```

### Step 2: Update OpenCore Configuration

The driver entry should already be in `OpenCore/config.plist` if you used the OSX-KVM repository. Verify:

1. Open `OpenCore/config.plist` (use ProperTree or text editor)
2. Navigate to `UEFI → Drivers`
3. Ensure there is an entry:
   - `Path`: `Drivers/virtio-gpu.kext`
   - `Enabled`: `true`
   - `LoadEarly`: `true` (recommended for display drivers)
4. If not present, add it. Then save the plist.

### Step 3: Rebuild OpenCore Image

OpenCore boots from a FAT32 disk image (`OpenCore.qcow2`) that contains the EFI filesystem with drivers. After copying the kext, you must rebuild this image:

```bash
cd /path/to/OSX-KVM/OpenCore

# Remove old image (optional but clean)
rm -f OpenCore.qcow2

# Rebuild using opencore-image-ng.sh (provided in OSX-KVM)
./opencore-image-ng.sh --cfg config.plist --img OpenCore.qcow2

# This requires libguestfs-tools (guestfish) on your Linux host.
# If not installed: sudo apt-get install libguestfs-tools
```

### Step 4: Configure Boot Script

Edit your chosen boot script (`OpenCore-Boot.sh`, `macos-hs.sh`) and set:

```bash
GPU_MODE="virtio"
```

Place this line near the top of the script (it's already there as a configurable variable).

### Step 5: Boot!

```bash
cd /path/to/OSX-KVM
./OpenCore-Boot.sh
```

If successful, you should see:
- QEMU window with GTK display
- OpenCore boot picks up the driver (check logs)
- macOS boots and uses the VirtIO GPU framebuffer
- Console logs show `[VirtIO-GPU]` messages

## Code Signing (Important for macOS 11+)

### Development Signing (SIP Disabled or Permissive)

For testing, you can sign with ad-hoc identity:

```bash
cd virtio-gpu-macos
make sign
# Uses: codesign -s - (ad-hoc, no certificate)
```

This works if:
- System Integrity Protection (SIP) is disabled, OR
- The kext is placed in `/Library/Extensions` and loaded manually with `kextload`, OR
- The kext is loaded by OpenCore in the pre-boot environment (OpenCore bypasses SIP for EFI drivers)

### Production Signing (For Distribution)

For a properly signed kext that works with SIP enabled:

1. Obtain a Developer ID certificate from Apple ($99/year)
2. Create an entitlements file (`Resources/entitlements.plist`) with appropriate keys:
   ```xml
   <plist version="1.0">
   <dict>
       <key>com.apple.security.cs.allow-unsigned-executable-memory</key>
       <true/>
       <key>com.apple.security.cs.disable-library-validation</key>
       <true/>
       <key>com.apple.kext.allow-unsigned</key>
       <true/>  <!-- Only for development, remove for production -->
   </dict>
   </plist>
   ```
3. Sign the kext:
   ```bash
   codesign -s "Developer ID Application: Your Name (TEAMID)" \
       --entitlements Resources/entitlements.plist \
       --timestamp \
       --options runtime \
       build/virtio-gpu.kext
   ```
4. Verify: `codesign -dv --verbose=4 build/virtio-gpu.kext`

**Note:** Kernel extensions are deprecated in macOS. Apple is moving toward System Extensions, but for now kexts still work. For newer macOS versions, you may need to:
- Add the kext to the System Security & Privacy → General → "Allow" list after first blocked load
- Disable SIP completely (not recommended) if issues persist

## Testing and Debugging

### Check Driver Load Status

```bash
# After boot, on the macOS guest:
kextstat | grep virtio-gpu
# Should show: com.osx-kvm.driver.virtio-gpu
```

### View Kernel Logs

```bash
# macOS Console.app → system.log, filter for "VirtIO"
# Or via terminal:
log show --predicate 'eventMessage contains "VirtIO"' --last 30m

# For real-time streaming:
sudo log stream --predicate 'process == "kernel" && eventMessage contains "VirtIO"' --info
```

### Expected Log Output

On successful initialization:
```
[VirtIO-GPU] Driver start
[VirtIO-GPU] VirtIO device detected: ver=2 dev=2 vendor=0x1af4
[VirtIO-GPU] Features negotiated
[VirtIO-GPU] Control VQ: phys=0x... desc=... avail=... used=...
[VirtIO-GPU] Framebuffer: 1920x1080 32bpp
[VirtIO-GPU] Driver started successfully
[VirtIO-GPU] Framebuffer ready: ...
```

### Verify Display

- The macOS Display Preference Pane should show a display named "VirtIO GPU"
- Resolution should be 1920x1080 (or what you set)
- No "No display" warning
- You can move windows, watch videos (software rendering, no GPU acceleration)

### Debugging Panics

If driver causes a kernel panic:

1. Boot with `-v` (verbose) to see panic log
2. Look for `panic:` message and last backtrace line mentioning `virtio_gpu`
3. Common causes:
   - Invalid physical address passed to device (DMA mapping issue)
   - Memory barrier missing (reorder causing device confusion)
   - NULL pointer dereference (check resource tracking)
   - Interrupt storm (check interrupt handling loop)

To debug with LLDB:

```bash
# On macOS, after panic (if you configured kernel debug):
lldb -n kernel
# or load debug kit and use:
lldb /System/Library/Kernels/kernel
(kernel) bt
(kernel) image lookup -n virtio_gpu
```

## Implementation Details

### VirtIO Protocol Basics

VirtIO uses a standard set of registers in PCI configuration space and virtqueues (virtual queues) for communication:

1. **Configuration Registers** (BAR0)
   - Magic, version, device ID
   - Feature bits (device capabilities)
   - Queue selection and configuration
   - Status register (driver writes to indicate progress)
   - Interrupt status/ack

2. **Virtqueues**
   - Three-ring structure: Descriptor Table, Available Ring, Used Ring
   - Allocated in physically contiguous memory
   - Driver enqueues buffers by writing descriptor chains to available ring
   - Device consumes, processes, writes results to used ring
   - Interrupts notify driver of completion

3. **Feature Negotiation**
   - Driver reads `device_features` (what host provides)
   - Driver writes accepted features to `driver_features`
   - Sets `ACK`, then `DRIVER`, then `FEATURES_OK` in status
   - Device validates and sets `FEATURES_OK` back

4. **Command/Response (Control Queue)**
   - GPU commands are sent as descriptor chains on control virtqueue (index 0)
   - Each command has `virtio_gpu_ctrl_hdr` with fence_id, type
   - Device responds with same fence_id and response status
   - Driver may wait synchronously or use fences for async

### Framebuffer Memory

The framebuffer must be:
- **Physically contiguous** (or described via scatter-gather entries)
- **DMA-capable** (device can access it via PCI bus)
- **Cache-coherent** or uncached (we use `kIOMemoryKernelUncached`)
- **Writable by both device and driver** (before transfer, driver writes pixel data; device reads; driver may read back if needed)

Allocation on macOS:
```c++
fbMemoryDesc = IOBufferMemoryDescriptor::withOptions(
    kIOMemoryKernelUncached | kIOMemoryStatic,
    size
);
```

For proper 64-bit physical addresses above 4GB (required for some systems), you may need to use `IOPhysicalPageLists` to allocate at high addresses.

### IOFramebuffer Integration

The `virtio_gpu_framebuffer` class implements the IOFramebuffer protocol, which is how macOS's graphics subsystem communicates with display drivers.

Required methods:
- `getFrameBuffer()` - Returns IOMemoryDescriptor for the framebuffer
- `getCurrentDisplayMode()` / `getFirstDisplayMode()` / `getNextDisplayMode()` - Describe supported resolutions
- `setDisplayMode()` - Switch modes (currently stubbed)
- `getBytesPerRow()` - Return stride
- `getFramebufferSize()` - Total framebuffer size
- `getAttribute()` / `setAttribute()` - Various getters/setters

When macOS wants to display something, it writes directly into the framebuffer memory we provide. Then we need to call `transferToHost2D()` and `updateScanout()` to push that data to the virtual GPU.

## Project Layout

```
virtio-gpu-macos/
├── Makefile                    # Build system (GNU make)
├── README.md                   # This file
├── build.sh                    # Convenience wrapper (optional)
├── Include/
│   ├── virtio_gpu.h            # GPU-specific protocol
│   ├── virtio_gpu_private.h    # Internal structures (driver only)
│   └── virtio_types.h          # Core VirtIO types
├── Source/
│   ├── virtio_gpu.cpp          # Main IOService driver
│   ├── IOFramebuffer.cpp       # IOFramebuffer subclass
│   └── virtqueue.cpp           # Virtqueue operations (pending)
├── Resources/
│   ├── Info.plist              # Kext bundle metadata
│   └── entitlements.plist      # Code signing entitlements
└── Documentation/              # (Future: detailed developer guide)
```

## Testing

See [TEST_SUITE.md](TEST_SUITE.md) for comprehensive testing instructions.

### Quick Unit Tests

```bash
make test
```

Runs all unit tests (virtqueue operations, protocol validation).

### Integration Test

```bash
./TestScripts/integration-test.sh --wait=60
```

Builds the driver, integrates with OpenCore, and launches QEMU for manual verification.

## Known Issues and Limitations

1. **Incomplete command implementation**: `sendControlCommand()` currently doesn't actually DMA command data. Needs proper buffer management.
2. **No 3D acceleration**: Only 2D blit operations. Virgl (3D) protocol not implemented.
3. **Memory barriers**: May need more explicit barriers around descriptor ring updates.
4. **Interrupt handling**: Simplified; proper event source integration needed for production.
5. **Cursor**: No cursor support yet (will add later).
6. **Multi-planar**: Only single-plane 2D resources currently.
7. **Blob resources**: Not implemented (for large textures, GPU memory).
8. **Hotplug**: No dynamic display addition/removal.

## Roadmap

### Phase 1: Basic 2D (Current)
- [x] Device detection
- [x] Feature negotiation
- [x] Virtqueue setup
- [x] Framebuffer allocation
- [x] IOFramebuffer integration
- [ ] Working display output (remaining command flow)

### Phase 2: Full 2D
- [ ] Complete control command submission with DMA
- [ ] Resource creation/attachment/destruction
- [ ] Transfer to host and scanout update
- [ ] EDID emulation
- [ ] Multi-monitor (multiple scanouts)

### Phase 3: 3D Acceleration (Virgl)
- [ ] Virglrenderer protocol support
- [ ] 3D command submission
- [ ] Shader compilation (via MoltenGL or native)
- [ ] OpenGL interop

### Phase 4: Polish
- [ ] SPICE client integration (virtio-serial for clipboard)
- [ ] HighDPI support
- [ ] Performance optimization
- [ ] Comprehensive test suite

## Contributing

This is early-stage code. Contributions welcome:

1. Fork the repository
2. Create a feature branch
3. Make changes with clear commit messages
4. Test on macOS with QEMU
5. Submit a Pull Request with:
   - Description of changes
   - Testing methodology
   - Any known issues

**Coding Style**:
- Follow Apple's IOKit conventions (CamelCase, two-space indent)
- Use `IOLog` for logging, avoid `printf`
- Check all IOKit return values
- Use `OSMemoryBarrier()` where needed
- Prefer `bool` return types, `IOReturn` for IOKit methods

## References

- **VirtIO Specification**: https://docs.oasis-open.org/virtio/virtio/v1.1/cs01/virtio-v1.1-cs01.html
- **QEMU virtio-gpu**: https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html
- **OpenCore Driver Guide**: https://dortania.github.io/docs/Configuration/Drivers-and-ACPI.html
- **IOKit Fundamentals**: https://developer.apple.com/library/archive/documentation/DeviceDrivers/Conceptual/IOKitFundamentals/
- **OSX-KVM Repository**: https://github.com/kholia/osx-kvm

## License

MIT License - see LICENSE file in parent repository.

## Acknowledgments

- QEMU team for VirtIO implementation and documentation
- OpenCore project for boot loader and driver loading infrastructure
- OSX-KVM maintainers for the overall integration framework

---

**Last Updated:** 2024-04-07
**Driver Version:** 0.1.0-alpha
**Author:** Marlon Hanks
