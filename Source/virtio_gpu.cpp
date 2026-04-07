/**
 * @file
 * VirtIO GPU Driver for macOS - Main Implementation
 *
 * This file contains the `virtio_gpu` IOService class which handles:
 * - PCI device detection and configuration
 * - VirtIO feature negotiation
 * - Virtqueue initialization and management
 * - Control command submission and response handling
 * - Resource management (framebuffers, textures)
 * - Interrupt processing
 * - Integration with IOFramebuffer subclass
 *
 * Status: Alpha - Core structure complete, needs testing
 *
 * @copyright Copyright (C) 2024 OSX-KVM Contributors
 * @license MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIConfigurator.h>
#include <IOKit/IOLock.h>
#include <libkern/c++/OSContainers.h>
#include "virtio_gpu.h"
#include "virtio_gpu_private.h"  // Internal declarations

#define DEBUG 1

#if DEBUG
#define GPU_LOG(fmt, ...) IOLog("[VirtIO-GPU] " fmt "\n", ##__VA_ARGS__)
#else
#define GPU_LOG(fmt, ...)
#endif

#define SAFE_RELEASE(obj) do { if (obj) { (obj)->release(); (obj) = nullptr; } } while(0)
#define LOCK(mutex) ((mutex)->lock())
#define UNLOCK(mutex) ((mutex)->unlock())

// PCI configuration space offsets
static const uint16_t kVendorIDOffset = 0x00;
static const uint16_t kDeviceIDOffset = 0x02;
static const uint16_t kCommandOffset = 0x04;
static const uint16_t kStatusOffset = 0x06;
static const uint16_t kClassOffset = 0x0A;

// VirtIO config space is via BAR0
static const uint16_t kBAR0Index = 0;

/**
 * @class virtio_gpu
 * @brief Main IOService driver for VirtIO GPU
 *
 * Lifecycle:
 * 1. probe() - matches on PCI vendor/device ID
 * 2. start() - allocates resources, initializes device, registers framebuffer
 * 3. stop() - tears down everything
 * 4. free() - final cleanup
 */
class virtio_gpu : public IOService
{
    OSDeclareDefaultStructors(virtio_gpu);

private:
    // PCI device
    IOPCIDevice*           pciDevice;
    IOMemoryMap*           configMap;          // Maps BAR0 (VirtIO registers)
    volatile VirtIORegion* virtioReg;          // Cached pointer to config space

    // IOKit plumbing
    IOLock*                lock;
    IOCommandGate*         cmdGate;
    IOWorkLoop*            workLoop;

    // VirtIO state
    virt_queue             controlVQ;          // Control queue (index 0)
    virt_queue             cursorVQ;           // Cursor queue (index 1), optional
    uint64_t               hostFeatures;       // Device capabilities
    uint64_t               driverFeatures;     // Accepted features
    bool                   featuresNegotiated;
    bool                   driverReady;

    // Interrupts
    IOInterruptEventSource* interruptSource;

    // Display configuration
    uint32_t               displayWidth;
    uint32_t               displayHeight;
    uint32_t               displayFormat;
    uint32_t               scanoutId;
    uint32_t               stride;

    // Framebuffer service
    virtio_gpu_framebuffer* framebuffer;

    // Resource tracking
    OSDictionary*          resources;          // resource_id -> FramebufferResource
    uint32_t               nextResourceId;
    uint64_t               nextFenceId;

    // Response tracking (simplified - should be a hash table keyed by fence_id)
    OSSet*                 pendingResponses;
    thread_call_t          responseThread;

public:
    // IOService overrides
    virtual bool start(IOService* provider) override;
    virtual void stop(IOService* provider) override;
    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual void free() override;

    // Power management
    virtual IOReturn setPowerState(unsigned long powerStateOrdinal, IOService* whichDevice) override;

    // Interrupt handling
    static void staticInterruptHandler(OSObject* target, void* refCon, IOInterruptEventSource* source, int intCount);
    void handleInterrupt();

