/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AVNC_FRAMEBUFFER_TEXTURE_H
#define AVNC_FRAMEBUFFER_TEXTURE_H

#include <GLES2/gl2.h>
#include <stddef.h>
#include <stdint.h>

// Full-width rows are contiguous even on OpenGL ES 2, which has no unpack row length.
struct FramebufferRows {
    int first;
    int end;

    void include(int top, int bottom, int frameHeight) {
        if (top < 0) top = 0;
        if (bottom > frameHeight) bottom = frameHeight;
        if (top >= bottom) return;

        if (end == 0 || top < first) first = top;
        if (bottom > end) end = bottom;
    }
};

// The decoder publishes completed updates under the framebuffer mutex. Each GL
// texture keeps its own uploaded revision, including across overlapping renderers.
struct FramebufferTexture {
    int width;
    int height;
    int64_t revision;
    int64_t sizeRevision;
    int64_t rowsSinceRevision;
    FramebufferRows changedRows;

    void resize(int w, int h) {
        width = w;
        height = h;
        sizeRevision = ++revision;
        rowsSinceRevision = revision;
        changedRows = {};
    }

    void publish(FramebufferRows rows) {
        if (rows.end > 0) {
            changedRows.include(rows.first, rows.end, height);
            ++revision;
        }
    }

    int64_t upload(const uint8_t *pixels, int64_t uploadedRevision) {
        if (!pixels || width <= 0 || height <= 0) return 0;
        if (uploadedRevision == revision) return revision;

        // Pixels are BGRA. The fragment shader swaps the components after sampling.
        if (uploadedRevision < sizeRevision || uploadedRevision > revision) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, pixels);
        } else {
            // A slower second renderer can predate the retained damage history.
            const auto rows = uploadedRevision < rowsSinceRevision ? FramebufferRows{0, height} : changedRows;
            const size_t offset = (size_t) rows.first * width * 4;
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, rows.first, width, rows.end - rows.first,
                            GL_RGBA, GL_UNSIGNED_BYTE, pixels + offset);
        }
        if (glGetError() != GL_NO_ERROR) return 0;
        rowsSinceRevision = revision;
        changedRows = {};
        return revision;
    }
};

#endif // AVNC_FRAMEBUFFER_TEXTURE_H
