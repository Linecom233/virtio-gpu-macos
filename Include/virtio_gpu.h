/**
 * @file
 * VirtIO GPU Protocol Definitions for macOS
 *
 * This header defines the VirtIO GPU protocol as implemented by QEMU and
 * specified in the VirtIO 1.1 specification.
 *
 * The driver implements a macOS kernel extension (kext) that provides
 * paravirtualized graphics for macOS guests running under QEMU/KVM.
 *
 * References:
 * - VirtIO 1.1 Spec: https://docs.oasis-open.org/virtio/virtio/v1.1/cs01/virtio-v1.1-cs01.html
 * - QEMU virtio-gpu: https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html
 * - macOS IOKit: https://developer.apple.com/documentation/iokit
 *
 * @copyright Copyright (C) 2024 OSX-KVM Contributors
 * @license MIT
 */

#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>
#include <IOKit/IOService.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 *  VIRTIO CONSTANTS
 *===========================================================================*/

/** VirtIO GPU Device ID (from spec) */
#define VIRTIO_GPU_DEVICE_ID 2

/** VirtIO Vendor ID for VirtIO devices (0x1AF4) */
#define VIRTIO_GPU_VENDOR_ID 0x1AF4

/** VirtIO GPU PCI Class/Subclass */
#define VIRTIO_GPU_PCI_CLASS 0x03    // Display controller
#define VIRTIO_GPU_PCI_SUBCLASS 0x00

/*===========================================================================
 *  DEVICE STATUS BITS (VirtIO 1.1 Section 4.1.4.3.1)
 *===========================================================================*/
#define VIRTIO_CONFIG_DEVICE_STATUS_RESET              0x00
#define VIRTIO_CONFIG_DEVICE_STATUS_ACK               0x01
#define VIRTIO_CONFIG_DEVICE_STATUS_DRIVER            0x02
#define VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK         0x04
#define VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK       0x08
#define VIRTIO_CONFIG_DEVICE_STATUS_DEVICE_NEEDS_RESET 0x10
#define VIRTIO_CONFIG_DEVICE_STATUS_FAILED            0x20
#define VIRTIO_CONFIG_DEVICE_STATUS_FINAL_SOFTRESET   0x40  // VirtIO 1.1+

/*===========================================================================
 *  FEATURE BITS
 *===========================================================================*/
/** Bit 0: Device supports Virgl (3D acceleration) */
#define VIRTIO_GPU_F_VIRGL 0
/** Bit 1: Device supports EDID emulation */
#define VIRTIO_GPU_F_EDID  1
/** Bit 2: Device supports multiplanar surfaces */
#define VIRTIO_GPU_F_MULTIPLANE 2

/*===========================================================================
 *  COMMAND TYPES
 *===========================================================================*/
/**
 * Control queue commands (device <- guest)
 * Values 0x0100-0x01FF are defined for GPU control commands
 */
enum virtio_gpu_ctrl_cmd {
    /* GET commands */
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_GET_EDID = 0x0101,

    /* SET commands */
    VIRTIO_GPU_CMD_SET_SCANOUT = 0x0200,
    VIRTIO_GPU_CMD_UPDATE_SCANOUT = 0x0201,
    VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0202,
    VIRTIO_GPU_CMD_MOVE_CURSOR = 0x0203,
    VIRTIO_GPU_CMD_UPDATE_CURSOR_BLOB = 0x0204,

    /* Resource management */
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D = 0x0300,
    VIRTIO_GPU_CMD_RESOURCE_DESTROY = 0x0301,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0302,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0303,
    VIRTIO_GPU_CMD_RESOURCE_ASSIGN_ID = 0x0304,    // Legacy
    VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB = 0x0305,
    VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB = 0x0306,
    VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB = 0x0307,

    /* Transfer operations */
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D = 0x0400,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D = 0x0401,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_2D = 0x0402,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D = 0x0403,

    /* Fence operations */
    VIRTIO_GPU_CMD_CTRL_CREATE_FENCE = 0x0500,
    VIRTIO_GPU_CMD_CTRL_PUSH_FENCE_TO_HOST = 0x0501,
    VIRTIO_GPU_CMD_CTRL_POLL_FENCE = 0x0502,

