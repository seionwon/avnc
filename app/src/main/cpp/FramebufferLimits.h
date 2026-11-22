#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t kMaxFramebufferBytes = 128u * 1024u * 1024u;

inline bool framebufferByteCount(int width, int height, int bytesPerPixel, size_t *size) {
    if (width <= 0 || height <= 0 || bytesPerPixel <= 0 || bytesPerPixel > 4)
        return false;
    const uint64_t bytes = static_cast<uint64_t>(width) * height * bytesPerPixel;
    if (bytes > kMaxFramebufferBytes)
        return false;
    *size = static_cast<size_t>(bytes);
    return true;
}
