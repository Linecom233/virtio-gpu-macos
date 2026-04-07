/**
 * @file
 * VirtIO GPU IOFramebuffer Implementation
 *
 * This module provides the IOFramebuffer subclass that bridges the
 * virtio_gpu driver to the macOS display subsystem.
 *
 * The IOFramebuffer is responsible for:
 * - Providing framebuffer memory to be written by macOS graphics
 * - Reporting supported display modes
 * - Handling mode set requests
 * - Interfacing with WindowServer and other display clients
 *
 * @copyright Copyright (C) 2024 OSX-KVM Contributors
 * @license MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include "virtio_gpu_private.h"
#include "virtio_gpu.h"

#define super IOFramebuffer
OSDefineMetaClassAndStructors(virtio_gpu_framebuffer, IOFramebuffer);

/*===========================================================================
 *  Lifecycle
 *===========================================================================*/

bool virtio_gpu_framebuffer::init(OSDictionary* properties)
{
    if (!super::init(properties)) {
        return false;
    }

    driver = nullptr;
    fbMemoryMap = nullptr;
    fbMemoryDesc = nullptr;
    fbWidth = fbHeight = fbStride = fbDepth = fbTotalSize = 0;
    currentMode = nullptr;
    memset(&pixelInfo, 0, sizeof(pixelInfo));
    memset(&modeInfo, 0, sizeof(modeInfo));
    fbOffset = 0;

    return true;
}

void virtio_gpu_framebuffer::free()
{
    if (currentMode) {
        currentMode->release();
        currentMode = nullptr;
    }

    if (fbMemoryMap) {
        fbMemoryMap->release();
        fbMemoryMap = nullptr;
    }

    if (fbMemoryDesc) {
        fbMemoryDesc->complete();
        fbMemoryDesc->release();
        fbMemoryDesc = nullptr;
    }

    super::free();
}

bool virtio_gpu_framebuffer::start(IOService* provider)
{
    // Get driver instance
    driver = OSDynamicCast(virtio_gpu, provider);
    if (!driver) {
        IOLog("[VirtIO-FB] Provider is not virtio_gpu\n");
        return false;
    }

    // Get display configuration from driver
    fbWidth = driver->displayWidth;
    fbHeight = driver->displayHeight;
    uint32_t format = driver->displayFormat;
    uint32_t bpp = virtio_gpu::formatToBpp(format);
    if (bpp == 0) bpp = 32;  // Default to 32bpp

    fbStride = fbWidth * (bpp / 8);
    fbDepth = bpp;
    fbTotalSize = fbStride * fbHeight;
    fbOffset = 0;

    IOLog("[VirtIO-FB] Allocating framebuffer: %ux%u %ubpp stride=%llu total=%llu\n",
          fbWidth, fbHeight, fbDepth, fbStride, fbTotalSize);

    // Allocate framebuffer memory - must be DMA-capable
    fbMemoryDesc = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryKernelUncached | kIOMemoryStatic,
        fbTotalSize
    );

    if (!fbMemoryDesc) {
        IOLog("[VirtIO-FB] Failed to allocate framebuffer descriptor (OOM?)\n");
        return false;
    }

    if (!fbMemoryDesc->prepare(kIODirectionInOut)) {
        IOLog("[VirtIO-FB] Failed to prepare framebuffer for I/O\n");
        fbMemoryDesc->release();
        fbMemoryDesc = nullptr;
        return false;
    }

    // Create virtual mapping for driver use
    fbMemoryMap = fbMemoryDesc->map();
    if (!fbMemoryMap) {
        IOLog("[VirtIO-FB] Failed to map framebuffer memory\n");
        fbMemoryDesc->complete();
        fbMemoryDesc->release();
        fbMemoryDesc = nullptr;
        return false;
    }

    // Clear to black
    memset(fbMemoryMap->getVirtualAddress(), 0, fbTotalSize);

    // Populate pixel information structure
    pixelInfo.bytesPerRow = fbStride;
    pixelInfo.bytesPerPixel = (bpp + 7) / 8;
    pixelInfo.bitsPerPixel = bpp;
    pixelInfo.pixelFormat = virtio_gpu::formatToMacOSFormat(format);
    pixelInfo.isPlanar = false;
    pixelInfo.isIndexed = false;
    pixelInfo.isFloatingPoint = false;
    pixelInfo.isNativeEndian = true;
    pixelInfo.isAligned = true;
    pixelInfo.componentCount = virtio_gpu::formatToComponentCount(format);

    // Component descriptions (for BGRA: B at offset 0, G at 8, R at 16, A at 24)
    for (uint32_t i = 0; i < pixelInfo.componentCount; i++) {
        pixelInfo.components[i].size = 8;  // 8 bits per component
        pixelInfo.components[i].offset = i * 8;
        pixelInfo.components[i].unknown = 0;
    }
    pixelInfo.pixelSize = pixelInfo.bytesPerPixel;
    pixelInfo.pixelGroupSize = pixelInfo.bytesPerPixel;
    pixelInfo.pixelGroupCount = 1;

    // Populate mode info
    modeInfo.width = fbWidth;
    modeInfo.height = fbHeight;
    modeInfo.pixelInformation = &pixelInfo;
    modeInfo.refreshRate = 60;  // Default, can be adjusted if device reports
    modeInfo.interlaced = false;
    modeInfo.properties = nullptr;
    modeInfo.numProperties = 0;

    // Create IODisplayMode object representing our mode
    currentMode = IODisplayMode::withAttributes(
        (IOOptionBits)fbWidth,
        (IOOptionBits)fbHeight,
        (IOOptionBits)fbDepth,
        (IOByteCount)fbTotalSize,
        pixelInfo.pixelFormat,
        modeInfo.refreshRate,
        0  // flags
    );

    if (!currentMode) {
        IOLog("[VirtIO-FB] Failed to create IODisplayMode\n");
        return false;
    }

    GPU_LOG("Framebuffer ready: %llu bytes at %p",
           fbTotalSize, fbMemoryMap->getVirtualAddress());

    return true;
}