    // Command submission
    IOReturn sendControlCommand(const void* cmd, uint32_t cmdSize, uint32_t respSize, uint64_t* fenceOut = nullptr);
    IOReturn waitForResponse(uint64_t fenceId, uint32_t timeoutMs = 5000);
    IOReturn sendControlCommandSync(const void* cmd, uint32_t cmdSize, uint32_t respSize, uint32_t timeoutMs = 5000);

    // Virtqueue setup
    bool initVirtqueues();
    void freeVirtqueues();
    bool notifyDevice(uint16_t queueIndex);
    bool mapVirtqueueMemory(virt_queue* vq, IOMemoryDescriptor* mem, uint64_t physAddr, uint16_t numDescriptors);

    // Device configuration
    bool negotiateFeatures();
    bool setDeviceStatus(uint8_t status);
    IOReturn getDisplayInfo();
    IOReturn setScanout(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
    IOReturn updateScanout(uint32_t scanoutId, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

    // Resource management
    uint32_t create2DResource(uint32_t format, uint32_t width, uint32_t height);
    bool attachResourceBacking(uint32_t resourceId, IOMemoryDescriptor* mem, uint32_t offset = 0);
    bool transferToHost2D(uint32_t resourceId, const struct virtio_gpu_rect* r, uint64_t offset);
    bool destroyResource(uint32_t resourceId);
    struct FramebufferResource* findResource(uint32_t resourceId);

    // Format conversion helpers
    static uint32_t formatToBpp(uint32_t format);
    static uint32_t formatToDepth(uint32_t format);
    static MacOSPixelFormat formatToMacOSFormat(uint32_t format);
    static uint32_t formatToComponentCount(uint32_t format);

    // Debug
    void dumpState() const;
};

/*===========================================================================
 *  IOService Implementation
 *===========================================================================*/

OSDefineMetaClassAndStructors(virtio_gpu, IOService);

bool virtio_gpu::start(IOService* provider)
{
    GPU_LOG("Driver start");

    // Call super
    if (!super::start(provider)) {
        GPU_LOG("super::start failed");
        return false;
    }

    // Get PCI device
    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        GPU_LOG("Provider is not IOPCIDevice");
        return false;
    }

    // Open the device (exclusive)
    if (!pciDevice->open(this)) {
        GPU_LOG("PCI device already opened by another driver");
        return false;
    }

    // Enable bus mastering for DMA
    uint16_t command = pciDevice->configRead16(kCommandOffset);
    command |= (kIOPCICommandMaster | kIOPCICommandMemorySpace);
    pciDevice->configWrite16(kCommandOffset, command);

    // Allocate lock for synchronization
    lock = IOLockAlloc();
    if (!lock) {
        pciDevice->close(this);
        return false;
    }

    // Get work loop
    workLoop = getWorkLoop();
    if (!workLoop) {
        IOLockFree(lock);
        pciDevice->close(this);
        return false;
    }

    // Create command gate
    cmdGate = IOCommandGate::commandGate(this);
    if (!cmdGate) {
        IOLockFree(lock);
        pciDevice->close(this);
        return false;
    }

    if (workLoop->addEventSource(cmdGate) != kIOReturnSuccess) {
        CHECK_AND_RELEASE(cmdGate);
        IOLockFree(lock);
        pciDevice->close(this);
        return false;
    }

    // Map VirtIO configuration space (BAR0)
    configMap = pciDevice->mapDeviceMemoryWithIndex(kBAR0Index, nullptr);
    if (!configMap) {
        GPU_LOG("Failed to map BAR0 (config space)");
        goto fail_cmdgate;
    }

    virtioReg = (volatile VirtIORegion*)configMap->getVirtualAddress();

    // Verify VirtIO magic
    if (virtioReg->magic != 0x53565256) {  // "VRT!" in little-endian
        GPU_LOG("Invalid VirtIO magic: 0x%08x", virtioReg->magic);
        goto fail_config;
    }

    GPU_LOG("VirtIO device: ver=%u dev=%u vendor=0x%x max_queue=%u",
            virtioReg->version, virtioReg->device_id, virtioReg->vendor_id,
            virtioReg->queue_max);

    if (virtioReg->device_id != VIRTIO_GPU_DEVICE_ID) {
        GPU_LOG("Not a GPU device (device_id=%u)", virtioReg->device_id);
        goto fail_config;
    }

    // Initialize driver state
    resources = OSDictionary::withCapacity(16);
    nextResourceId = 1;
    nextFenceId = 0;
    pendingResponses = OSSet::withCapacity(32);
    driverReady = false;
    featuresNegotiated = false;
    interruptsEnabled = false;
    framebuffer = nullptr;

    // Default display configuration (will be updated by GET_DISPLAY_INFO)
    displayWidth = 1920;
    displayHeight = 1080;
    displayFormat = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    stride = displayWidth * 4;  // 32bpp = 4 bytes per pixel
    scanoutId = 0;

    GPU_LOG("Negotiating features...");
    if (!negotiateFeatures()) {
        GPU_LOG("Feature negotiation failed");
        goto fail_dict;
    }

    GPU_LOG("Initializing virtqueues...");
    if (!initVirtqueues()) {
        GPU_LOG("Virtqueue initialization failed");
        goto fail_dict;
    }

    GPU_LOG("Setting device status to DRIVER_OK");
    if (!setDeviceStatus(VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK)) {
        GPU_LOG("Failed to set DRIVER_OK status");
        goto fail_vqs;
    }

    driverReady = true;
    GPU_LOG("Device ready");

    // Query display configuration
    GPU_LOG("Querying display info...");
    if (getDisplayInfo() != kIOReturnSuccess) {
        GPU_LOG("getDisplayInfo failed, using default %ux%u", displayWidth, displayHeight);
        // Continue anyway; defaults are set
    }

    // Create framebuffer service
    framebuffer = new virtio_gpu_framebuffer;
    if (!framebuffer || !framebuffer->init(nullptr)) {
        GPU_LOG("Framebuffer init failed");
        SAFE_RELEASE(framebuffer);
        goto fail_status;
    }

    if (!framebuffer->attach(this)) {
        GPU_LOG("Framebuffer attach failed");
        SAFE_RELEASE(framebuffer);
        goto fail_status;
    }

    if (!framebuffer->start(this)) {
        GPU_LOG("Framebuffer start failed");
        framebuffer->detach(this);
        SAFE_RELEASE(framebuffer);
        goto fail_status;
    }

    // Attach interrupt source
    interruptSource = pciDevice->createInterruptEventSource(
        virtio_gpu::staticInterruptHandler,
        this,
        0  // interrupt index
    );

    if (interruptSource && workLoop) {
        interruptSource->enable();
        workLoop->addEventSource(interruptSource);
        GPU_LOG("Interrupt source enabled");
    } else if (interruptSource) {
        interruptSource->release();
        interruptSource = nullptr;
        GPU_LOG("Warning: Failed to add interrupt source to work loop");
    }

    // Register as a service (makes it visible to IOKit)
    registerService();

    GPU_LOG("Driver started successfully");
    GPU_LOG("Framebuffer: %ux%u %ubpp stride=%u",
            displayWidth, displayHeight, formatToBpp(displayFormat), stride);

    return true;

fail_status:
    setDeviceStatus(VIRTIO_CONFIG_DEVICE_STATUS_FAILED);
    // Fall through to cleanup
fail_vqs:
    freeVirtqueues();
fail_dict:
    if (resources) resources->release();
    if (pendingResponses) pendingResponses->release();
    if (responseThread) {
        thread_call_cancel(responseThread);
        thread_call_free(responseThread);
    }
    CHECK_AND_RELEASE(configMap);
fail_config:
    workLoop->removeEventSource(cmdGate);
    CHECK_AND_RELEASE(cmdGate);
    IOLockFree(lock);
    pciDevice->close(this);
    return false;
fail_cmdgate:
    workLoop->removeEventSource(cmdGate);
    CHECK_AND_RELEASE(cmdGate);
    IOLockFree(lock);
    pciDevice->close(this);
    return false;
}

void virtio_gpu::stop(IOService* provider)
{
    GPU_LOG("Stopping driver");

    // Set failure status to tell device we're going away
    setDeviceStatus(VIRTIO_CONFIG_DEVICE_STATUS_FAILED);

    // Stop interrupt first
    if (interruptSource) {
        interruptSource->disable();
        workLoop->removeEventSource(interruptSource);
        interruptSource->release();
        interruptSource = nullptr;
    }

    // Cleanup framebuffer
    if (framebuffer) {
        framebuffer->stop(this);
        framebuffer->detach(this);
        framebuffer->free();
        framebuffer = nullptr;
    }

    // Cleanup virtqueues
    freeVirtqueues();

    // Cleanup command gate
    if (cmdGate && workLoop) {
        workLoop->removeEventSource(cmdGate);
    }
    CHECK_AND_RELEASE(cmdGate);

    // Cleanup locks
    if (lock) {
        IOLockFree(lock);
        lock = nullptr;
    }

    // Cleanup resources
    if (resources) {
        resources->release();
        resources = nullptr;
    }

    if (pendingResponses) {
        pendingResponses->release();
        pendingResponses = nullptr;
    }

    if (responseThread) {
        thread_call_cancel(responseThread);
        thread_call_free(responseThread);
        responseThread = nullptr;
    }

    // Cleanup config mapping
    if (configMap) {
        configMap->release();
        configMap = nullptr;
        virtioReg = nullptr;
    }

    // Close PCI device
    if (pciDevice) {
        pciDevice->close(this);
        pciDevice = nullptr;
    }

    super::stop(provider);
}

IOService* virtio_gpu::probe(IOService* provider, SInt32* score)
{
    GPU_LOG("Probing device");

    IOPCIDevice* dev = OSDynamicCast(IOPCIDevice, provider);
    if (!dev) {
        return nullptr;
    }

    // Check vendor and device ID
    uint16_t vendor = dev->configRead16(kVendorIDOffset);
    uint16_t device = dev->configRead16(kDeviceIDOffset);

    if (vendor != VIRTIO_GPU_VENDOR_ID || device != VIRTIO_GPU_DEVICE_ID) {
        GPU_LOG("Not our device: vendor=0x%04x device=0x%04x", vendor, device);
        return nullptr;
    }

    *score = 5000;  // High score for display driver
    GPU_LOG("Found VirtIO GPU device!");
    return this;
}

void virtio_gpu::free()
{
    GPU_LOG("Freeing driver");
    super::free();
}

/*===========================================================================
 *  Power Management
 *===========================================================================*/

IOReturn virtio_gpu::setPowerState(unsigned long powerStateOrdinal, IOService* whichDevice)
{
    // Display drivers typically don't power down
    return kIOReturnSuccess;
}

/*===========================================================================
 *  Feature Negotiation
 *===========================================================================*/

bool virtio_gpu::negotiateFeatures()
{
    LOCK(lock);

    // Read device feature bits
    hostFeatures = virtioReg->device_features;
    driverFeatures = 0;

    GPU_LOG("Device features: 0x%llx", hostFeatures);

    // Accept all device features we understand (limiting to 32-bit for legacy simplicity)
    driverFeatures = hostFeatures & 0xFFFFFFFFULL;

    GPU_LOG("Accepting features: 0x%llx", driverFeatures);

    // Write driver features (legacy: only low 32 bits)
    virtioReg->driver_features = (uint32_t)driverFeatures;
    virtioReg->driver_features_sel = 0;
    OSMemoryBarrier();

    // Set ACK
    uint8_t status = virtioReg->status;
    status |= VIRTIO_CONFIG_DEVICE_STATUS_ACK;
    virtioReg->status = status;
    OSMemoryBarrier();

    // Set DRIVER
    status |= VIRTIO_CONFIG_DEVICE_STATUS_DRIVER;
    virtioReg->status = status;
    OSMemoryBarrier();

    // For VirtIO 1.0+, set FEATURES_OK
    if (virtioReg->version >= 2) {
        status |= VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK;
        virtioReg->status = status;
        OSMemoryBarrier();

        if (!(virtioReg->status & VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK)) {
            GPU_LOG("Device rejected features");
            UNLOCK(lock);
            return false;
        }
    }

    featuresNegotiated = true;
    UNLOCK(lock);

    GPU_LOG("Feature negotiation complete");
    return true;
}

bool virtio_gpu::setDeviceStatus(uint8_t status)
{
    LOCK(lock);
    virtioReg->status = status;
    OSMemoryBarrier();
    UNLOCK(lock);

    GPU_LOG("Device status -> 0x%02x", status);
    return true;
}

/*===========================================================================
 *  Virtqueue Implementation
 *===========================================================================*/

bool virtio_gpu::initVirtqueues()
{
    // Determine queue size: device may provide max
    uint32_t queueSize = virtioReg->queue_max ? virtioReg->queue_max : 256;
    if (queueSize > 256) queueSize = 256;  // Cap at our static max
    if (queueSize < 2) {
        GPU_LOG("Device reports too small queue max: %u", queueSize);
        return false;
    }

    GPU_LOG("Using queue size %u", queueSize);

    // Calculate memory needed for one vq (desc + avail + used)
    size_t vqMemSize = VIRTQUEUE_SIZE(queueSize);
    GPU_LOG("VQ memory per queue: %lu bytes", vqMemSize);

    // Allocate for control queue
    IOMemoryDescriptor* ctrlMem = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryKernelUncached | kIOMemoryStatic,
        vqMemSize
    );

