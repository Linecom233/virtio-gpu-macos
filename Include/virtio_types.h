/**
 * @file
 * Core VirtIO Types and Structures
 *
 * This file defines the fundamental VirtIO structures needed by the driver,
 * including the configuration space layout, virtqueue rings, and status bits.
 *
 * The VirtIO specification is managed by OASIS:
 * https://docs.oasis-open.org/virtio/virtio/v1.1/cs01/virtio-v1.1-cs01.html
 *
 * @copyright Copyright (C) 2024 OSX-KVM Contributors
 * @license MIT
 */

#ifndef VIRTIO_TYPES_H
#define VIRTIO_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 *  VIRTIO CONFIGURATION SPACE (PCI BAR)
 *===========================================================================*/

/**
 * @struct VirtIORegion
 * @brief Layout of VirtIO Device Configuration Region ( BAR0, 0x00-0x3F )
 *
 * This structure maps the first 64 bytes of the device's configuration
 * space (BAR0). This is the legacy layout; VirtIO 1.0 uses a slightly
 * different layout but this works for most implementations.
 *
 * The device appears as a standard PCI device with vendor=0x1AF4, device=0x1050.
 * After mapping BAR0, the driver can read/write these fields to control the
 * VirtIO device.
 *
 * Memory ordering: All registers are LE (little-endian) on x86. Use OSReadLittleInt32
 * and OSWriteLittleInt32 when accessing via IOMemoryMap to ensure correct byte swabbing
 * on big-endian platforms (not relevant for x86_64).
 *
 * @note All fields are 32-bit unless specified. Multi-byte fields may require
 *       two writes (high/low) for 64-bit values in legacy mode.
 */
struct VirtIORegion {
    /* Read-only device identification */
    uint32_t magic;             ///< Magic value: 0x53565256 ("VRT!" little-endian)
    uint32_t version;           ///< VirtIO version: 0x2 = legacy, 0x3 = 1.0, 0x10 = 1.1
    uint32_t device_id;         ///< Device type (2 = GPU)
    uint32_t vendor_id;         ///< Vendor ID (0x1AF4 = VirtIO)

    /* Feature bits (device -> guest) */
    uint32_t device_features;               ///< Lower 32 bits of device feature bits
    uint32_t device_features_sel;           ///< Device features selector (for 64-bit features)

    /* Driver feature negotiation (guest -> device) */
    uint32_t driver_features;              ///< Lower 32 bits of driver feature bits
    uint32_t driver_features_sel;          ///< Driver features selector (for 64-bit)

    /* Queue management */
    uint32_t queue_sel;                    ///< Queue number selected by driver (for queue operations)
    uint32_t queue_num_max;                ///< Maximum number of virtqueue elements supported
    uint32_t queue_num;                    ///< Queue size (number of descriptors) negotiated
    uint32_t queue_align;                  ///< Required alignment for descriptor table (usually 4096)
    uint32_t queue_pfn;                    ///< Page Frame Number (PFN) of descriptor table
    uint32_t queue_notify;                 ///< Queue notification (write: queue index to notify)

    /* Interrupts */
    uint32_t interrupt_status;             ///< Interrupt status (ISR - read to check)
    uint32_t interrupt_ack;                ///< Interrupt acknowledge (write to clear)

    /* Device status */
    uint32_t status;                       ///< Device status bits (VIRTIO_CONFIG_DEVICE_STATUS_*)

    /* Queue addresses (64-bit for VirtIO 1.0+; split into two 32-bit for legacy) */
    uint64_t queue_desc;                  ///< Physical address of descriptor table
    uint64_t queue_driver;                ///< Physical address of driver area (available ring)
    uint64_t queue_device;                ///< Physical address of device area (used ring)

    /* Configuration generation counter (for detecting config changes) */
    uint32_t config_generation;           ///< Incremented when config space changes

    /* Device-specific configuration space (starts here, size varies by device) */
    uint8_t config[104];                  ///< Device-specific config (GPU has specific fields)
};

/*===========================================================================
 *  STATUS BIT VALUES
 *===========================================================================*/