void virtio_gpu_framebuffer::stop(IOService* provider)
{
    GPU_LOG("Stpping framebuffer");

    if (currentMode) {
        currentMode->release();
        currentMode = nullptr;
    }

    if (fbMemoryMap) {
        fbMemoryMap->release();
        fbMemoryMap = nullptr;
    }

    if (fbMemoryDesc) {
        fbMemoryDesc->complete();
        fbMemoryDesc->release();
        fbMemoryDesc = nullptr;
    }

    super::stop(provider);
}

/*===========================================================================
 *  IOFramebuffer Required Methods
 *===========================================================================*/

IOReturn virtio_gpu_framebuffer::setDisplayMode(const IODisplayMode* newMode, IOOptionBits options)
{
    if (!newMode) {
        return kIOReturnBadArgument;
    }

    IOByteCount w = newMode->getAttribute(kIOFBWidth);
    IOByteCount h = newMode->getAttribute(kIOFBHeight);
    IOByteCount d = newMode->getAttribute(kIOFBDepth);

    IOLog("[VirtIO-FB] setDisplayMode requested: %lux%lu %lubpp (current: %ux%u %u)\n",
          w, h, d, fbWidth, fbHeight, fbDepth);

    // For now we only support a single fixed mode
    if (w == fbWidth && h == fbHeight && d == fbDepth) {
        // Already in this mode, nothing to do
        return kIOReturnSuccess;
    }

    // Tell driver to reconfigure scanout if we had a different mode
    // In future, could allocate new framebuffer of different size
    if (driver) {
        // driver->setScanout(...) would go here
    }

    return kIOReturnUnsupported;  // Mode change not yet supported
}

IODisplayMode* virtio_gpu_framebuffer::getCurrentDisplayMode()
{
    if (currentMode) {
        currentMode->retain();
    }
    return currentMode;
}

IODisplayMode* virtio_gpu_framebuffer::getFirstDisplayMode()
{
    return getCurrentDisplayMode();
}

IODisplayMode* virtio_gpu_framebuffer::getNextDisplayMode(const IODisplayMode* current)
{
    // Only one mode supported, no next
    return nullptr;
}

IODisplayModeInfo* virtio_gpu_framebuffer::getDisplayModeInfo(const IODisplayMode* mode, IOByteComp* options)
{
    if (!mode) {
        return nullptr;
    }

    // Must be our current mode
    if (mode != currentMode) {
        return nullptr;
    }

    if (options) {
        // Compose option word (documented in IOFramebuffer.h)
        // Bits: low 32 = total bytes, next 32 = width, next 32 = height
        uint64_t opt = (uint64_t)fbTotalSize | (((uint64_t)fbWidth) << 32) | (((uint64_t)fbHeight) << 48);
        *options = (IOByteComp)opt;
    }

    return &modeInfo;
}

