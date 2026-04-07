/**
 * @file
 * Test Pattern Generator for VirtIO GPU Framebuffer
 *
 * This program can be run on macOS (inside the guest) to verify that
 * the framebuffer is working correctly. It writes different test patterns
 * to the screen to validate:
 * - Framebuffer memory is writable
 * - Updates are visible
 * - Color depth is correct
 *
 * Usage:
 *   sudo virtio_gpu_test_pattern
 *
 * This requires the driver to be loaded and the framebuffer accessible.
 *
 * In a real scenario, you'd compile this as a macOS command-line tool
 * and run it inside the guest. Or, simpler: use existing tools like
 * `screencapture` or open Preview and draw.
 *
 * For now, this is a reference implementation.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// In a real implementation, you'd mmap the framebuffer via IOKit
// For this example, we'll just simulate

// Assume 1920x1080, 32bpp BGRA
#define WIDTH 1920
#define HEIGHT 1080
#define BPP 32
#define STRIDE (WIDTH * 4)
#define FB_SIZE (STRIDE * HEIGHT)

// Simulated framebuffer
static uint32_t* framebuffer = NULL;

void allocate_fake_fb() {
    framebuffer = (uint32_t*)malloc(FB_SIZE);
    if (!framebuffer) {
        perror("malloc");
        exit(1);
    }
}

void free_fake_fb() {
    free(framebuffer);
}

void clear_screen(uint32_t color) {
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            framebuffer[y * (STRIDE/4) + x] = color;
        }
    }
}

void draw_color_bars() {
    // Draw 8 vertical color bars (like SMPTE test pattern)
    int bar_width = WIDTH / 8;
    uint32_t colors[8] = {
        0xFFFFFFFF,  // White
        0xFFFFFF00,  // Yellow (BGR: 0, 255, 255)
        0xFF00FFFF,  // Cyan
        0xFF00FF00,  // Green
        0xFFFF00FF,  // Magenta
        0xFF0000FF,  // Red
        0xFF0000FF,  // Blue (actually this and next are similar, adjust)
        0xFF0000FF   // Blue
    };
    
    for (int bar = 0; bar < 8; bar++) {
        int start_x = bar * bar_width;
        for (int x = start_x; x < start_x + bar_width; x++) {
            for (int y = 0; y < HEIGHT; y++) {
                framebuffer[y * (STRIDE/4) + x] = colors[bar];
            }
        }
    }
}

void draw_gradient() {
    for (int y = 0; y < HEIGHT; y++) {
        uint8_t intensity = (uint8_t)((double)y / HEIGHT * 256);
        uint32_t color = (intensity << 16) | (intensity << 8) | intensity; // BGRA: R=G=B=intensity, A=0 (implicit)
        for (int x = 0; x < WIDTH; x++) {
            framebuffer[y * (STRIDE/4) + x] = color;
        }
    }
}

void draw_checkerboard(int size) {
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int checker = ((x / size) + (y / size)) % 2;
            uint32_t color = checker ? 0xFFFFFFFF : 0xFF000000; // White or Black
            framebuffer[y * (STRIDE/4) + x] = color;
        }
    }
}

int main() {
    printf("VirtIO GPU Test Pattern Generator\n");
    printf("This program writes patterns to the framebuffer.\n");
    printf("NOTE: This is a simulation; the real framebuffer would be mapped via IOKit.\n");
    printf("\n");
    
    allocate_fake_fb();
    
    printf("1. Testing solid color fill (Red)\n");
    clear_screen(0x00FF0000);  // BGRA: Blue=0, Green=0, Red=255, Alpha=0
    sleep(2);
    
    printf("2. Testing color bars\n");
    draw_color_bars();
    sleep(2);
    
    printf("3. Testing gradient\n");
    draw_gradient();
    sleep(2);
    
    printf("4. Testing checkerboard\n");
    draw_checkerboard(64);
    sleep(2);
    
    printf("All patterns written. In a real test, you would verify they appear on screen.\n");
    
    free_fake_fb();
    return 0;
}

#endif
