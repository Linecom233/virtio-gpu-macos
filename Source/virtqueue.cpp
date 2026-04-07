/**
 * @file
 * Virtqueue Operations for VirtIO GPU Driver
 *
 * This module implements the low-level virtqueue ring management:
 * - Initialization of descriptor tables, available/used rings
 * - Adding buffers to available ring
 * - Retrieving used buffers from used ring
 * - Notifying the device
 *
 * The implementation follows the VirtIO 1.1 specification's ring layout.
 *
 * @copyright Copyright (C) 2024 OSX-KVM Contributors
 * @license MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "virtio_types.h"
#include "virtio_gpu_private.h"

#define DEBUG 1

#if DEBUG
#define VQ_LOG(fmt, ...) IOLog("[VQ] " fmt "\n", ##__VA_ARGS__)
#else
#define VQ_LOG(fmt, ...)
#endif

/*===========================================================================
 *  VIRTQUEUE INITIALIZATION
 *===========================================================================*/

/**
 * @function init_virtqueue
 * @brief Initialize a virtqueue software state and hardware rings
 *
 * This function sets up the virt_queue structure and initializes the
 * descriptor table, available ring, and used ring in memory. The memory
 * must already be allocated and zeroed by the caller.
 *
 * @param vq         Pointer to virt_queue structure (already allocated)
 * @param index      Queue index (0 for control, 1 for cursor)
 * @param desc       Virtual address of descriptor table
 * @param avail      Virtual address of available ring
 * @param used       Virtual address of used ring
 * @param num        Number of descriptors in this queue (must be power of 2, but not strictly enforced)
 *
 * @return true on success, false if parameters invalid
 *
 * @note The physical address must be set separately via vq->physical_addr
 *       before programming to device.
 */
bool init_virtqueue(virt_queue* vq, uint16_t index,
                    volatile vring_desc* desc,
                    volatile vring_avail* avail,
                    volatile vring_used* used,
                    uint16_t num)
{
    if (!vq || !desc || !avail || !used || num == 0) {
        VQ_LOG("init: invalid parameters");
        return false;
    }

    // Initialize structure
    vq->index = index;
    vq->num = num;
    vq->desc = desc;
    vq->avail = avail;
    vq->used = used;
    vq->last_avail_idx = 0;
    vq->last_used_idx = 0;
    vq->free_head = 0;
    vq->physical_addr = 0;
    vq->virtual_addr = (void*)desc;
    vq->memory_map = nullptr;
    vq->memory_desc = nullptr;

    // Initialize descriptor table as one big free chain
    for (uint16_t i = 0; i < num - 1; i++) {
        desc[i].addr = 0;
        desc[i].len = 0;
        desc[i].flags = 0;
        desc[i].next = i + 1;
    }
    // Last descriptor terminates chain
    desc[num - 1].addr = 0;
    desc[num - 1].len = 0;
    desc[num - 1].flags = 0;
    desc[num - 1].next = VIRTQUEUE_NO_FREE_DESC;

    // Initialize available ring
    avail->flags = 0;  // No interrupt suppression
    avail->idx = 0;    // Driver starts at index 0
    // ring[] entries are undefined until we add descriptors

    // Initialize used ring
    used->flags = 0;   // No interrupt suppression (device side)
    used->idx = 0;    // Device starts at index 0
    // ring[] entries will be filled by device

    // Ensure all writes complete before device sees the rings
    OSMemoryBarrier();

    VQ_LOG("Initialized vq%u: desc=%p avail=%p used=%p num=%u",
           index, desc, avail, used, num);

    return true;
}

/**
 * @function virtqueue_enable
 * @brief Enable notifications for a virtqueue
 *
 * After initializing the virtqueue and programming it to the device,
 * call this to start receiving interrupts for completed buffers.
 *
 * @param vq         Virtqueue to enable
 * @param enable     true to enable, false to disable
 *
 * @note This is typically a no-op for legacy interrupts; MSI-X uses
 *       a different mechanism via PCI config space.
 */