    if (!ctrlMem) {
        GPU_LOG("Failed to allocate control vq memory");
        return false;
    }

    if (!ctrlMem->prepare(kIODirectionInOut)) {
        GPU_LOG("Failed to prepare control vq memory");
        ctrlMem->release();
        return false;
    }

    // Zero the memory
    memset(ctrlMem->getBytesNoCopy(), 0, vqMemSize);

    // Split into regions
    uint8_t* base = (uint8_t*)ctrlMem->getBytesNoCopy();
    vring_desc* ctrlDesc = (vring_desc*)base;
    vring_avail* ctrlAvail = (vring_avail*)(base + sizeof(vring_desc) * queueSize);
    vring_used* ctrlUsed = (vring_used*)((uint8_t*)ctrlAvail + sizeof(vring_avail) + queueSize * sizeof(uint16_t));

    uint64_t ctrlPhys = ctrlMem->getPhysicalAddress();

    GPU_LOG("Control VQ: phys=0x%llx desc=%p avail=%p used=%p",
            ctrlPhys, ctrlDesc, ctrlAvail, ctrlUsed);

    // Initialize control virtqueue
    if (!init_virtqueue(&controlVQ, 0, ctrlDesc, ctrlAvail, ctrlUsed, queueSize)) {
        GPU_LOG("init_virtqueue failed for control");
        ctrlMem->complete();
        ctrlMem->release();
        return false;
    }