/** Device is in reset (initial state) */
#define VIRTIO_STATUS_RESET 0x00
/** Driver has noticed device */
#define VIRTIO_STATUS_ACK 0x01
/** Driver knows it's a virtio device */
#define VIRTIO_STATUS_DRIVER 0x02
/** Features negotiated successfully */
#define VIRTIO_STATUS_FEATURES_OK 0x04
/** Driver is set up and ready to receive interrupts */
#define VIRTIO_STATUS_DRIVER_OK 0x08
/** Device needs to be reset */
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x10
/** Device has fatal error */
#define VIRTIO_STATUS_FAILED 0x20
/** Optional: device has completed feature negotiation (1.1) */
#define VIRTIO_STATUS_FINAL_SOFTRESET 0x40

/*===========================================================================
 *  VIRTQUEUE STRUCTURES
 *===========================================================================*/

/**
 * @struct vring_desc
 * @brief One descriptor in the descriptor table
 *
 * Descriptors form chains. Each chain describes a buffer (or scatter-gather
 * list) that is either sent to the device (write=0) or received from the
 * device (write=VRING_DESC_F_WRITE). Chains are linked via the 'next' field.
 *
 * The driver adds descriptor chains to the available ring, device removes them
 * and places them in the used ring after completing I/O.
 *
 * @note Physical addresses must be 16-byte aligned. Use cache-inhibited mappings
 *       for proper DMA coherence.
 */
struct vring_desc {
    uint64_t addr;           ///< Physical address of buffer
    uint32_t len;            ///< Length of buffer in bytes
    uint16_t flags;          ///< Descriptor flags (see below)
    uint16_t next;           ///< Index of next descriptor in chain (or \c VRING_DESC_F_NEXT)
};

/* Descriptor flags */
#define VRING_DESC_F_NEXT    0x0001  ///< Next descriptor in chain
#define VRING_DESC_F_WRITE   0x0002  ///< Device writes to buffer (guest reads)
#define VRING_DESC_F_INDIRECT 0x0004 ///< Indirect descriptor chain (next points to another table)

/**
 * @struct vring_avail
 * @brief Available ring (guest-writes, device-reads)
 *
 * The driver writes descriptor chain indices into this ring to indicate
 * new available buffers. The device consumes them and places completed
 * descriptors in the used ring.
 *
 * Layout (little-endian):
 *   - flags: bit 0 = VIRTIO_RING_F_INDIRECT_DESC (unused usually)
 *   - idx: driver increments this as it adds descriptors
 *   - ring[]: array of descriptor table indices (each points to head of a chain)
 *   - (optional) used_event: for event index optimization (if VIRTIO_F_EVENT_IDX)
 */
struct vring_avail {
    uint16_t flags;          ///< Flags (currently only bit 0: VIRTIO_RING_F_INDIRECT_DESC)
    uint16_t idx;            ///< Driver-writes: index of next free ring entry
    uint16_t ring[];         ///< Variable-length array of descriptor indices
    // uint16_t used_event;   ///< Present if VIRTIO_RING_F_EVENT_IDX feature negotiated
};

/**
 * @struct vring_used_elem
 * @brief One entry in the used ring
 *
 * The device writes these after completing I/O on a descriptor chain.
 */
struct vring_used_elem {
    uint32_t id;             ///< Descriptor chain index (from available ring)
    uint32_t len;            ///< Total length written/read by device
};

/**
 * @struct vring_used
 * @brief Used ring (device-writes, guest-reads)
 *
 * The device writes completed descriptor indices here. The driver consumes
 * them to process completed I/O.
 *
 * Layout (little-endian):
 *   - flags: bit 0 = VIRTIO_RING_F_NO_NOTIFY (device doesn't want notifications)
 *   - idx: device increments as it adds used entries
 *   - ring[]: array of used_elem
 *   - (optional) avail_event: for event index optimization
 */
struct vring_used {
    uint16_t flags;          ///< Flags (bit 0 = VIRTIO_RING_F_NO_NOTIFY)
    uint16_t idx;            ///< Device-writes: index of next used entry
    uint16_t ring[];         ///< Variable-length array of vring_used_elem
    // uint16_t avail_event;   ///< Present if VIRTIO_RING_F_EVENT_IDX feature negotiated
};