IOByteCount virtio_gpu_framebuffer::getFramebufferSize()
{
    return fbTotalSize;
}

IOByteOffset virtio_gpu_framebuffer::getFramebufferOffset()
{
    return fbOffset;
}

IOMemoryDescriptor* virtio_gpu_framebuffer::getFrameBuffer(IOByteCount* length, IOByteOffset* offset)
{
    if (length) {
        *length = fbTotalSize;
    }
    if (offset) {
        *offset = fbOffset;
    }

    if (fbMemoryDesc) {
        fbMemoryDesc->retain();
    }

    return fbMemoryDesc;
}

IOByteCount virtio_gpu_framebuffer::getFramebufferAlignment()
{
    // 4096 is typical page size, but our allocation is page-aligned anyway
    return 4096;
}

IOByteCount virtio_gpu_framebuffer::getBytesPerRow()
{
    return fbStride;
}

IOReturn virtio_gpu_framebuffer::getAppleSense(double* sense)
{
    if (!sense) {
        return kIOReturnBadArgument;
    }
    *sense = 0.0;  // No sense line for virtual display
    return kIOReturnSuccess;
}

IOReturn virtio_gpu_framebuffer::setInterruptState(IOInterruptState state)
{
    // Interrupts are managed by the driver, not the framebuffer
    return kIOReturnSuccess;
}

IOReturn virtio_gpu_framebuffer::getCLUTEntry(IOClutShared* clut)
{
    // No color lookup table for truecolor modes
    return kIOReturnUnsupported;
}

IOReturn virtio_gpu_framebuffer::setCLUTEntry(IOClutShared* clut)
{
    return kIOReturnUnsupported;
}

IOReturn virtio_gpu_framebuffer::getGammaTable(const IOGammaTable* table)
{
    return kIOReturnUnsupported;  // No gamma correction yet
}

IOReturn virtio_gpu_framebuffer::getAttribute(const IOSelect attribute, void* value, IOByteCount* length)
{
    if (!value || !length) {
        return kIOReturnBadArgument;
    }

    switch (attribute) {
        case kIOFBMemoryMapAttribute:
            if (*length >= sizeof(IOMemoryMap*)) {
                if (fbMemoryMap) {
                    fbMemoryMap->retain();
                }
                *(IOMemoryMap**)value = fbMemoryMap;
                *length = sizeof(IOMemoryMap*);
                return kIOReturnSuccess;
            }
            break;

        case kIOFBCurrentDisplayModeAttribute:
            if (*length >= sizeof(IODisplayMode*)) {
                if (currentMode) {
                    currentMode->retain();
                }
                *(IODisplayMode**)value = currentMode;
                *length = sizeof(IODisplayMode*);
                return kIOReturnSuccess;
            }
            break;

        case kIOFBVramWidthAttribute:
            if (*length >= sizeof(IOByteCount)) {
                *(IOByteCount*)value = fbWidth * 8;  // Bits
                *length = sizeof(IOByteCount);
                return kIOReturnSuccess;
            }
            break;

        case kIOFBVramHeightAttribute:
            if (*length >= sizeof(IOByteCount)) {
                *(IOByteCount*)value = fbHeight;
                *length = sizeof(IOByteCount);
                return kIOReturnSuccess;
            }
            break;

        default:
            break;
    }

    return kIOReturnUnsupported;
}

IOReturn virtio_gpu_framebuffer::setAttribute(const IOSelect attribute, const void* value, IOByteCount length)
{
    // No settable attributes implemented yet
    return kIOReturnUnsupported;
}

/**
 * @function getVramPhysicalAddress
 * @brief Get physical address of framebuffer (if needed by driver)
 *
 * Some drivers need the physical address to program into hardware.
 * macOS doesn't directly expose this from IOMemoryDescriptor, but we can
 * get it via getPhysicalAddress() on the descriptor.
 *
 * @return Physical address, or 0 if not available
 */
IOPhysicalAddress virtio_gpu_framebuffer::getVramPhysicalAddress() const
{
    if (fbMemoryDesc) {
        return fbMemoryDesc->getPhysicalAddress();
    }
    return 0;
}

/**
 * @function getVramVirtualAddress
 * @brief Get virtual address for driver to write framebuffer
 */
void* virtio_gpu_framebuffer::getVramVirtualAddress() const
{
    if (fbMemoryMap) {
        return fbMemoryMap->getVirtualAddress();
    }
    return nullptr;
}