    controlVQ.physical_addr = ctrlPhys;
    controlVQ.memory_map = nullptr;
    controlVQ.memory_desc = ctrlMem;

    // Program device's queue configuration
    virtioReg->queue_sel = 0;
    OSMemoryBarrier();

    uint64_t pageSize = virtioReg->page_size ? virtioReg->page_size : 4096;
    uint32_t pfn = (uint32_t)(ctrlPhys / pageSize);

    GPU_LOG("Programming control queue: PFN=0x%08x (page_size=%llu)", pfn, pageSize);
    virtioReg->queue_pfn = pfn;
    OSMemoryBarrier();

    // Try cursor queue if device supports it
    if (virtioReg->queue_num_max >= 2) {
        IOMemoryDescriptor* curMem = IOBufferMemoryDescriptor::withOptions(
            kIOMemoryKernelUncached | kIOMemoryStatic,
            vqMemSize
        );

        if (curMem && curMem->prepare(kIODirectionInOut)) {
            memset(curMem->getBytesNoCopy(), 0, vqMemSize);

            uint8_t* curBase = (uint8_t*)curMem->getBytesNoCopy();
            vring_desc* curDesc = (vring_desc*)curBase;
            vring_avail* curAvail = (vring_avail*)(curBase + sizeof(vring_desc) * queueSize);
            vring_used* curUsed = (vring_used*)((uint8_t*)curAvail + sizeof(vring_avail) + queueSize * sizeof(uint16_t));
            uint64_t curPhys = curMem->getPhysicalAddress();

            if (init_virtqueue(&cursorVQ, 1, curDesc, curAvail, curUsed, queueSize)) {
                cursorVQ.physical_addr = curPhys;
                cursorVQ.memory_desc = curMem;

                virtioReg->queue_sel = 1;
                OSMemoryBarrier();
                uint32_t curPfn = (uint32_t)(curPhys / pageSize);
                virtioReg->queue_pfn = curPfn;
                OSMemoryBarrier();

                GPU_LOG("Cursor VQ initialized (phys=0x%llx)", curPhys);
            } else {
                GPU_LOG("Cursor VQ init failed (software)");
                curMem->complete();
                curMem->release();
            }
        }
    }

