/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Standalone host regression test. Only the GLES2 headers are needed.
#include "../../main/cpp/FramebufferTexture.h"
#include "../../main/cpp/FramebufferLimits.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

static std::vector<uint32_t> texture;
static size_t transferred;
static int allocations;
static int updates;
static GLenum nextError = GL_NO_ERROR;

void GL_APIENTRY glTexImage2D(GLenum, GLint, GLint, GLsizei width, GLsizei height, GLint,
                              GLenum, GLenum, const void *pixels) {
    const auto *src = static_cast<const uint32_t *>(pixels);
    texture.assign(src, src + width * height);
    transferred += texture.size() * sizeof(uint32_t);
    ++allocations;
}

void GL_APIENTRY glTexSubImage2D(GLenum, GLint, GLint x, GLint y, GLsizei width, GLsizei height,
                                 GLenum, GLenum, const void *pixels) {
    assert(x == 0 && y >= 0 && width > 0 && height > 0);
    assert(static_cast<size_t>(y + height) * width <= texture.size());
    std::copy_n(static_cast<const uint32_t *>(pixels), width * height, texture.begin() + y * width);
    transferred += static_cast<size_t>(width) * height * sizeof(uint32_t);
    ++updates;
}

GLenum GL_APIENTRY glGetError() {
    const GLenum error = nextError;
    nextError = GL_NO_ERROR;
    return error;
}

int main() {
    size_t byteCount = 0;
    assert(framebufferByteCount(7680, 4320, 4, &byteCount) && byteCount == 132710400u);
    assert(framebufferByteCount(8192, 4096, 4, &byteCount) && byteCount == kMaxFramebufferBytes);
    assert(!framebufferByteCount(8192, 4097, 4, &byteCount));
    assert(!framebufferByteCount(0, 1, 4, &byteCount));
    assert(!framebufferByteCount(-1, 1, 4, &byteCount));
    constexpr int width = 1920, height = 1080;
    std::vector<uint32_t> pixels(width * height, 0x11223344);
    const auto *data = reinterpret_cast<const uint8_t *>(pixels.data());
    FramebufferTexture frame{};
    assert(frame.upload(nullptr, 0) == 0);

    frame.resize(width, height);
    auto revision = frame.upload(data, 0);
    const auto firstRevision = revision;
    const auto fullBytes = transferred;
    assert(revision != 0 && allocations == 1 && texture == pixels);

    for (int i = 0; i < 60; ++i)
        assert(frame.upload(data, revision) == revision);
    assert(allocations == 1 && updates == 0 && transferred == fullBytes);

    // Separate server updates may both arrive before the next draw.
    pixels[100 * width + 12] = 0x55667788;
    frame.publish({100, 101});
    pixels[115 * width + 47] = 0x99aabbcc;
    frame.publish({115, 116});
    revision = frame.upload(data, revision);
    const auto bandBytes = transferred - fullBytes;
    assert(bandBytes == 16 * width * sizeof(uint32_t));
    assert(allocations == 1 && updates == 1 && texture == pixels);

    // A second texture that missed those edits must recover all rows.
    std::fill(texture.begin(), texture.end(), 0);
    assert(frame.upload(data, firstRevision) == revision);
    assert(transferred == 2 * fullBytes + bandBytes && texture == pixels);

    // A recreated GL context starts from zero, even for an unchanged session.
    texture.clear();
    assert(frame.upload(data, 0) == revision && allocations == 2 && texture == pixels);

    FramebufferRows clipped{};
    clipped.include(height, height + 8, height);
    frame.publish(clipped);
    assert(frame.upload(data, revision) == revision);
    clipped.include(height - 1, height + 8, height);
    clipped.include(-3, 1, height);
    assert(clipped.first == 0 && clipped.end == height);

    frame.resize(3, 4);
    nextError = GL_OUT_OF_MEMORY;
    assert(frame.upload(data, revision) == 0);
    assert(frame.upload(data, 0) != 0 && texture.size() == 12);
    assert(allocations == 4);

    std::printf("1920x1080 RGBA: full %zu bytes, 60 unchanged redraws 0 bytes, 16 rows %zu bytes\n",
                fullBytes, bandBytes);
}
