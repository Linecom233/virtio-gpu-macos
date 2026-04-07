/**
 * @file
 * Private definitions for VirtIO GPU driver
 *
 * This file contains internal structures and forward declarations
 * used by the driver implementation but not exposed to other drivers.
 *
 * @copyright Copyright (C) 2024 OSX-KVM Contributors
 * @license MIT
 */

#ifndef VIRTIO_GPU_PRIVATE_H
#define VIRTIO_GPU_PRIVATE_H

#include "virtio_gpu.h"
#include "virtio_types.h"
#include <IOKit/IOLib.h>

/*===========================================================================
 *  Forward Declarations
 *===========================================================================*/

class virtio_gpu;
class virtio_gpu_framebuffer;

/*===========================================================================
 *  Resource Tracking
 *===========================================================================*/

/**
 * @struct FramebufferResource
 * @brief Tracks a GPU resource (framebuffer, texture, etc.)
 */
struct FramebufferResource {
    uint32_t        id;             ///< Resource ID (guest-assigned)
    uint32_t        format;         ///< Pixel format (virtio_gpu_formats)
    uint32_t        width;          ///< Width in pixels
    uint32_t        height;         ///< Height in pixels
    uint32_t        stride;         ///< Bytes per row (may be padded)
    uint64_t        handle;         ///< Device handle (may differ from id)
    IOMemoryDescriptor* memory;    ///< Backing memory descriptor
    IOByteCount     size;           ///< Size of memory in bytes
    uint32_t        refcount;       ///< Reference count
};

/*===========================================================================
 *  Virtqueue Software State
 *===========================================================================*/

/**
 * @struct virt_queue (from virtio_types.h)
 * @brief Driver's view of a virtqueue
 *
 * This structure extends the basic vring pointers with driver bookkeeping.
 */
#ifndef VIRT_QUEUE_STRUCT
#define VIRT_QUEUE_STRUCT
struct virt_queue {
    uint16_t               num;         ///< Number of descriptors
    volatile struct vring_desc* desc;     ///< Descriptor table (hardware)
    volatile struct vring_avail* avail;   ///< Available ring
    volatile struct vring_used* used;     ///< Used ring
    uint16_t               last_avail_idx; ///< Last consumed available index
    uint16_t               last_used_idx;  ///< Last consumed used index
    uint16_t               free_head;      ///< Head of free descriptor chain
    uint16_t               index;          ///< Queue index
    uint64_t               physical_addr;  ///< Physical base of entire ring memory
    void*                  virtual_addr;   ///< Virtual base of descriptor table
    IOMemoryMap*           memory_map;     ///< IOMemoryMap for cleanup
    IOMemoryDescriptor*    memory_desc;    ///< IOMemoryDescriptor (for release)
    IOLock*                lock;           ///< Per-queue lock (optional)
    uint32_t               pad[2];         ///< Padding/alignment
};
#endif

/*===========================================================================
 *  Helper Declarations
 *===========================================================================*/

#ifdef __cplusplus
extern "C" {
#endif

/* Virtqueue operations (implemented in virtqueue.cpp) */
bool init_virtqueue(virt_queue* vq, uint16_t index,
                    volatile vring_desc* desc,
                    volatile vring_avail* avail,
                    volatile vring_used* used,
                    uint16_t num);

void virtqueue_destroy(virt_queue* vq);
void virtqueue_reset(virt_queue* vq);
bool virtqueue_enqueue_single(virt_queue* vq, uint64_t addr, uint32_t len, bool write);
bool virtqueue_enqueue_multi(virt_queue* vq, uint64_t* addrs, uint32_t* lens, uint16_t count, bool write);
bool virtqueue_kick(virt_queue* vq, volatile uint32_t* notify_reg);
uint16_t virtqueue_get_buf(virt_queue* vq, uint32_t* out_len);
bool virtqueue_available(virt_queue* vq);
uint16_t virtqueue_peek_used(virt_queue* vq, uint32_t* out_len);
bool virtqueue_is_empty(virt_queue* vq);
void virtqueue_dump(virt_queue* vq);

/* Format conversion */
uint32_t formatToBpp(uint32_t format);
MacOSPixelFormat formatToMacOSFormat(uint32_t format);
uint32_t formatToComponentCount(uint32_t format);

/* Memory barrier wrapper (maps to OSMemoryBarrier in C++) */
static inline void virtio_mb() {
    OSMemoryBarrier();
}

#ifdef __cplusplus
}
#endif

#endif /* VIRTIO_GPU_PRIVATE_H */