    return true;
}

void virtio_gpu::freeVirtqueues()
{
    if (controlVQ.memory_desc) {
        controlVQ.memory_desc->complete();
        controlVQ.memory_desc->release();
        controlVQ.memory_desc = nullptr;
    }
    memset(&controlVQ, 0, sizeof(controlVQ));

    if (cursorVQ.memory_desc) {
        cursorVQ.memory_desc->complete();
        cursorVQ.memory_desc->release();
        cursorVQ.memory_desc = nullptr;
    }
    memset(&cursorVQ, 0, sizeof(cursorVQ));

    GPU_LOG("Virtqueues freed");
}

/*===========================================================================
 *  Interrupt Handling
 *===========================================================================*/

void virtio_gpu::handleInterrupt()
{
    LOCK(lock);

    uint32_t isr = virtioReg->interrupt_status;
    virtioReg->interrupt_ack = isr;
    OSMemoryBarrier();

    GPU_LOG("Interrupt: ISR=0x%08x", isr);

    // Used ring update (bit 0)
    if (isr & 0x01) {
        if (virtqueue_available(&controlVQ)) {
            uint16_t descIdx;
            uint32_t respSize;
            while ((descIdx = virtqueue_get_buf(&controlVQ, &respSize)) != VIRTQUEUE_NO_FREE_DESC) {
                GPU_LOG("Control response: descIdx=%u len=%u", descIdx, respSize);
                // TODO: Match fence_id and wake waiters
            }
        }

        if (cursorVQ.memory_desc && virtqueue_available(&cursorVQ)) {
            uint32_t dummy;
            while (virtqueue_get_buf(&cursorVQ, &dummy) != VIRTQUEUE_NO_FREE_DESC) {
                GPU_LOG("Cursor VQ event");
            }
        }
    }

    // Configuration change (bit 1)
    if (isr & 0x02) {
        GPU_LOG("Configuration changed");
    }

    UNLOCK(lock);
}