void virtqueue_enable(virt_queue* vq, bool enable)
{
    if (!vq) return;

    // For legacy INTx, the device always interrupts. For MSI-X, we'd
    // need to configure the MSI-X table entry for this queue.
    // For now, this is a placeholder.
    VQ_LOG("virtqueue_enable(vq%u, %s) - not fully implemented",
           vq->index, enable ? "true" : "false");
}

/*===========================================================================
 *  ENQUEUE OPERATIONS (Driver → Device)
 *===========================================================================*/

/**
 * @function virtqueue_alloc_descriptors
 * @brief Allocate a chain of descriptors from the free list
 *
 * @param vq         Virtqueue
 * @param needed     Number of descriptors needed in chain
 * @return Index of first descriptor, or VIRTQUEUE_NO_FREE_DESC if not enough free
 *
 * @note This is a simplified implementation that only handles linear chains.
 *       In production, you'd need to handle wrap-around more carefully.
 */
static uint16_t virtqueue_alloc_descriptors(virt_queue* vq, uint16_t needed)
{
    if (needed == 0) return VIRTQUEUE_NO_FREE_DESC;
    if (needed == 1) return vq->free_head;

    // Count available contiguous descriptors in the free chain
    uint16_t count = 0;
    uint16_t idx = vq->free_head;
    while (idx != VIRTQUEUE_NO_FREE_DESC && count < needed) {
        count++;
        idx = vq->desc[idx].next;
    }

    if (count < needed) {
        VQ_LOG("alloc_descriptors: need %u, only %u free", needed, count);
        return VIRTQUEUE_NO_FREE_DESC;
    }

    // Allocate the chain
    uint16_t head = vq->free_head;
    vq->free_head = idx;  // idx is now after the chain we allocated
    return head;
}

/**
 * @function virtqueue_enqueue_single
 * @brief Add a single buffer to the virtqueue
 *
 * This is the simplest enqueue operation: one buffer, one descriptor.
 * The buffer is added to the available ring after setting its physical
 * address and length.
 *
 * @param vq         Virtqueue
 * @param addr       Physical address of buffer (must be device-accessible)
 * @param len        Length of buffer in bytes
 * @param write      true if device writes to buffer (guest reads), false otherwise
 *
 * @return true if enqueued successfully, false if queue full
 *
 * @note This is a synchronous call that prepares the descriptor but does
 *       NOT notify the device. Call virtqueue_kick() afterward to signal.
 */
bool virtqueue_enqueue_single(virt_queue* vq, uint64_t addr, uint32_t len, bool write)
{
    if (!vq || addr == 0 || len == 0) {
        return false;
    }

    LOCK(vq->lock) {  // Need a lock in virt_queue struct for thread safety
        // Allocate one descriptor
        uint16_t head = vq->free_head;
        if (head == VIRTQUEUE_NO_FREE_DESC) {
            VQ_LOG("enqueue_single: vq%u full", vq->index);
            return false;
        }

        // Set up descriptor
        vring_desc* desc = &vq->desc[head];
        desc->addr = addr;
        desc->len = len;
        desc->flags = write ? VRING_DESC_F_WRITE : 0;
        desc->next = VIRTQUEUE_NO_FREE_DESC;

        // Add index to available ring
        uint16_t avail_idx = vq->avail->idx % vq->num;
        vq->avail->ring[avail_idx] = head;

        // Memory barrier: ensure ring entry is visible before updating idx
        OSMemoryBarrier();

        // Increment available index
        vq->avail->idx++;
        vq->free_head = desc->next;  // Advance free list

        VQ_LOG("enqueue_single: vq%u head=%u addr=0x%llx len=%u write=%d avail_idx=%u",
               vq->index, head, addr, len, write, avail_idx);
    }
    return true;
}

/**
 * @function virtqueue_enqueue_multi
 * @brief Enqueue multiple buffers as a scatter-gather chain
 *
 * This creates a descriptor chain with multiple segments, allowing
 * non-contiguous data to be presented to the device as one buffer.
 * Common for: command + response buffer pair, or fragmented data.
 *
 * @param vq         Virtqueue
 * @param addrs      Array of physical addresses (length = count)
 * @param lens       Array of lengths matching addrs
 * @param count      Number of buffers to chain
 * @param write      true if device writes to last buffer
 *
 * @return true if enqueued, false if insufficient descriptors
 *
 * @note The 'write' flag applies only to the last descriptor in the chain.
 *       The chain order is preserved by the device.
 */