    /* Context operations */
    VIRTIO_GPU_CMD_CTXL_CREATE = 0x0600,
    VIRTIO_GPU_CMD_CTXL_DESTROY = 0x0601,
    VIRTIO_GPU_CMD_CTXL_ATTACH_RESOURCE = 0x0602,
    VIRTIO_GPU_CMD_CTXL_DETACH_RESOURCE = 0x0603,

    /* Internal commands (for testing/debug) */
    VIRTIO_GPU_CMD_GET_CAPSET_INFO = 0x0700,
    VIRTIO_GPU_CMD_GET_CAPSET = 0x0701,

    /* Maximum command value */
    VIRTIO_GPU_CMD_MAX
};

/*===========================================================================
 *  RESPONSE TYPES
 *===========================================================================*/
enum virtio_gpu_ctrl_type {
    VIRTIO_GPU_RESPONSE_OK = 0x3100,
    VIRTIO_GPU_RESPONSE_ERROR_UNSPEC = 0x3200,
    VIRTIO_GPU_RESPONSE_ERROR_OUT_OF_MEMORY,
    VIRTIO_GPU_RESPONSE_ERROR_INVALID_SCANOUT_ID,
    VIRTIO_GPU_RESPONSE_ERROR_INVALID_RESOURCE_ID,
    VIRTIO_GPU_RESPONSE_ERROR_INVALID_CONTEXT_ID,
    VIRTIO_GPU_RESPONSE_ERROR_INVALID_PARAMETER,
    VIRTIO_GPU_RESPONSE_ERROR_UNSUPPORTED_FEATURE,
    VIRTIO_GPU_RESPONSE_ERROR_UNSUPPORTED_FORMAT,
    VIRTIO_GPU_RESPONSE_ERROR_UNSUPPORTED_RESOURCE,
};

/*===========================================================================
 *  RESOURCE FORMATS (Pixel Formats)
 *===========================================================================*/
enum virtio_gpu_formats {
    /* 32-bit formats */
    VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 0,   ///< BGRA (little-endian)
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 1,   ///< BGRX
    VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 2,   ///< ARGB (Windows-style)
    VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 3,   ///< XRGB
    VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM = 4,   ///< RGBA
    VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM = 5,   ///< RGX
    VIRTIO_GPU_FORMAT_R16G16B16A8_FLOAT = 6,
    VIRTIO_GPU_FORMAT_R32G32B32A8_FLOAT = 7,
    VIRTIO_GPU_FORMAT_R8G8B8_UNORM = 8,     ///< 24-bit RGB
    VIRTIO_GPU_FORMAT_R5G6B5_UNORM = 9,     ///< 16-bit RGB565
    VIRTIO_GPU_FORMAT_R10G10B10A2_UNORM = 10,

    /* YUV planar formats */
    VIRTIO_GPU_FORMAT_NV12 = 0x100,         ///< YUV 4:2:0 planar
    VIRTIO_GPU_FORMAT_YUV420,
    VIRTIO_GPU_FORMAT_YUYV,
    VIRTIO_GPU_FORMAT_UYVY,

    /* Vendor-specific extensions may use higher values */
};

/*===========================================================================
 *  BLOB MEMORY TYPES
 *===========================================================================*/
#define VIRTIO_GPU_BLOB_MEM_GUEST   0x0001  ///< Memory in guest RAM
#define VIRTIO_GPU_BLOB_MEM_HOST3D  0x0002  ///< Host 3D space (GPU memory)
#define VIRTIO_GPU_BLOB_MEM_HOST2D  0x0004  ///< Host 2D space
#define VIRTIO_GPU_BLOB_FLAG_USE_MAPPING 0x0001  ///< Blob uses a mapping

/*===========================================================================
 *  STRUCTURES (Packed per VirtIO spec)
 *===========================================================================*/
#pragma pack(push, 1)

/**
 * @struct virtio_gpu_ctrl_hdr
 * @brief Base header for all control commands and responses
 *
 * All commands and responses on the control queue start with this header.
 * The header is followed by command-specific payload.
 */