void virtio_gpu::staticInterruptHandler(OSObject* target, void* refCon, IOInterruptEventSource* source, int intCount)
{
    virtio_gpu* self = (virtio_gpu*)target;
    if (self) {
        self->handleInterrupt();
    }
}

/*===========================================================================
 *  Control Command Interface
 *===========================================================================*/

IOReturn virtio_gpu::sendControlCommand(const void* cmd, uint32_t cmdSize, uint32_t respSize, uint64_t* fenceOut)
{
    if (!driverReady || !cmd) {
        return kIOReturnNotReady;
    }

    // Fence ID for this command
    uint64_t fenceId = nextFenceId++;

    // Allocate response buffer (device will write here)
    void* respBuffer = IOMalloc(respSize);
    if (!respBuffer) {
        return kIOReturnNoMemory;
    }
    memset(respBuffer, 0, respSize);

    LOCK(lock);

    // TODO: Proper implementation requires:
    // 1. Allocate DMA buffer for command (can't use stack, need physical address)
    // 2. Copy cmd to DMA buffer
    // 3. Build descriptor chain: [cmd buffer] -> [resp buffer]
    // 4. Enqueue on controlVQ
    // 5. Notify device via virtioReg->queue_notify = 0
    // 6. Track fenceId -> respBuffer mapping for wait
    // 7. Response handled in interrupt, waiter awakened

    GPU_LOG("sendControlCommand: type=0x%x fence=0x%llx (NOT IMPLEMENTED YET)",
            ((const uint32_t*)cmd)[0], fenceId);

    // Stub for now - pretend it worked
    if (fenceOut) {
        *fenceOut = fenceId;
    }

    UNLOCK(lock);
    IOFree(respBuffer, respSize);

    return kIOReturnSuccess;  // Placeholder
}