bool virtqueue_enqueue_multi(virt_queue* vq, uint64_t* addrs, uint32_t* lens, uint16_t count, bool write)
{
    if (!vq || !addrs || !lens || count == 0) {
        return false;
    }

    LOCK(vq->lock) {
        // Allocate 'count' descriptors in a chain
        uint16_t head = virtqueue_alloc_descriptors(vq, count);
        if (head == VIRTQUEUE_NO_FREE_DESC) {
            VQ_LOG("enqueue_multi: vq%u not enough descriptors (need %u)", vq->index, count);
            return false;
        }

        // Build the chain
        uint16_t cur = head;
        for (uint16_t i = 0; i < count; i++) {
            vring_desc* desc = &vq->desc[cur];
            desc->addr = addrs[i];
            desc->len = lens[i];
            desc->flags = 0;

            // All except last have VRING_DESC_F_NEXT
            if (i < count - 1) {
                desc->flags |= VRING_DESC_F_NEXT;
                cur = desc->next;  // Get next from free list (which we allocated)
            } else {
                desc->next = VIRTQUEUE_NO_FREE_DESC;
                if (write) {
                    desc->flags |= VRING_DESC_F_WRITE;
                }
            }
        }

        // Add head to available ring
        uint16_t avail_idx = vq->avail->idx % vq->num;
        vq->avail->ring[avail_idx] = head;

        OSMemoryBarrier();
        vq->avail->idx++;

        VQ_LOG("enqueue_multi: vq%u head=%u count=%u", vq->index, head, count);
    }
    return true;
}

/**
 * @function virtqueue_kick
 * @brief Notify device that new buffers are available
 *
 * After enqueueing one or more buffers on the available ring, the driver
 * must write the queue index to the device's `queue_notify` register to
 * trigger processing. This is the actual "kick".
 *
 * @param vq         Virtqueue to kick
 * @param regNotify  Pointer to the device's queue_notify register (from BAR0)
 *
 * @return true if notification sent
 */
bool virtqueue_kick(virt_queue* vq, volatile uint32_t* regNotify)
{
    if (!vq || !regNotify) {
        return false;
    }

    // Memory barrier: ensure available ring writes complete before notification
    OSMemoryBarrier();

    // Write queue index to notify register
    *regNotify = vq->index;
    OSMemoryBarrier();

    VQ_LOG("kick: vq%u notification written", vq->index);
    return true;
}

/**
 * @function virtqueue_needs_notification
 * @brief Check if device wants notification (for event index optimization)
 *
 * When the VIRTIO_RING_F_EVENT_IDX feature is negotiated, the device can
 * suppress interrupts by setting the used_event field. This function checks
 * if a notification is needed based on that field.
 *
 * @param vq         Virtqueue
 * @return true if driver should send notification
 */
bool virtqueue_needs_notification(virt_queue* vq)
{
    if (!vq || !vq->used) return false;

    OSMemoryBarrier();
    uint16_t event_idx = vq->used->ring[vq->num].avail_event;  // field after ring[]
    uint16_t idx = vq->avail->idx;
    return ((int16_t)(idx - event_idx - 1) >= 0);
}

/*===========================================================================
 *  DEQUEUE OPERATIONS (Device → Driver)
 *===========================================================================*/

/**
 * @function virtqueue_get_buf
 * @brief Retrieve a used buffer from the virtqueue
 *
 * The driver calls this after the device signals completion (interrupt).
 * It checks the used ring for entries that the device has written.
 *
 * @param vq         Virtqueue
 * @param out_len    If non-NULL, receives length of buffer
 *
 * @return Descriptor index of the consumed buffer, or VIRTQUEUE_NO_FREE_DESC
 *         if no buffers are available.
 *
 * @note The returned descriptor index refers to the head of the chain that
 *       was originally enqueued. The driver should free the entire chain
 *       back to the free list after processing.
 *
 * @note This function does NOT remove the entry from the used ring; it just
 *       advances the driver's cursor. The entry is considered logically
 *       removed once consumed.
 */