struct virtio_gpu_ctrl_hdr {
    uint32_t type;          ///< Command or response type (virtio_gpu_ctrl_cmd / virtio_gpu_ctrl_type)
    uint32_t flags;         ///< Command flags (currently unused, must be 0)
    uint64_t fence_id;      ///< Fence ID for async completion tracking
    uint32_t ctx_id;        ///< Context ID (0 for default context)
    uint32_t padding[3];    ///< Padding to 32 bytes
};

/**
 * @struct virtio_gpu_ctrl_hdr_resp
 * @brief Response structure (includes header + response type)
 */
struct virtio_gpu_ctrl_hdr_resp {
    struct virtio_gpu_ctrl_hdr hdr;   ///< Original command header (with matching fence_id)
    uint32_t type;                   ///< Response type (one of virtio_gpu_ctrl_type)
    uint32_t padding;                ///< Padding
};

/**
 * @struct virtio_gpu_rect
 * @brief Rectangle definition (x, y, width, height)
 */
struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

/**
 * @struct virtio_gpu_display_scanout
 * @brief Information about a single scanout (display output)
 */
struct virtio_gpu_display_scanout {
    uint32_t scanout_id;             ///< Scanout identifier
    uint32_t resv;                   ///< Reserved
    uint64_t enabled;                ///< Boolean: is this scanout enabled?
    uint64_t flags;                  ///< Scanout flags (reserved)
    struct virtio_gpu_rect r;        ///< Target rectangle position in display
    uint32_t res_handle;             ///< Resource bound to this scanout (0 if none)
    uint32_t padding[3];             ///< Padding
};

/**
 * @struct virtio_gpu_display_info
 * @brief Response to VIRTIO_GPU_CMD_GET_DISPLAY_INFO
 *
 * This structure is sent as the command payload. The actual scanout array
 * follows immediately after this structure in memory.
 */
struct virtio_gpu_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanouts;               ///< Number of scanouts
    struct virtio_gpu_display_scanout scanouts[];  ///< Variable-length array
};

/*---------------------------------------------------------------------------
 *  Resource Management Commands
 *---------------------------------------------------------------------------*/

/**
 * @struct virtio_gpu_resource_create_2d
 * @brief Create a 2D resource (framebuffer)
 */
struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;           ///< Resource ID (assigned by driver, device echoes back)
    uint32_t format;                ///< Pixel format (virtio_gpu_formats)
    uint32_t width;                 ///< Width in pixels
    uint32_t height;                ///< Height in pixels
    uint32_t resv;                  ///< Reserved
};

/**
 * @struct virtio_gpu_resource_destroy
 * @brief Destroy a resource
 */
struct virtio_gpu_resource_destroy {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;           ///< Resource ID to destroy
    uint32_t resv;                  ///< Reserved
};

/**
 * @struct virtio_gpu_mem_entry
 * @brief Memory entry for resource backing (scatter-gather list)
 *
 * This describes one contiguous physical memory region that backs a resource.
 * Multiple entries can be chained for non-contiguous allocations.
 */
struct virtio_gpu_mem_entry {
    uint64_t addr;                  ///< Physical address of memory region
    uint32_t length;                ///< Length of region in bytes
    uint32_t padding;               ///< Padding to 8-byte boundary
};

/**
 * @struct virtio_gpu_resource_attach_backing
 * @brief Attach backing memory to a resource
 */
struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;           ///< Resource ID
    uint32_t nr_entries;            ///< Number of mem_entry structures
    struct virtio_gpu_mem_entry entries[];  ///< Array of mem_entry (variable length)
};

/**
 * @struct virtio_gpu_resource_detach_backing
 * @brief Detach backing memory from a resource
 */
struct virtio_gpu_resource_detach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;           ///< Resource ID
    uint32_t resv;                  ///< Reserved
};

/**
 * @struct virtio_gpu_resource_assign_id
 * @brief Legacy: assign a resource ID (for older implementations)
 */
struct virtio_gpu_resource_assign_id {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;           ///< Resource ID to assign
    uint32_t res_handle;            ///< Device handle (usually same as ID)
    uint32_t resv;                  ///< Reserved
};