/*===========================================================================
 *  VIRTQUEUE SOFTWARE STATE (Driver-private)
 *===========================================================================*/

/**
 * @struct virt_queue
 * @brief Software representation of a virtqueue
 *
 * This structure tracks the driver's state for a single virtqueue. It is
 * backed by three hardware rings (desc, avail, used) in physically contiguous
 * memory. The driver initializes this with init_virtqueue().
 */
struct virt_queue {
    uint16_t               num;         ///< Number of descriptors in this queue
    volatile struct vring_desc*  desc;      ///< Descriptor table (hardware ring)
    volatile struct vring_avail* avail;     ///< Available ring (driver writes, device reads)
    volatile struct vring_used*  used;      ///< Used ring (device writes, driver reads)
    uint16_t               last_avail_idx; ///< Last available index driver consumed from avail.idx
    uint16_t               last_used_idx;  ///< Last used index driver consumed from used.idx
    uint16_t               free_head;      ///< Head of free descriptor chain list
    uint16_t               index;          ///< Queue index (0, 1, ...) matches queue_sel
    uint64_t               physical_addr;  ///< Physical base address of entire queue memory
    void*                  virtual_addr;   ///< Virtual address of descriptor table
    IOMemoryMap*           memory_map;     ///< IOMemoryMap for cleanup
    IOMemoryDescriptor*    memory_desc;    ///< IOMemoryDescriptor backing the memory
    uint32_t               pad[3];         ///< Padding for cache line alignment
};

/*===========================================================================
 *  INTERRUPT STATUS BITS (read from interrupt_status register)
 *===========================================================================*/
#define VIRTIO_INT_CONFIG_CHANGE  0x00000001  ///< Configuration space changed
#define VIRTIO_INT_QUEUE_UPDATE   0x00000002  ///< Used ring updated (for that queue)
#define VIRTIO_INT_DEVICE_SPECIFIC 0x00000004 // Device-specific interrupt

/*===========================================================================
 *  HELPER MACROS
 *===========================================================================*/

/** Get the number of bytes needed for a virtqueue of 'num' descriptors */
#define VIRTQUEUE_SIZE(num) ( \
    sizeof(struct vring_desc) * (num) + \
    sizeof(struct vring_avail) + (num) * sizeof(uint16_t) + \
    sizeof(struct vring_used) + (num) * sizeof(uint16_t) + \
    3 * sizeof(uint16_t) /* alignment padding */ )

/** Memory barrier for VirtIO (use OSMemoryBarrier() in IOKit) */
#define virtio_mb() OSMemoryBarrier()

/** Little-endian read/write helpers (for portability, though x86 is LE) */
static inline uint32_t virtio_read32(volatile uint32_t* addr) {
    return *(volatile uint32_t*)addr;
}
static inline void virtio_write32(volatile uint32_t* addr, uint32_t val) {
    *(volatile uint32_t*)addr = val;
}

/** Check if device status bit is set */
#define virtio_status_check(status, bit) ((status) & (bit))
#define virtio_status_set(status, bit) ((status) |= (bit))
#define virtio_status_clear(status, bit) ((status) &= ~(bit))

/*===========================================================================
 *  C++ DECLARATIONS
 *===========================================================================*/

#ifdef __cplusplus
}

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

/**
 * @class VirtIODevice
 * @brief Base class for VirtIO devices (optional, for code reuse)
 *
 * This C++ class provides common VirtIO operations that can be inherited
 * by device-specific drivers (GPU, net, block, etc.).
 */
class VirtIODevice {
public:
    virtual ~VirtIODevice() {}

    /** Initialize the device after PCI mapping */
    virtual bool init() = 0;

    /** Reset the device */
    virtual void reset() = 0;

    /** Probe device features and negotiate with host */
    virtual bool negotiateFeatures(uint64_t deviceFeatures) = 0;

    /** Set device status and update hardware */
    virtual void setStatus(uint8_t status) = 0;

    /** Check if device is ready (DRIVER_OK) */
    virtual bool isReady() const = 0;
};

#endif /* __cplusplus */

#endif /* VIRTIO_TYPES_H */