uint16_t virtqueue_get_buf(virt_queue* vq, uint32_t* out_len)
{
    if (!vq || !vq->used) {
        return VIRTQUEUE_NO_FREE_DESC;
    }

    LOCK(vq->lock) {
        // Check if there are any new used entries
        uint16_t used_idx = vq->used->idx;
        if (used_idx == vq->last_used_idx) {
            // No new completions
            VQ_LOG("get_buf: vq%u no available used buffers", vq->index);
            return VIRTQUEUE_NO_FREE_DESC;
        }

        // Read the used ring entry
        uint16_t ring_idx = vq->last_used_idx % vq->num;
        vring_used_elem* elem = &vq->used->ring[ring_idx];

        uint16_t desc_idx = elem->id;
        uint32_t len = elem->len;

        // Advance our position
        vq->last_used_idx++;

        // Memory barrier before freeing descriptors
        OSMemoryBarrier();

        // Return the descriptor chain to the free list
        // In a full implementation, we'd walk the chain. For now assume single descriptor.
        vq->desc[desc_idx].next = vq->free_head;
        vq->free_head = desc_idx;

        if (out_len) {
            *out_len = len;
        }

        VQ_LOG("get_buf: vq%u desc_idx=%u len=%u last_used=%u used_idx=%u",
               vq->index, desc_idx, len, vq->last_used_idx, used_idx);
    }
    return desc_idx;
}

/**
 * @function virtqueue_available
 * @brief Check if there are completed buffers waiting in the used ring
 *
 * @param vq         Virtqueue
 *
 * @return true if at least one buffer is ready for consumption
 *
 * @note This is a non-consuming check; `virtqueue_get_buf()` actually removes.
 */
bool virtqueue_available(virt_queue* vq)
{
    if (!vq || !vq->used) return false;

    OSMemoryBarrier();
    return (vq->used->idx != vq->last_used_idx);
}

/**
 * @function virtqueue_consume_all
 * @brief Process all available used buffers
 *
 * Calls a callback for each completed buffer. Useful for interrupt handling
 * where we want to drain the ring completely.
 *
 * @param vq         Virtqueue
 * @param callback   Function to call for each buffer (passes descriptor index, length, arg)
 * @param arg        User argument passed to callback
 *
 * @return Number of buffers processed
 */
uint32_t virtqueue_consume_all(virt_queue* vq, bool (*callback)(uint16_t desc_idx, uint32_t len, void* arg), void* arg)
{
    if (!vq || !callback) return 0;

    uint32_t count = 0;

    LOCK(vq->lock) {
        while (virtqueue_available(vq)) {
            uint32_t len;
            uint16_t desc_idx = virtqueue_get_buf(vq, &len);
            if (desc_idx == VIRTQUEUE_NO_FREE_DESC) {
                break;
            }

            if (callback(desc_idx, len, arg)) {
                count++;
            }
        }
    }

    return count;
}

/*===========================================================================
 *  UTILITY FUNCTIONS
 *===========================================================================*/

/**
 * @function virtqueue_peek_used
 * @brief Look at the next used buffer without removing it
 *
 * Useful for checking if there's work without committing to consume.
 *
 * @param vq         Virtqueue
 * @param out_len    Returns length
 *
 * @return Descriptor index, or VIRTQUEUE_NO_FREE_DESC if none
 */
uint16_t virtqueue_peek_used(virt_queue* vq, uint32_t* out_len)
{
    if (!vq || !vq->used) return VIRTQUEUE_NO_FREE_DESC;

    OSMemoryBarrier();
    if (vq->used->idx == vq->last_used_idx) {
        return VIRTQUEUE_NO_FREE_DESC;
    }

    uint16_t ring_idx = vq->last_used_idx % vq->num;
    vring_used_elem* elem = &vq->used->ring[ring_idx];

    if (out_len) {
        *out_len = elem->len;
    }

    VQ_LOG("peek_used: vq%u desc=%u len=%u", vq->index, elem->id, elem->len);
    return elem->id;
}

