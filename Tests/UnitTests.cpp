/**
 * @file
 * Unit Tests for VirtIO GPU Driver
 *
 * These tests verify individual components in isolation. They can be
 * compiled as a user-space program to test the virtqueue and protocol
 * logic without requiring kernel execution.
 *
 * Build:
 *   clang++ -std=c++17 -I../Include UnitTests.cpp -o virtio-gpu-tests
 *
 * Run:
 *   ./virtio-gpu-tests
 */

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "virtio_types.h"
#include "virtqueue.h"

/*===========================================================================
 *  Test Framework
 *===========================================================================*/

static int test_count = 0;
static int pass_count = 0;

#define TEST(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", #expr, __LINE__); \
        return false; \
    } \
} while(0)

#define RUN_TEST(fn) do { \
    fprintf(stdout, "Test: %s\n", #fn); \
    if (fn()) { \
        fprintf(stdout, "  PASS\n"); \
        pass_count++; \
    } else { \
        fprintf(stdout, "  FAIL\n"); \
    } \
    test_count++; \
} while(0)

/*===========================================================================
 *  Tests
 *===========================================================================*/

static bool test_types()
{
    ASSERT_EQ(sizeof(struct VirtIORegion), 156);
    ASSERT_EQ(sizeof(struct vring_desc), 16);
    ASSERT_EQ(sizeof(vring_used_elem), 8);
    return true;
}

static bool test_virtqueue_init()
{
    size_t size = VIRTQUEUE_SIZE(4);
    uint8_t* mem = (uint8_t*)malloc(size);
    memset(mem, 0, size);
    
    vring_desc* desc = (vring_desc*)mem;
    vring_avail* avail = (vring_avail*)(mem + 4 * sizeof(vring_desc));
    vring_used* used = (vring_used*)((uint8_t*)avail + sizeof(vring_avail) + 4 * sizeof(uint16_t));
    
    virt_queue vq;
    bool ok = init_virtqueue(&vq, 0, desc, avail, used, 4);
    TEST(ok);
    TEST(vq.num == 4);
    TEST(vq.desc == desc);
    TEST(vq.avail == avail);
    TEST(vq.used == used);
    TEST(vq.last_avail_idx == 0);
    TEST(vq.last_used_idx == 0);
    TEST(vq.free_head == 0);
    TEST(desc[0].next == 1);
    TEST(desc[3].next == VIRTQUEUE_NO_FREE_DESC);
    TEST(avail->idx == 0);
    
    free(mem);
    return true;
}

static bool test_enqueue_single()
{
    size_t size = VIRTQUEUE_SIZE(4);
    uint8_t* mem = (uint8_t*)malloc(size);
    memset(mem, 0, size);
    
    vring_desc* desc = (vring_desc*)mem;
    vring_avail* avail = (vring_avail*)(mem + 4 * sizeof(vring_desc));
    vring_used* used = (vring_used*)((uint8_t*)avail + sizeof(vring_avail) + 4 * sizeof(uint16_t));
    
    virt_queue vq;
    init_virtqueue(&vq, 0, desc, avail, used, 4);
    
    bool ok = virtqueue_enqueue_single(&vq, 0x1000, 256, false);
    TEST(ok);
    TEST(vq.avail->idx == 1);
    TEST(vq.free_head == 1);
    TEST(desc[0].addr == 0x1000);
    TEST(desc[0].len == 256);
    TEST((desc[0].flags & VRING_DESC_F_WRITE) == 0);
    
    free(mem);
    return true;
}

static bool test_get_buf()
{
    size_t size = VIRTQUEUE_SIZE(4);
    uint8_t* mem = (uint8_t*)malloc(size);
    memset(mem, 0, size);
    
    vring_desc* desc = (vring_desc*)mem;
    vring_avail* avail = (vring_avail*)(mem + 4 * sizeof(vring_desc));
    vring_used* used = (vring_used*)((uint8_t*)avail + sizeof(vring_avail) + 4 * sizeof(uint16_t));
    
    virt_queue vq;
    init_virtqueue(&vq, 0, desc, avail, used, 4);
    virtqueue_enqueue_single(&vq, 0x1000, 256, false);
    
    // Device completes
    used->ring[0].id = 0;
    used->ring[0].len = 256;
    used->idx = 1;
    OSMemoryBarrier();
    
    uint32_t out_len = 0;
    uint16_t idx = virtqueue_get_buf(&vq, &out_len);
    TEST(idx == 0);
    TEST(out_len == 256);
    TEST(vq.last_used_idx == 1);
    
    free(mem);
    return true;
}