/*---------------------------------------------------------------------------
 *  Blob Resource Commands (for large or special resources)
 *---------------------------------------------------------------------------*/

/**
 * @struct virtio_gpu_resource_create_blob
 * @brief Create a blob resource (large, potentially GPU-local)
 */
struct virtio_gpu_resource_create_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint64_t resource_id;           ///< Resource ID (64-bit for blobs)
    uint32_t format;                ///< Pixel format or VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM
    uint32_t resource_handle;       ///< Device handle (output)
    uint64_t size;                  ///< Size in bytes
    uint32_t resv;                  ///< Reserved
    uint32_t blob_mem;              ///< Memory type (VIRTIO_GPU_BLOB_MEM_*)
    uint64_t blob_flags;            ///< Blob-specific flags
    uint32_t resv2[4];              ///< Reserved
};

/*---------------------------------------------------------------------------
 *  Transfer Commands
 *---------------------------------------------------------------------------*/

/**
 * @struct virtio_gpu_transfer_to_host_2d
 * @brief Copy 2D data from guest memory to device resource
 *
 * This command copies a rectangular region from the guest's backing memory
 * into the device resource. The backing memory must already be attached.
 */
struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;           ///< Resource ID to copy into
    uint32_t resv;                  ///< Reserved
    struct virtio_gpu_rect r;       ///< Region to transfer (in resource coordinates)
    uint64_t offset;                ///< Offset into resource (usually 0)
};

/**
 * @struct virtio_gpu_transfer_from_host_2d
 * @brief Copy 2D data from device resource to guest memory
 *
 * Used for readback (e.g., screenshots). Less common.
 */
struct virtio_gpu_transfer_from_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t resv;
    struct virtio_gpu_rect r;
    uint64_t offset;
};

/*---------------------------------------------------------------------------
 *  Scanout Commands
 *---------------------------------------------------------------------------*/

/**
 * @struct virtio_gpu_set_scanout
 * @brief Assign a resource to a scanout
 *
 * This binds a resource to a scanout. The resource determines what is
 * displayed on that scanout. Multiple scanouts can reference the same
 * resource (cloned displays).
 */
struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout_id;            ///< Scanout ID to configure
    uint32_t resv;                  ///< Reserved
    struct virtio_gpu_rect r;       ///< Rectangle within resource to display
    uint32_t res_handle;            ///< Resource handle to display
};

/**
 * @struct virtio_gpu_update_scanout
 * @brief Update scanout parameters (position, size)
 *
 * This command updates the scanout's position within the virtual display
 * and optionally its size. It does NOT change which resource is displayed;
 * use SET_SCANOUT to change the resource.
 */
struct virtio_gpu_update_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout_id;            ///< Scanout ID to update
    uint32_t resv;                  ///< Reserved
    uint32_t x;                     ///< X position in virtual display
    uint32_t y;                     ///< Y position in virtual display
    uint32_t width;                 ///< Width (can be 0 to keep current)
    uint32_t height;                ///< Height (0 = keep current)
};

/*---------------------------------------------------------------------------
 *  Cursor Commands
 *---------------------------------------------------------------------------*/

/**
 * @struct virtio_gpu_update_cursor
 * @brief Update cursor position and visibility
 *
 * The cursor is a separate resource (typically a small RGBA image).
 */
struct virtio_gpu_update_cursor {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout_id;            ///< Scanout to update cursor on
    uint32_t res_handle;            ///< Cursor resource ID (0 to hide)
    uint32_t x;                     ///< X position (hot spot)
    uint32_t y;                     ///< Y position (hot spot)
    uint32_t visible;               ///< Boolean: show cursor?
};

/**
 * @struct virtio_gpu_update_cursor_blob
 * @brief Update cursor with a cursor image (blob resource)
 */
struct virtio_gpu_update_cursor_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t scanout_id;
    uint32_t res_handle;
    uint32_t x;
    uint32_t y;
    uint32_t hot_x;                 ///< Hot spot X within cursor image
    uint32_t hot_y;                 ///< Hot spot Y
    uint32_t resv;                  ///< Reserved
};

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* VIRTIO_GPU_H */