/**
 * @function virtqueue_is_empty
 * @brief Check if virtqueue has no pending buffers
 *
 * Returns true if both available and used rings are empty, meaning the
 * driver has no outstanding requests.
 *
 * @param vq         Virtqueue
 * @return true if empty, false otherwise
 */
bool virtqueue_is_empty(virt_queue* vq)
{
    if (!vq) return true;

    LOCK(vq->lock) {
        return (vq->avail->idx == vq->last_avail_idx) &&
               (vq->used->idx == vq->last_used_idx);
    }
}

/**
 * @function virtqueue_get_ring_size
 * @brief Get the number of descriptors in this virtqueue
 */
uint16_t virtqueue_get_size(virt_queue* vq)
{
    return vq ? vq->num : 0;
}

/**
 * @function virtqueue_get_pending_count
 * @brief Count number of buffers driver has enqueued but not yet consumed
 *
 * This is the difference between avail.idx and last_used_idx (accounting for wrap).
 */
uint16_t virtqueue_get_pending_count(virt_queue* vq)
{
    if (!vq) return 0;

    LOCK(vq->lock) {
        uint16_t avail = vq->avail->idx;
        uint16_t used = vq->last_used_idx;
        // Handle wrap-around (queue size is power of 2 usually)
        return (avail - used);
    }
}

/*===========================================================================
 *  RESET AND REUSE
 *===========================================================================*/

/**
 * @function virtqueue_reset
 * @brief Reset virtqueue to initial state (clears all pending)
 *
 * This is typically called during device reset. It reclaims all descriptors
 * back to the free list and resets indices.
 *
 * @param vq         Virtqueue to reset
 */
void virtqueue_reset(virt_queue* vq)
{
    if (!vq) return;

    LOCK(vq->lock) {
        // Rebuild free list: all descriptors become free
        for (uint16_t i = 0; i < vq->num - 1; i++) {
            vq->desc[i].addr = 0;
            vq->desc[i].len = 0;
            vq->desc[i].flags = 0;
            vq->desc[i].next = i + 1;
        }
        vq->desc[vq->num - 1].next = VIRTQUEUE_NO_FREE_DESC;

        // Reset indices
        vq->last_avail_idx = 0;
        vq->last_used_idx = 0;
        vq->free_head = 0;

        // Clear hardware rings (optional, good for debugging)
        vq->avail->idx = 0;
        vq->used->idx = 0;
        vq->avail->flags = 0;
        vq->used->flags = 0;

        OSMemoryBarrier();

        VQ_LOG("reset: vq%u cleared", vq->index);
    }
}

/**
 * @function virtqueue_destroy
 * @brief Clean up virtqueue resources
 *
 * @param vq         Virtqueue to destroy (structure itself NOT freed)
 */
void virtqueue_destroy(virt_queue* vq)
{
    if (!vq) return;

    VQ_LOG("destroy: vq%u", vq->index);

    if (vq->memory_desc) {
        vq->memory_desc->complete();
        vq->memory_desc->release();
        vq->memory_desc = nullptr;
    }

    if (vq->memory_map) {
        vq->memory_map->release();
        vq->memory_map = nullptr;
    }

    memset(vq, 0, sizeof(virt_queue));
}

/*===========================================================================
 *  DIAGNOSTICS
 *===========================================================================*/

/**
 * @function virtqueue_dump
 * @brief Dump virtqueue state to console (debugging)
 */
void virtqueue_dump(virt_queue* vq)
{
    if (!vq) {
        IOLog("[VQ] NULL virtqueue\n");
        return;
    }

    IOLog("[VQ] VirtQueue %u:\n", vq->index);
    IOLog("  num = %u\n", vq->num);
    IOLog("  last_avail = %u\n", vq->last_avail_idx);
    IOLog("  last_used = %u\n", vq->last_used_idx);
    IOLog("  free_head = %u\n", vq->free_head);
    IOLog("  phys = 0x%llx\n", vq->physical_addr);
    IOLog("  avail.idx = %u\n", vq->avail ? vq->avail->idx : 0);
    IOLog("  used.idx = %u\n", vq->used ? vq->used->idx : 0);
    IOLog("  pending = %u\n", virtqueue_get_pending_count(vq));
}