IOReturn virtio_gpu::sendControlCommandSync(const void* cmd, uint32_t cmdSize, uint32_t respSize, uint32_t timeoutMs)
{
    uint64_t fenceId;
    IOReturn ret = sendControlCommand(cmd, cmdSize, respSize, &fenceId);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    return waitForResponse(fenceId, timeoutMs);
}

IOReturn virtio_gpu::waitForResponse(uint64_t fenceId, uint32_t timeoutMs)
{
    // Simplified: just sleep a bit and return success
    // Real implementation would wait on a condition variable or semaphore
    // that is signaled by interrupt handler when fence completes.
    IOSleep(10);  // Tiny delay
    return kIOReturnSuccess;
}

/*===========================================================================
 *  Display Configuration
 *===========================================================================*/

IOReturn virtio_gpu::getDisplayInfo()
{
    struct virtio_gpu_ctrl_get_display_info cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    cmd.hdr.fence_id = nextFenceId++;

    GPU_LOG("Sending GET_DISPLAY_INFO");

    // Would parse response and update displayWidth/Height/scanoutCount
    // For now, keep defaults
    return kIOReturnSuccess;
}

/*===========================================================================
 *  Resource Management
 *===========================================================================*/

uint32_t virtio_gpu::create2DResource(uint32_t format, uint32_t width, uint32_t height)
{
    uint32_t bpp = formatToBpp(format);
    if (bpp == 0) {
        GPU_LOG("Invalid format: %u", format);
        return 0;
    }

    // Calculate stride (align to 64 bytes for DMA efficiency)
    uint32_t stride = ((width * bpp + 63) / 64) * 64;
    uint32_t size = stride * height;

    GPU_LOG("Creating 2D resource: %ux%u format=%u bpp=%u stride=%u size=%u",
            width, height, format, bpp, stride, size);

    // Allocate DMA memory
    IOMemoryDescriptor* mem = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryKernelUncached | kIOMemoryStatic,
        size
    );

    if (!mem) {
        GPU_LOG("Failed to allocate memory");
        return 0;
    }

    if (!mem->prepare(kIODirectionInOut)) {
        GPU_LOG("Failed to prepare memory");
        mem->release();
        return 0;
    }

    // Zero it
    void* va = mem->getBytesNoCopy();
    if (va) {
        memset(va, 0, size);
    }

    // Send RESOURCE_CREATE_2D command
    struct virtio_gpu_resource_create_2d cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd.hdr.fence_id = nextFenceId++;
    cmd.resource_id = nextResourceId;
    cmd.format = format;
    cmd.width = width;
    cmd.height = height;

    IOReturn ret = sendControlCommand(&cmd, sizeof(cmd), sizeof(struct virtio_gpu_ctrl_hdr_resp), nullptr);
    if (ret != kIOReturnSuccess) {
        GPU_LOG("RESOURCE_CREATE_2D failed");
        mem->complete();
        mem->release();
        return 0;
    }

    // Track resource
    FramebufferResource* res = (FramebufferResource*)IOMalloc(sizeof(FramebufferResource));
    if (!res) {
        mem->complete();
        mem->release();
        return 0;
    }
    memset(res, 0, sizeof(FramebufferResource));

    res->id = nextResourceId;
    res->format = format;
    res->width = width;
    res->height = height;
    res->stride = stride;
    res->memory = mem;
    res->size = size;
    res->handle = nextResourceId;

    // Store in dictionary with numeric key
    OSSymbol* key = OSSymbol::withNumber(nextResourceId);
    if (key) {
        resources->setObject(key, (OSObject*)res);
        key->release();
    }

    GPU_LOG("Created resource %u", nextResourceId);

    return nextResourceId++;
}

