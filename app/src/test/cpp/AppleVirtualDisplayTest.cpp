/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host test: a viewer that joins a Mac must never replace virtual displays another viewer is using.
 * Build: clang++ -std=c++17 -I app/src/main/cpp -I $(brew --prefix openssl@3)/include \
 *        app/src/test/cpp/AppleVirtualDisplayTest.cpp app/src/main/cpp/AppleRfb.cpp \
 *        -L $(brew --prefix openssl@3)/lib -lcrypto -lz -o /tmp/avd-test
 */

#include "AppleRfb.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static uint8_t *gFb = nullptr;
static uint8_t *resizeFb(void *, int w, int h) {
    free(gFb);
    gFb = (uint8_t *) calloc((size_t) w * h, 4);
    return gFb;
}
static void noop(void *) {}

static void be16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void be32(uint8_t *p, unsigned v) { be16(p, v >> 16); be16(p + 2, v & 0xffff); }

struct Server {
    int fd;
    void send(const void *data, size_t len) {
        if (write(fd, data, len) != (ssize_t) len) abort();
    }
    // Number of SetDisplayConfiguration messages the viewer has sent so far.
    int drainVirtualRequests() {
        static const char marker[] = "AVNC Virtual Display";
        uint8_t buf[8192];
        int found = 0;
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i + (ssize_t) sizeof(marker) - 1 <= n; ++i)
                if (memcmp(buf + i, marker, sizeof(marker) - 1) == 0) ++found;
        }
        return found;
    }
    void handshake() {
        send("RFB 003.889\n", 12);
        const uint8_t sec[2] = {1, 1}; // one type offered: None
        send(sec, 2);
        const uint8_t ok[4] = {0, 0, 0, 0};
        send(ok, 4);

        uint8_t init[24 + 22 + 3] = {0};
        be16(init, 1920);
        be16(init + 2, 1080);
        be32(init + 20, 22 + 3);
        uint8_t *ext = init + 24;
        uint8_t *commands = ext + 6;
        commands[0x0d / 8] |= 0x80u >> (0x0d % 8); // SetDisplay
        commands[0x1d / 8] |= 0x80u >> (0x1d % 8); // SetDisplayConfiguration
        memcpy(ext + 22, "Mac", 3);
        send(init, sizeof(init));
    }
    // A DisplayLayout with `physical` ordinary displays followed by `virtualCount` virtual ones.
    void sendLayout(int physical, int virtualCount, int virtualWidth, int virtualHeight) {
        int count = physical + virtualCount;
        size_t payloadLen = 0x14 + (size_t) count * 0x38;
        uint8_t msg[4 + 12 + 2 + 0x14 + 8 * 0x38] = {0};
        be16(msg + 2, 1);                       // one rectangle
        be32(msg + 4 + 8, 1105);                // DisplayLayout
        be16(msg + 16, (unsigned) payloadLen);
        uint8_t *p = msg + 18;
        be16(p + 0x06, 1920);
        be16(p + 0x08, 1080);
        be32(p + 0x0a, 0xffffffffu);
        be16(p + 0x12, (unsigned) count);
        for (int i = 0; i < count; ++i) {
            uint8_t *r = p + 0x14 + i * 0x38;
            bool isVirtual = i >= physical;
            int w = isVirtual ? virtualWidth : 1920, h = isVirtual ? virtualHeight : 1080;
            be32(r + 0x10, 100 + i);
            be16(r + 0x18, h);
            be16(r + 0x1a, w);
            be16(r + 0x20, h);
            be16(r + 0x22, w);
            be32(r + 0x24, isVirtual ? APPLE_DISPLAY_FLAG_VIRTUAL : (i == 0 ? APPLE_DISPLAY_FLAG_MAIN : 0));
        }
        send(msg, 18 + payloadLen);
    }
};

struct Session {
    Server server{};
    AppleRfbClient *client = nullptr;

    explicit Session(AppleDisplayMode mode) {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) abort();
        server.fd = fds[1];
        fcntl(server.fd, F_SETFL, fcntl(server.fd, F_GETFL) | O_NONBLOCK);
        server.handshake();

        AppleRfbConfig cfg{};
        cfg.displayMode = mode;
        cfg.initialDisplayIndex = 1;
        cfg.virtualWidth = 1920;
        cfg.virtualHeight = 1080;
        cfg.virtualDensity = 1.0;
        AppleRfbCallbacks cbs{};
        cbs.resizeFramebuffer = resizeFb;
        cbs.lockFramebuffer = noop;
        cbs.unlockFramebuffer = noop;
        int err = 0;
        if (appleRfbConnect(fds[0], &cfg, &cbs, nullptr, &client, &err) != APPLE_RFB_OK) {
            fprintf(stderr, "connect failed: %d\n", err);
            exit(1);
        }
    }
    void layout(int physical, int virtualCount, int w = 0, int h = 0) {
        server.sendLayout(physical, virtualCount, w, h);
        if (!appleRfbHandleServerMessage(client)) {
            fprintf(stderr, "layout rejected\n");
            exit(1);
        }
    }
    int virtualRequests() { return server.drainVirtualRequests(); }
};

static int failures = 0;
#define EXPECT(cond) do { if (!(cond)) { ++failures; fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #cond); } } while (0)

int main() {
    {   // Nothing is requested before the Mac reports what is already there.
        Session s(APPLE_DISPLAY_VIRTUAL_1);
        EXPECT(s.virtualRequests() == 0);
        EXPECT(!appleRfbSetDesktopSize(s.client, 1280, 800));
        EXPECT(s.virtualRequests() == 0);
    }
    {   // Another viewer already uses two virtual displays: join, never reconfigure.
        Session s(APPLE_DISPLAY_VIRTUAL_1);
        s.layout(0, 2, 2560, 1440);
        EXPECT(s.virtualRequests() == 0);
        EXPECT(!appleRfbSetDesktopSize(s.client, 1280, 800));
        s.layout(0, 2, 2560, 1440);
        EXPECT(s.virtualRequests() == 0);
    }
    {   // Same when the other viewer uses a single virtual display next to physical ones.
        Session s(APPLE_DISPLAY_VIRTUAL_1);
        s.layout(2, 1, 1920, 1080);
        EXPECT(s.virtualRequests() == 0);
        EXPECT(!appleRfbSetDesktopSize(s.client, 1280, 800));
        EXPECT(s.virtualRequests() == 0);
    }
    {   // No virtual display in use: create ours, and resizing it keeps working.
        Session s(APPLE_DISPLAY_VIRTUAL_1);
        s.layout(2, 0);
        EXPECT(s.virtualRequests() == 1);
        s.layout(0, 1, 1920, 1080);
        EXPECT(s.virtualRequests() == 0);
        EXPECT(appleRfbSetDesktopSize(s.client, 1280, 800));
        EXPECT(s.virtualRequests() == 1);
        s.layout(0, 1, 1280, 800);

        // A second viewer then replaces the configuration with its own two displays.
        s.layout(0, 2, 2560, 1440);
        EXPECT(!appleRfbSetDesktopSize(s.client, 1000, 700));
        EXPECT(s.virtualRequests() == 0);
    }
    {   // Standard mode never asks for a virtual display.
        Session s(APPLE_DISPLAY_STANDARD);
        s.layout(2, 0);
        EXPECT(s.virtualRequests() == 0);
        EXPECT(!appleRfbSetDesktopSize(s.client, 1280, 800));
    }

    if (failures) return 1;
    puts("AppleVirtualDisplayTest: all passed");
    return 0;
}