static bool test_queue_full()
{
    size_t size = VIRTQUEUE_SIZE(2);
    uint8_t* mem = (uint8_t*)malloc(size);
    memset(mem, 0, size);
    
    vring_desc* desc = (vring_desc*)mem;
    vring_avail* avail = (vring_avail*)(mem + 2 * sizeof(vring_desc));
    vring_used* used = (vring_used*)((uint8_t*)avail + sizeof(vring_avail) + 2 * sizeof(uint16_t));
    
    virt_queue vq;
    init_virtqueue(&vq, 0, desc, avail, used, 2);
    
    TEST(virtqueue_enqueue_single(&vq, 0x1000, 256, false));
    TEST(virtqueue_enqueue_single(&vq, 0x2000, 256, false));
    TEST_FALSE(virtqueue_enqueue_single(&vq, 0x3000, 256, false));
    
    free(mem);
    return true;
}

static bool test_available()
{
    size_t size = VIRTQUEUE_SIZE(4);
    uint8_t* mem = (uint8_t*)malloc(size);
    memset(mem, 0, size);
    
    vring_desc* desc = (vring_desc*)mem;
    vring_avail* avail = (vring_avail*)(mem + 4 * sizeof(vring_desc));
    vring_used* used = (vring_used*)((uint8_t*)avail + sizeof(vring_avail) + 4 * sizeof(uint16_t));
    
    virt_queue vq;
    init_virtqueue(&vq, 0, desc, avail, used, 4);
    
    TEST_FALSE(virtqueue_available(&vq));
    virtqueue_enqueue_single(&vq, 0x1000, 256, false);
    TEST_FALSE(virtqueue_available(&vq));  // Not yet completed
    
    used->ring[0].id = 0;
    used->ring[0].len = 256;
    used->idx = 1;
    OSMemoryBarrier();
    TEST_TRUE(virtqueue_available(&vq));
    
    free(mem);
    return true;
}

static bool test_reset()
{
    size_t size = VIRTQUEUE_SIZE(4);
    uint8_t* mem = (uint8_t*)malloc(size);
    memset(mem, 0, size);
    
    vring_desc* desc = (vring_desc*)mem;
    vring_avail* avail = (vring_avail*)(mem + 4 * sizeof(vring_desc));
    vring_used* used = (vring_used*)((uint8_t*)avail + sizeof(vring_avail) + 4 * sizeof(uint16_t));
    
    virt_queue vq;
    init_virtqueue(&vq, 0, desc, avail, used, 4);
    virtqueue_enqueue_single(&vq, 0x1000, 256, false);
    used->ring[0].id = 0;
    used->ring[0].len = 256;
    used->idx = 1;
    OSMemoryBarrier();
    virtqueue_get_buf(&vq, nullptr);
    
    virtqueue_reset(&vq);
    
    TEST(vq.last_avail_idx == 0);
    TEST(vq.last_used_idx == 0);
    TEST(vq.free_head == 0);
    TEST(avail->idx == 0);
    TEST(used->idx == 0);
    TEST(desc[0].next == 1);
    
    free(mem);
    return true;
}

/*===========================================================================
 *  Main
 *===========================================================================*/

int main()
{
    fprintf(stdout, "VirtIO GPU Unit Tests\n");
    fprintf(stdout, "====================\n\n");
    
    RUN_TEST(test_types);
    RUN_TEST(test_virtqueue_init);
    RUN_TEST(test_enqueue_single);
    RUN_TEST(test_get_buf);
    RUN_TEST(test_queue_full);
    RUN_TEST(test_available);
    RUN_TEST(test_reset);
    
    fprintf(stdout, "\n");
    fprintf(stdout, "Results: %d/%d passed\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}