bool virtio_gpu::attachResourceBacking(uint32_t resourceId, IOMemoryDescriptor* mem, uint32_t offset)
{
    // TODO: Implement proper scatter-gather list
    GPU_LOG("attachResourceBacking: resource=%u (stub)", resourceId);
    return true;
}

bool virtio_gpu::transferToHost2D(uint32_t resourceId, const struct virtio_gpu_rect* r, uint64_t offset)
{
    struct virtio_gpu_transfer_to_host_2d cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd.hdr.fence_id = nextFenceId++;
    cmd.resource_id = resourceId;

    if (r) {
        cmd.r = *r;
    } else {
        cmd.r.x = 0;
        cmd.r.y = 0;
        cmd.r.width = displayWidth;
        cmd.r.height = displayHeight;
    }
    cmd.offset = offset;

    GPU_LOG("transferToHost2D: resource=%u rect=%ux%u+%d+%d",
            resourceId, cmd.r.width, cmd.r.height, cmd.r.x, cmd.r.y);

    IOReturn ret = sendControlCommand(&cmd, sizeof(cmd), sizeof(struct virtio_gpu_ctrl_hdr_resp), nullptr);
    return ret == kIOReturnSuccess;
}

bool virtio_gpu::destroyResource(uint32_t resourceId)
{
    // TODO
    GPU_LOG("destroyResource: %u (not implemented)", resourceId);
    return true;
}

struct FramebufferResource* virtio_gpu::findResource(uint32_t resourceId)
{
    OSSymbol* key = OSSymbol::withNumber(resourceId);
    if (!key) return nullptr;

    OSObject* obj = resources->getObject(key);
    key->release();

    return obj ? (FramebufferResource*)obj : nullptr;
}

/*===========================================================================
 *  Utilities
 *===========================================================================*/

uint32_t virtio_gpu::formatToBpp(uint32_t format)
{
    switch (format) {
        case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
        case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
            return 32;
        case VIRTIO_GPU_FORMAT_R5G6B5_UNORM:
            return 16;
        case VIRTIO_GPU_FORMAT_R8G8B8_UNORM:
            return 24;
        default:
            return 0;
    }
}

uint32_t virtio_gpu::formatToDepth(uint32_t format)
{
    return formatToBpp(format);
}

MacOSPixelFormat virtio_gpu::formatToMacOSFormat(uint32_t format)
{
    switch (format) {
        case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM: return 'BGRA';
        case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM: return 'ARGB';
        case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM: return 'RGBA';
        case VIRTIO_GPU_FORMAT_R5G6B5_UNORM:    return 'RGB6';
        default: return 'BGRA';
    }
}

uint32_t virtio_gpu::formatToComponentCount(uint32_t format)
{
    switch (format) {
        case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
            return 4;
        case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
            return 3;
        case VIRTIO_GPU_FORMAT_R5G6B5_UNORM:
            return 3;
        default:
            return 4;
    }
}

void virtio_gpu::dumpState() const
{
    IOLog("[VirtIO-GPU] State:\n");
    IOLog("  Device status: 0x%02x\n", virtioReg ? virtioReg->status : 0xFF);
    IOLog("  Features: negotiated=%s driver=0x%llx host=0x%llx\n",
          featuresNegotiated ? "yes" : "no", driverFeatures, hostFeatures);
    IOLog("  Display: %ux%u format=%u stride=%u\n", displayWidth, displayHeight, displayFormat, stride);
    IOLog("  Resources: %u\n", resources ? resources->getCount() : 0);
    IOLog("  Next resource ID: %u\n", nextResourceId);
    IOLog("  Next fence ID: 0x%llx\n", nextFenceId);
    IOLog("  Framebuffer service: %s\n", framebuffer ? "attached" : "none");
}
