/*
 * Copyright (c) 2026  AVNC Contributors.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

#include "AppleRfb.h"
#include "FramebufferLimits.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <zlib.h>

/******************************************************************************
 * Protocol Constants
 *****************************************************************************/

static const uint8_t kSecNone      = 1;
static const uint8_t kSecVncAuth   = 2;
static const uint8_t kSecAppleDH   = 30;
static const uint8_t kSecAppleRSA  = 33;
static const uint8_t kSecAppleSRP  = 36;

static const uint32_t kServerFlagObserveOnly   = 0x01u;
static const uint32_t kServerFlagSessionSelect = 0x04u;

static const int32_t kEncRaw                 = 0;
static const int32_t kEncCopyRect            = 1;
static const int32_t kEncZlib                = 6;
static const int32_t kEncZRLE                = 16;
static const int32_t kEncAppleThousands      = 1002;
static const int32_t kEncMediaStream         = 1010;
static const int32_t kEncCursorPosition      = 1100;
static const int32_t kEncDisplayInfo         = 1101;
static const int32_t kEncUserInfo            = 1102;
static const int32_t kEncRekey               = 1103;
static const int32_t kEncCursorImage         = 1104;
static const int32_t kEncDisplayLayout       = 1105;
static const int32_t kEncVendorKeysyms       = 1107;
static const int32_t kEncKeyboardSource      = 1109;
static const int32_t kEncDeviceInfo          = 1110;
static const int32_t kEncPointerPos          = -232;
static const int32_t kEncLastRect            = -224;
static const int32_t kEncDesktopSize         = -223;
static const int32_t kEncDesktopName         = -307;
static const int32_t kEncExtendedDesktopSize = -308;

static const size_t kRecordChecksumSize = 20; // SHA-1
static const size_t kRecordMaxSize      = 65520;
static const size_t kRecordMaxBody      = kRecordMaxSize - 2 - kRecordChecksumSize;
static const size_t kRekeyBodySize      = 36;
static const size_t kMaxCompressedRect  = 64u * 1024u * 1024u;
static const size_t kMaxCursorSize      = 512;
// Decoder output above this is released after use, so one oversized rect cannot pin memory.
static const size_t kMaxRetainedDecoderBytes = 16u * 1024u * 1024u;
static const uint64_t kMaxSrpIterations = 1000000ULL;
static const size_t kMaxCursorCache     = 64;

static const int kVirtualDisplayLargestSide = 16384;
static const uint32_t kDefaultUpdateIntervalUs = 16667u; // ~60 fps

static const uint16_t kMacKeyboardTypeAnsi    = 40;
static const uint8_t  kMacKeyFlagKeyCodeValid = 0x01u;
static const uint8_t  kMacKeyFlagPreferCode   = 0x02u;
static const uint16_t kMacKeyCodeV            = 9;
static const uint16_t kMacKeyCodeSpace        = 49;
static const uint16_t kMacKeyCodeCommand      = 55;
static const uint16_t kMacKeyCodeControl      = 59;
static const uint32_t kKeySymVoid             = 0x00ffffffu;
static const size_t   kMaxSavedClipboardBytes = 256u * 1024u;
static const uint64_t kPasteboardEchoSupprMs  = 3000ULL;

static uint64_t nowMonotonicMs() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

/******************************************************************************
 * Endianness & Byte Buffer Helpers
 *****************************************************************************/

static inline uint16_t be16(const uint8_t *p) {
    return (uint16_t) (((uint16_t) p[0] << 8) | (uint16_t) p[1]);
}

static inline int16_t be16s(const uint8_t *p) {
    return (int16_t) be16(p);
}

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t) p[0] << 24) |
           ((uint32_t) p[1] << 16) |
           ((uint32_t) p[2] << 8) |
           (uint32_t) p[3];
}

static inline uint64_t be64(const uint8_t *p) {
    return ((uint64_t) be32(p) << 32) | (uint64_t) be32(p + 4);
}

static inline uint16_t le16(const uint8_t *p) {
    return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

static inline void putBe16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) (v & 0xffu);
}

static inline void putLe16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t) (v & 0xffu);
    p[1] = (uint8_t) (v >> 8);
}

static inline void putBe32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) ((v >> 16) & 0xffu);
    p[2] = (uint8_t) ((v >> 8) & 0xffu);
    p[3] = (uint8_t) (v & 0xffu);
}

static inline void putBe64(uint8_t *p, uint64_t v) {
    putBe32(p, (uint32_t) (v >> 32));
    putBe32(p + 4, (uint32_t) (v & 0xffffffffu));
}

static inline void putBeFloat32(uint8_t *p, float v) {
    uint32_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    putBe32(p, bits);
}

static inline void putBeFloat64(uint8_t *p, double v) {
    uint64_t bits = 0;
    memcpy(&bits, &v, sizeof(bits));
    putBe64(p, bits);
}

static inline double beFloat64(const uint8_t *p) {
    uint64_t bits = be64(p);
    double v = 0.0;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

static inline uint16_t clampU16(int v) {
    if (v < 0) return 0;
    if (v > 65535) return 65535;
    return (uint16_t) v;
}

static bool bnToFixedBytes(const BIGNUM *bn, uint8_t *out, size_t len) {
    int n = BN_num_bytes(bn);
    if (n < 0 || (size_t) n > len) return false;
    memset(out, 0, len - (size_t) n);
    BN_bn2bin(bn, out + (len - (size_t) n));
    return true;
}

/******************************************************************************
 * Cursor Cache & Zlib Stream Wrappers
 *****************************************************************************/

struct CachedCursor {
    uint32_t id;
    uint16_t width;
    uint16_t height;
    uint16_t xHot;
    uint16_t yHot;
    uint8_t *bgra;
};

struct ZlibDecoder {
    bool initialized;
    z_stream zs;
    uint8_t *outBuf;
    size_t outCap;
};

// Clears key material on every exit path, including early returns and gotos.
struct WipeOnExit {
    void *ptr;
    size_t len;
    ~WipeOnExit() { OPENSSL_cleanse(ptr, len); }
};

static void zlibDecoderTrim(ZlibDecoder *d) {
    if (d->outCap > kMaxRetainedDecoderBytes) {
        free(d->outBuf);
        d->outBuf = nullptr;
        d->outCap = 0;
    }
}

static void zlibDecoderFree(ZlibDecoder *d) {
    if (d->initialized) {
        inflateEnd(&d->zs);
        d->initialized = false;
    }
    free(d->outBuf);
    d->outBuf = nullptr;
    d->outCap = 0;
}

static bool zlibDecoderInflate(ZlibDecoder *d, const uint8_t *in, size_t inLen,
                               size_t expectedOrMax, bool exactExpected,
                               uint8_t **outPtr, size_t *outLen) {
    if (!d->initialized) {
        memset(&d->zs, 0, sizeof(d->zs));
        if (inflateInit(&d->zs) != Z_OK)
            return false;
        d->initialized = true;
    }

    size_t cap = exactExpected ? expectedOrMax : (inLen * 4 + 4096);
    if (cap < 4096) cap = 4096;
    if (cap > expectedOrMax && expectedOrMax > 0) cap = expectedOrMax;

    if (d->outCap < cap) {
        uint8_t *nb = (uint8_t *) realloc(d->outBuf, cap);
        if (!nb) return false;
        d->outBuf = nb;
        d->outCap = cap;
    }

    d->zs.next_in = (Bytef *) in;
    d->zs.avail_in = (uInt) inLen;
    size_t total = 0;

    while (true) {
        if (total == expectedOrMax || total == d->outCap) {
            if (total == expectedOrMax) {
                if (d->zs.avail_in == 0)
                    break;
                // A flush marker may still follow the final pixel byte.
                Bytef extra;
                d->zs.next_out = &extra;
                d->zs.avail_out = 1;
                uInt before = d->zs.avail_in;
                int rc = inflate(&d->zs, Z_SYNC_FLUSH);
                if (d->zs.avail_out == 0 || d->zs.avail_in == before ||
                    (rc != Z_OK && rc != Z_BUF_ERROR && rc != Z_STREAM_END)) return false;
                continue;
            }
            size_t newCap = d->outCap * 2;
            if (newCap > expectedOrMax) newCap = expectedOrMax;
            uint8_t *nb = (uint8_t *) realloc(d->outBuf, newCap);
            if (!nb) return false;
            d->outBuf = nb;
            d->outCap = newCap;
        }
        d->zs.next_out = d->outBuf + total;
        size_t limit = d->outCap < expectedOrMax ? d->outCap : expectedOrMax;
        d->zs.avail_out = (uInt) (limit - total);
        int rc = inflate(&d->zs, Z_SYNC_FLUSH);
        total = limit - d->zs.avail_out;
        if (rc == Z_STREAM_END) {
            break;
        }
        if (exactExpected && total == expectedOrMax && d->zs.avail_in == 0) {
            break;
        }
        if (rc == Z_BUF_ERROR && d->zs.avail_in == 0) {
            break;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            return false;
        }
        if (d->zs.avail_in == 0 && d->zs.avail_out > 0) {
            break;
        }
        if (rc == Z_BUF_ERROR && d->zs.avail_out > 0) {
            // No more progress possible
            break;
        }
    }

    if (exactExpected && total != expectedOrMax)
        return false;

    *outPtr = d->outBuf;
    *outLen = total;
    return true;
}

static bool inflateOneShot(const uint8_t *in, size_t inLen, size_t maxOut,
                           uint8_t **outBuf, size_t *outLen) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK)
        return false;

    size_t cap = maxOut > 0 && maxOut < 65536 ? maxOut : 4096;
    if (cap == 0) cap = 64;
    uint8_t *buf = (uint8_t *) malloc(cap);
    if (!buf) {
        inflateEnd(&zs);
        return false;
    }

    zs.next_in = (Bytef *) in;
    zs.avail_in = (uInt) inLen;
    size_t total = 0;
    bool ok = true;

    while (true) {
        if (total == cap) {
            if (cap >= maxOut) {
                ok = false;
                break;
            }
            size_t newCap = cap * 2;
            if (newCap > maxOut) newCap = maxOut;
            uint8_t *nb = (uint8_t *) realloc(buf, newCap);
            if (!nb) {
                ok = false;
                break;
            }
            buf = nb;
            cap = newCap;
        }
        zs.next_out = buf + total;
        zs.avail_out = (uInt) (cap - total);
        int rc = inflate(&zs, Z_SYNC_FLUSH);
        total = cap - zs.avail_out;
        if (rc == Z_STREAM_END) {
            break;
        }
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) {
            break;
        }
        if (rc != Z_OK) {
            ok = false;
            break;
        }
        if (zs.avail_in == 0 && zs.avail_out > 0) {
            break;
        }
    }

    inflateEnd(&zs);
    if (!ok) {
        free(buf);
        return false;
    }
    *outBuf = buf;
    *outLen = total;
    return true;
}

/******************************************************************************
 * Client Struct
 *****************************************************************************/

struct AppleRfbClient {
    int sock;
    AppleRfbConfig config;
    AppleRfbCallbacks cbs;
    void *userCtx;

    // Server capabilities from extended ServerInit
    bool extended;
    uint32_t serverFlags;
    uint8_t serverCommands[16];
    char *desktopName;

    // Framebuffer geometry & pointer
    uint8_t *fb;
    int fbWidth;
    int fbHeight;
    double pointerScale;
    bool paused;
    bool viewOnly;
    bool running;

    // Display layout & initial auto-selection state
    AppleDisplayLayout layout;
    bool hasLayout;
    bool initialDisplayApplied;
    int activeVirtualDisplays;
    int virtualLogicalWidth;
    int virtualLogicalHeight;

    // Crypto / Record layer
    uint8_t wrapKey[16];
    bool hasWrapKey;
    bool pendingRekey;
    uint8_t nextKey[16];
    uint8_t nextIv[16];

    bool recordReadActive;
    EVP_CIPHER_CTX *decCtx;
    uint32_t readSeq;
    uint8_t *readPlainBuf;
    size_t readPlainPos;
    size_t readPlainLen;

    pthread_mutex_t writeMutex;
    bool writeMutexInit;
    bool recordWriteActive;
    EVP_CIPHER_CTX *encCtx;
    uint32_t writeSeq;
    uint8_t *writeWireBuf;

    // Pixel decoders
    ZlibDecoder zlibDec;
    ZlibDecoder zrleDec;
    ZlibDecoder thousandsDec;
    uint8_t *scratchBuf;
    size_t scratchCap;

    // Cursor cache (encoding 1104)
    CachedCursor cursors[kMaxCursorCache];
    size_t cursorCount;
    size_t cursorEvictIdx;

    // Keyboard input source (encoding 1109) & pasteboard echo/restore state
    char keyboardSourceId[256];
    bool keyboardSecureInput;
    bool hasKeyboardSource;

    pthread_mutex_t clipMutex;
    bool clipMutexInit;
    uint8_t *savedClipboardArchive;
    size_t savedClipboardArchiveLen;
    uint64_t ownPasteboardEchoUntilMs;

    // Framebuffer update watchdog & pointer click-count tracking
    bool hasReceivedPixels;
    int hasInputSinceLastPixel; // atomic (Sender writes 1, Receiver reads/clears)
    bool inputFullRefreshPending;
    bool forceIncrementalPull;
    int inputStallRecoveries;
    uint64_t lastPixelUpdateMs;
    uint64_t lastInputSentMs;   // atomic (Sender writes, Receiver reads)
    uint64_t lastWatchdogReqMs;
    uint64_t lastInputRecoveryReqMs;

    uint8_t lastPointerButtons;
    int lastPointerX;
    int lastPointerY;
    bool hasLastPointerPos;
    uint64_t lastLeftDownMs;
    int lastLeftDownX;
    int lastLeftDownY;
    int leftClickCount;

    int lastErrno;
};

static void logInfo(const AppleRfbClient *c, const char *fmt, ...) {
    if (!c || !c->cbs.logInfo) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    c->cbs.logInfo("%s", buf);
}

static void logErr(const AppleRfbClient *c, const char *fmt, ...) {
    if (!c || !c->cbs.logError) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    c->cbs.logError("%s", buf);
}

static bool serverAccepts(const AppleRfbClient *c, uint8_t msgType) {
    if (!c || !c->extended) return false;
    size_t idx = msgType / 8u;
    if (idx >= sizeof(c->serverCommands)) return false;
    return (c->serverCommands[idx] & (uint8_t) (0x80u >> (msgType % 8u))) != 0;
}

// An encrypted session accepts clipboard text only once the record layer authenticates it;
// anything earlier could have been injected into the cleartext prelude.
static bool clipboardIsAuthenticated(const AppleRfbClient *c) {
    return !c->hasWrapKey || c->recordReadActive;
}

static bool supportsApplePasteboard(const AppleRfbClient *c) {
    // macOS screensharingd advertises clipboard sharing via 0x15 (AutoPasteboard) and/or 0x1f;
    // never fail closed on bit 0x1f alone when 0x15 is advertised.
    return c && c->extended && (serverAccepts(c, 0x1f) || serverAccepts(c, 0x15));
}

static uint8_t *ensureScratch(AppleRfbClient *c, size_t size) {
    if (c->scratchCap < size) {
        uint8_t *nb = (uint8_t *) realloc(c->scratchBuf, size);
        if (!nb) return nullptr;
        c->scratchBuf = nb;
        c->scratchCap = size;
    }
    return c->scratchBuf;
}

/******************************************************************************
 * Low-level Socket I/O
 *****************************************************************************/

static bool rawWriteAll(int sock, const uint8_t *buf, size_t len, int *errOut) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, buf + sent, len - sent,
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
        if (n > 0) {
            sent += (size_t) n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 5000);
            if (pr <= 0) {
                if (errOut) *errOut = (pr == 0) ? ETIMEDOUT : errno;
                return false;
            }
        } else {
            if (errOut) *errOut = (n == 0) ? ECONNRESET : errno;
            return false;
        }
    }
    return true;
}

static bool rawReadExact(int sock, uint8_t *buf, size_t len, int *errOut) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(sock, buf + got, len - got, 0);
        if (n > 0) {
            got += (size_t) n;
        } else if (n == 0) {
            if (errOut) *errOut = 0; // Clean disconnect
            return false;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 10000);
            if (pr <= 0) {
                if (errOut) *errOut = (pr == 0) ? ETIMEDOUT : errno;
                return false;
            }
        } else {
            if (errOut) *errOut = errno;
            return false;
        }
    }
    return true;
}

int appleRfbPeekIsAppleBanner(int sock, int timeoutMs) {
    if (sock < 0) return -1;
    uint8_t buf[12];
    int remainingMs = timeoutMs > 0 ? timeoutMs : 5000;
    const int stepMs = 100;

    while (remainingMs > 0) {
        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int wait = remainingMs < stepMs ? remainingMs : stepMs;
        int pr = poll(&pfd, 1, wait);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) {
            remainingMs -= wait;
            continue;
        }
        ssize_t n = recv(sock, buf, sizeof(buf), MSG_PEEK);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                remainingMs -= wait;
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = 0;
            return -1;
        }
        if (n < 12) {
            // Wait briefly for remaining banner bytes to arrive
            usleep(10000);
            remainingMs -= 10;
            continue;
        }
        if (memcmp(buf, "RFB 003.889\n", 12) == 0 || memcmp(buf, "RFB 003.888\n", 12) == 0) {
            return 1;
        }
        return 0;
    }
    errno = ETIMEDOUT;
    return -1;
}

/******************************************************************************
 * Crypto Primitives (LibreSSL + OpenSSL 3 Compatible)
 *****************************************************************************/

static bool aes128EcbCrypt(const uint8_t key[16], uint8_t *data, size_t len, bool encrypt) {
    if (len == 0 || (len % 16) != 0) return false;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    bool ok = false;
    int outLen = 0;
    if (encrypt) {
        ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
             EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
             EVP_EncryptUpdate(ctx, data, &outLen, data, (int) len) == 1 &&
             outLen == (int) len;
    } else {
        ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) == 1 &&
             EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
             EVP_DecryptUpdate(ctx, data, &outLen, data, (int) len) == 1 &&
             outLen == (int) len;
    }
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

static bool computeSha1RecordTag(uint32_t seq, const uint8_t *data, size_t len, uint8_t out[20]) {
    uint8_t seqBe[4];
    putBe32(seqBe, seq);
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (!md) return false;
    unsigned int outLen = 0;
    bool ok = EVP_DigestInit_ex(md, EVP_sha1(), nullptr) == 1 &&
              EVP_DigestUpdate(md, seqBe, 4) == 1 &&
              EVP_DigestUpdate(md, data, len) == 1 &&
              EVP_DigestFinal_ex(md, out, &outLen) == 1 &&
              outLen == 20;
    EVP_MD_CTX_free(md);
    return ok;
}

static bool computeSha512Parts(const uint8_t *const *parts, const size_t *lens, size_t count,
                               uint8_t out[64]) {
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (!md) return false;
    unsigned int outLen = 0;
    bool ok = EVP_DigestInit_ex(md, EVP_sha512(), nullptr) == 1;
    for (size_t i = 0; ok && i < count; ++i) {
        if (lens[i] > 0)
            ok = EVP_DigestUpdate(md, parts[i], lens[i]) == 1;
    }
    if (ok)
        ok = EVP_DigestFinal_ex(md, out, &outLen) == 1 && outLen == 64;
    EVP_MD_CTX_free(md);
    return ok;
}

static bool computeDigest(const EVP_MD *evpMd, const uint8_t *data, size_t len, uint8_t *out,
                          unsigned int expectedLen) {
    unsigned int outLen = 0;
    return EVP_Digest(data, len, out, &outLen, evpMd, nullptr) == 1 && outLen == expectedLen;
}

/******************************************************************************
 * Record-Layer Stream Read / Write
 *****************************************************************************/

static bool readNextRecord(AppleRfbClient *c) {
    uint8_t hdr[2];
    if (!rawReadExact(c->sock, hdr, 2, &c->lastErrno))
        return false;

    uint16_t size = be16(hdr);
    if (size < 32 || (size % 16) != 0 || size > kRecordMaxSize) {
        logErr(c, "AppleRfb: invalid record size %u", (unsigned) size);
        c->lastErrno = EPROTO;
        return false;
    }

    if (!rawReadExact(c->sock, c->readPlainBuf, size, &c->lastErrno))
        return false;

    int outLen = 0;
    if (EVP_DecryptUpdate(c->decCtx, c->readPlainBuf, &outLen, c->readPlainBuf, size) != 1 ||
        outLen != size) {
        logErr(c, "AppleRfb: record decrypt failed");
        c->lastErrno = EPROTO;
        return false;
    }

    size_t checksumOff = (size_t) size - kRecordChecksumSize;
    uint8_t expected[20];
    if (!computeSha1RecordTag(c->readSeq, c->readPlainBuf, checksumOff, expected) ||
        CRYPTO_memcmp(expected, c->readPlainBuf + checksumOff, kRecordChecksumSize) != 0) {
        logErr(c, "AppleRfb: record integrity check failed (seq=%u, size=%u)",
               c->readSeq, (unsigned) size);
        c->lastErrno = EPROTO;
        return false;
    }

    uint16_t bodyLen = be16(c->readPlainBuf);
    if ((size_t) bodyLen + 2u > checksumOff) {
        logErr(c, "AppleRfb: invalid record body length %u in %u-byte record",
               (unsigned) bodyLen, (unsigned) size);
        c->lastErrno = EPROTO;
        return false;
    }

    c->readSeq++;
    c->readPlainPos = 2;
    c->readPlainLen = 2u + bodyLen;
    return true;
}

static bool streamReadExact(AppleRfbClient *c, uint8_t *out, size_t len) {
    if (!c->recordReadActive)
        return rawReadExact(c->sock, out, len, &c->lastErrno);

    size_t copied = 0;
    while (copied < len) {
        if (c->readPlainPos >= c->readPlainLen) {
            if (!readNextRecord(c))
                return false;
            continue;
        }
        size_t avail = c->readPlainLen - c->readPlainPos;
        size_t take = (len - copied) < avail ? (len - copied) : avail;
        memcpy(out + copied, c->readPlainBuf + c->readPlainPos, take);
        c->readPlainPos += take;
        copied += take;
    }
    return true;
}

static bool streamReadU8(AppleRfbClient *c, uint8_t *v) {
    return streamReadExact(c, v, 1);
}

static bool streamReadU16(AppleRfbClient *c, uint16_t *v) {
    uint8_t b[2];
    if (!streamReadExact(c, b, 2)) return false;
    *v = be16(b);
    return true;
}

static bool streamReadU32(AppleRfbClient *c, uint32_t *v) {
    uint8_t b[4];
    if (!streamReadExact(c, b, 4)) return false;
    *v = be32(b);
    return true;
}

static bool streamDiscard(AppleRfbClient *c, size_t len) {
    uint8_t tmp[1024];
    while (len > 0) {
        size_t chunk = len < sizeof(tmp) ? len : sizeof(tmp);
        if (!streamReadExact(c, tmp, chunk)) return false;
        len -= chunk;
    }
    return true;
}

static bool streamWrite(AppleRfbClient *c, const uint8_t *data, size_t len) {
    pthread_mutex_lock(&c->writeMutex);
    bool ok = true;

    if (!c->recordWriteActive) {
        ok = rawWriteAll(c->sock, data, len, &c->lastErrno);
    } else {
        size_t offset = 0;
        while (offset < len && ok) {
            size_t chunk = len - offset;
            if (chunk > kRecordMaxBody) chunk = kRecordMaxBody;

            size_t size = (2u + chunk + kRecordChecksumSize + 15u) & ~((size_t) 15u);
            uint8_t *wire = c->writeWireBuf;
            putBe16(wire, (uint16_t) size);
            uint8_t *plain = wire + 2;

            putBe16(plain, (uint16_t) chunk);
            memcpy(plain + 2, data + offset, chunk);
            size_t checksumOff = size - kRecordChecksumSize;
            size_t padLen = checksumOff - (2u + chunk);
            if (padLen > 0)
                memset(plain + 2 + chunk, 0, padLen);

            if (!computeSha1RecordTag(c->writeSeq, plain, checksumOff, plain + checksumOff)) {
                c->lastErrno = EIO;
                ok = false;
                break;
            }

            int outLen = 0;
            if (EVP_EncryptUpdate(c->encCtx, plain, &outLen, plain, (int) size) != 1 ||
                outLen != (int) size) {
                c->lastErrno = EIO;
                ok = false;
                break;
            }

            c->writeSeq++;
            ok = rawWriteAll(c->sock, wire, 2u + size, &c->lastErrno);
            offset += chunk;
        }
    }

    pthread_mutex_unlock(&c->writeMutex);
    return ok;
}

/******************************************************************************
 * Wire Message Builders (Pure Helpers)
 *****************************************************************************/

void appleRfbBuildSetDisplayMessage(uint32_t displayId, uint8_t out[8]) {
    bool combineAll = (displayId == 0u || displayId == APPLE_ALL_DISPLAYS_WIRE);
    out[0] = 0x0d;
    out[1] = combineAll ? 1u : 0u;
    out[2] = 0;
    out[3] = 0;
    putBe32(out + 4, combineAll ? 0u : displayId);
}

bool appleRfbBuildVirtualDisplayMessage(int displayCount, int logicalWidth, int logicalHeight,
                                        double density, int maxWidth, int maxHeight,
                                        double refreshRate, bool hdr,
                                        uint8_t *out, size_t outCap, size_t *outLen) {
    if (!out || !outLen) return false;
    if (displayCount != 1) return false;
    if (logicalWidth <= 0 || logicalHeight <= 0) return false;
    if (density != 1.0 && density != 2.0) density = 1.0;
    if (refreshRate <= 0.0) refreshRate = 60.0;

    int pixelWidth = (int) llround((double) logicalWidth * density);
    int pixelHeight = (int) llround((double) logicalHeight * density);
    if (maxWidth < pixelWidth) maxWidth = pixelWidth;
    if (maxHeight < pixelHeight) maxHeight = pixelHeight;
    if (maxWidth > kVirtualDisplayLargestSide) maxWidth = kVirtualDisplayLargestSide;
    if (maxHeight > kVirtualDisplayLargestSide) maxHeight = kVirtualDisplayLargestSide;

    const size_t kDescHead = 0x9c;
    const size_t kModeSize = 0x1c;
    const size_t descSize = kDescHead + kModeSize; // 184 bytes (mode_count = 1)
    const size_t bodySize = 8u + (size_t) displayCount * descSize;
    const size_t totalSize = 4u + bodySize;

    if (outCap < totalSize || bodySize > 65535u)
        return false;

    memset(out, 0, totalSize);
    out[0] = 0x1d; // SetDisplayConfiguration
    out[1] = 0;
    putBe16(out + 2, (uint16_t) bodySize);
    putBe16(out + 4, 1);                      // version = 1
    putBe16(out + 6, (uint16_t) displayCount); // display_count (1 or 2)
    putBe32(out + 8, 0);                      // flags = 0

    float physW = (float) ((double) logicalWidth / 132.0 * 25.4);
    float physH = (float) ((double) logicalHeight / 132.0 * 25.4);

    for (int i = 0; i < displayCount; ++i) {
        uint8_t *d = out + 12u + (size_t) i * descSize;
        putBe16(d + 0x00, (uint16_t) descSize);

        char name[64];
        snprintf(name, sizeof(name), "AVNC Virtual Display %d", i + 1);
        size_t nameLen = strlen(name);
        if (nameLen > 119) nameLen = 119;
        memcpy(d + 0x02, name, nameLen);

        putBe32(d + 0x7a, 1u);               // display_flags: bit 0 = DYNAMIC_RESOLUTION
        putBe32(d + 0x7e, 4u);               // display_type: 4 = virtual display
        putBeFloat32(d + 0x82, physW);
        putBeFloat32(d + 0x86, physH);
        putBe32(d + 0x8a, (uint32_t) maxWidth);
        putBe32(d + 0x8e, (uint32_t) maxHeight);
        putBe16(d + 0x92, 0u);               // current_mode_index
        putBe16(d + 0x94, 0u);               // preferred_mode_index
        putBe32(d + 0x96, 7u);               // rotations/reserved (matches Screen Sharing.app)
        putBe16(d + 0x9a, 1u);               // mode_count = 1

        uint8_t *m = d + kDescHead;
        putBe32(m + 0x00, (uint32_t) pixelWidth);
        putBe32(m + 0x04, (uint32_t) pixelHeight);
        putBe32(m + 0x08, (uint32_t) logicalWidth);
        putBe32(m + 0x0c, (uint32_t) logicalHeight);
        putBeFloat64(m + 0x10, refreshRate);
        putBe32(m + 0x18, hdr ? 1u : 0u);
    }

    *outLen = totalSize;
    return true;
}

bool appleRfbParseDisplayLayout(const uint8_t *payload, size_t len, AppleDisplayLayout *out) {
    const size_t kHeaderSize = 0x14;
    const size_t kRecordSize = 0x38;
    if (!payload || !out || len < kHeaderSize)
        return false;

    uint16_t count = be16(payload + 0x12);
    if (count > APPLE_MAX_LAYOUT_DISPLAYS || len < kHeaderSize + (size_t) count * kRecordSize)
        return false;

    memset(out, 0, sizeof(*out));
    out->version = be16(payload + 0x00);
    out->desktopWidth = be16(payload + 0x02);
    out->desktopHeight = be16(payload + 0x04);
    out->fbWidth = be16(payload + 0x06);
    out->fbHeight = be16(payload + 0x08);
    out->currentDisplayId = be32(payload + 0x0a);
    out->sessionState = be32(payload + 0x0e);
    out->count = count;

    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t *r = payload + kHeaderSize + (size_t) i * kRecordSize;
        AppleDisplayInfo *d = &out->displays[i];

        d->density = beFloat64(r + 0x00);
        d->scale = beFloat64(r + 0x08);
        d->id = be32(r + 0x10);

        int16_t lTop = be16s(r + 0x14);
        int16_t lLeft = be16s(r + 0x16);
        int16_t lBottom = be16s(r + 0x18);
        int16_t lRight = be16s(r + 0x1a);
        d->logicalX = lLeft;
        d->logicalY = lTop;
        d->logicalWidth = (lRight > lLeft) ? (uint16_t) (lRight - lLeft) : 0u;
        d->logicalHeight = (lBottom > lTop) ? (uint16_t) (lBottom - lTop) : 0u;

        int16_t bTop = be16s(r + 0x1c);
        int16_t bLeft = be16s(r + 0x1e);
        int16_t bBottom = be16s(r + 0x20);
        int16_t bRight = be16s(r + 0x22);
        d->backingX = bLeft;
        d->backingY = bTop;
        d->backingWidth = (bRight > bLeft) ? (uint16_t) (bRight - bLeft) : 0u;
        d->backingHeight = (bBottom > bTop) ? (uint16_t) (bBottom - bTop) : 0u;

        d->flags = be32(r + 0x24);

        if (!(d->scale > 0.0 && d->scale <= 1.0))
            d->scale = 1.0;
        if (d->density == 0.0 && d->logicalWidth > 0)
            d->density = (double) d->backingWidth / (double) d->logicalWidth / d->scale;
    }

    return true;
}

uint8_t appleRfbMapPointerButtons(uint8_t standardMask) {
    // Apple RFB 3.889 maps bit 1 to Right and bit 2 to Middle (CGMouseButton indices).
    uint8_t mapped = standardMask & (uint8_t) ~(0x02u | 0x04u);
    if (standardMask & 0x02u) mapped |= 0x04u;
    if (standardMask & 0x04u) mapped |= 0x02u;
    return mapped;
}

bool appleRfbParseKeyboardSource(const uint8_t *payload, size_t len,
                                 char *sourceIdOut, size_t sourceIdCap,
                                 bool *secureInputOut) {
    if (!payload || len < 8u || !sourceIdOut || sourceIdCap == 0)
        return false;

    uint32_t flags = be32(payload + 2);
    uint16_t idLen = be16(payload + 6);
    if (8u + (size_t) idLen > len)
        return false;

    size_t copyLen = (size_t) idLen;
    if (copyLen >= sourceIdCap)
        copyLen = sourceIdCap - 1u;
    if (copyLen > 0)
        memcpy(sourceIdOut, payload + 8, copyLen);
    sourceIdOut[copyLen] = '\0';

    if (secureInputOut)
        *secureInputOut = (flags & 0x01u) != 0;
    return true;
}

bool appleRfbBuildKeyboardSourceMessage(const char *sourceId,
                                        uint8_t *out, size_t outCap, size_t *outLen) {
    if (!out || !outLen)
        return false;
    size_t idLen = sourceId ? strlen(sourceId) : 0u;
    if (idLen > 65531u)
        return false;
    size_t bodyLen = 4u + idLen;
    size_t total = 4u + bodyLen;
    if (outCap < total)
        return false;

    out[0] = 0x1a;
    out[1] = 0;
    putBe16(out + 2, (uint16_t) bodyLen);
    putBe16(out + 4, 1u);
    putBe16(out + 6, (uint16_t) idLen);
    if (idLen > 0)
        memcpy(out + 8, sourceId, idLen);

    *outLen = total;
    return true;
}

bool appleRfbBuildEncryptedKeyMessage(const uint8_t wrapKey[16],
                                      uint16_t macKeyCode, uint32_t keysym, bool down,
                                      uint8_t out[18]) {
    if (!wrapKey || !out)
        return false;
    uint8_t block[16];
    memset(block, 0, sizeof(block));
    block[0] = 0xffu;
    block[1] = down ? 1u : 0u;
    putBe32(block + 2, keysym);
    putBe16(block + 12, kMacKeyboardTypeAnsi);
    putBe16(block + 14, macKeyCode);

    if (!aes128EcbCrypt(wrapKey, block, sizeof(block), true))
        return false;

    out[0] = 0x10;
    out[1] = kMacKeyFlagKeyCodeValid | kMacKeyFlagPreferCode;
    memcpy(out + 2, block, sizeof(block));
    return true;
}

bool appleRfbBuildEncryptedPointerMessage(const uint8_t wrapKey[16],
                                          uint8_t appleButtons, int clickCount,
                                          int wireX, int wireY, uint8_t out[18]) {
    if (!wrapKey || !out)
        return false;
    uint8_t block[16];
    memset(block, 0, sizeof(block));
    block[10] = 0xffu;
    block[11] = appleButtons;
    putBe16(block + 12, clampU16(wireX));
    putBe16(block + 14, clampU16(wireY));

    if (!aes128EcbCrypt(wrapKey, block, sizeof(block), true))
        return false;

    out[0] = 0x10;
    if (clickCount > 0) {
        int clamped = clickCount > 7 ? 7 : clickCount;
        out[1] = (uint8_t) (0x01u | ((uint8_t) clamped << 1));
    } else {
        out[1] = 0;
    }
    memcpy(out + 2, block, sizeof(block));
    return true;
}

static void buildViewerInfoMessage(uint8_t out[66]) {
    memset(out, 0, 66);
    out[0] = 0x21; // ViewerInfo
    putBe16(out + 2, 62);
    putBe16(out + 4, 1); // version
    const uint32_t fields[7] = {2, 6, 1, 0, 15, 3, 0}; // appId=2, appVer=6.1.0, osVer=15.3.0
    for (int i = 0; i < 7; ++i) {
        putBe32(out + 6 + i * 4, fields[i]);
    }
    uint8_t *bitmap = out + 34;
    bitmap[0] = 0xb0;
    bitmap[2] = 0x0c;
    bitmap[3] = 0x03;
    bitmap[4] = 0x90;
    bitmap[10] = 0x40;
}

static void buildSetModeMessage(uint8_t out[4], bool observeOnly) {
    out[0] = 0x0a;
    out[1] = 0;
    putBe16(out + 2, observeOnly ? 0u : 1u);
}

static void buildAutoPasteboardMessage(uint8_t out[8], bool enabled) {
    memset(out, 0, 8);
    out[0] = 0x15;
    out[3] = enabled ? 1u : 2u;
}

static void buildSetPixelFormatBGRA(uint8_t out[20]) {
    memset(out, 0, 20);
    out[0] = 0x00; // SetPixelFormat
    out[4] = 32;   // bpp
    out[5] = 24;   // depth
    out[6] = 0;    // little-endian
    out[7] = 1;    // true-color
    putBe16(out + 8, 255);
    putBe16(out + 10, 255);
    putBe16(out + 12, 255);
    out[14] = 16;  // redShift
    out[15] = 8;   // greenShift
    out[16] = 0;   // blueShift
}

static void buildFbUpdateRequest(uint8_t out[10], bool incremental, int width, int height) {
    out[0] = 0x03;
    out[1] = incremental ? 1u : 0u;
    putBe16(out + 2, 0);
    putBe16(out + 4, 0);
    putBe16(out + 6, clampU16(width));
    putBe16(out + 8, clampU16(height));
}

static void buildAutoFbUpdate(uint8_t out[16], uint32_t intervalUs, int width, int height) {
    out[0] = 0x09;
    out[1] = 0;
    putBe16(out + 2, 1);
    putBe32(out + 4, intervalUs);
    putBe16(out + 8, 0);
    putBe16(out + 10, 0);
    putBe16(out + 12, clampU16(width));
    putBe16(out + 14, clampU16(height));
}

static bool sendEncodings(AppleRfbClient *c) {
    int32_t encs[24];
    size_t count = 0;

    if (c->config.useRawEncoding) {
        encs[count++] = kEncRaw;
        encs[count++] = kEncCopyRect;
    } else {
        if (c->config.imageQuality <= 4) {
            encs[count++] = kEncAppleThousands;
        }
        encs[count++] = kEncZRLE;
        encs[count++] = kEncZlib;
        encs[count++] = kEncCopyRect;
        encs[count++] = kEncRaw;
    }

    if (c->config.useLocalCursor) {
        encs[count++] = kEncCursorImage;
        encs[count++] = kEncCursorPosition;
        encs[count++] = kEncPointerPos;
    }

    encs[count++] = kEncDesktopSize;
    encs[count++] = kEncDisplayInfo;
    encs[count++] = kEncDisplayLayout;
    encs[count++] = kEncVendorKeysyms;
    encs[count++] = kEncKeyboardSource;
    encs[count++] = kEncDeviceInfo;
    encs[count++] = kEncLastRect;

    uint8_t msg[4 + 24 * 4];
    msg[0] = 0x02;
    msg[1] = 0;
    putBe16(msg + 2, (uint16_t) count);
    for (size_t i = 0; i < count; ++i) {
        putBe32(msg + 4 + i * 4, (uint32_t) encs[i]);
    }
    return streamWrite(c, msg, 4 + count * 4);
}

static inline void markInputActivity(AppleRfbClient *c, uint64_t now) {
    __atomic_store_n(&c->lastInputSentMs, now, __ATOMIC_RELEASE);
    __atomic_store_n(&c->hasInputSinceLastPixel, 1, __ATOMIC_RELEASE);
}

static bool sendUpdateArm(AppleRfbClient *c, bool nonIncremental) {
    int fbW = __atomic_load_n(&c->fbWidth, __ATOMIC_ACQUIRE);
    int fbH = __atomic_load_n(&c->fbHeight, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&c->paused, __ATOMIC_ACQUIRE)) {
        if (serverAccepts(c, 0x09)) {
            uint8_t arm[16];
            buildAutoFbUpdate(arm, 0xffffffffu, fbW, fbH);
            return streamWrite(c, arm, sizeof(arm));
        }
        return true;
    }

    if (fbW <= 0 || fbH <= 0)
        return true;

    uint8_t req[10];
    buildFbUpdateRequest(req, !nonIncremental, fbW, fbH);
    if (!streamWrite(c, req, sizeof(req)))
        return false;

    if (serverAccepts(c, 0x09)) {
        uint8_t arm[16];
        buildAutoFbUpdate(arm, kDefaultUpdateIntervalUs, fbW, fbH);
        if (!streamWrite(c, arm, sizeof(arm)))
            return false;
    }
    return true;
}

/******************************************************************************
 * Authentication: Apple DH (30), Direct SRP (36), RSA1 / RSA-SRP (33)
 *****************************************************************************/

static bool buildCredentialBlock(const char *username, const char *password, uint8_t block[128]) {
    size_t uLen = strlen(username);
    size_t pLen = strlen(password);
    if (uLen > 63 || pLen > 63) return false;
    if (RAND_bytes(block, 128) != 1)
        memset(block, 0, 128);
    memcpy(block, username, uLen + 1);
    memcpy(block + 64, password, pLen + 1);
    return true;
}

static bool buildSrpIdentity(const char *username, uint8_t *out, size_t outCap, size_t *outLen) {
    size_t uLen = strlen(username);
    if (uLen > 65500) return false;
    size_t bodyLen = 2 + 2 + uLen + 2 + 1;
    if (4 + bodyLen > outCap) return false;
    putBe32(out, (uint32_t) bodyLen);
    putBe16(out + 4, 0);
    putBe16(out + 6, (uint16_t) uLen);
    memcpy(out + 8, username, uLen);
    putBe16(out + 8 + uLen, 0);
    out[10 + uLen] = 0;
    *outLen = 4 + bodyLen;
    return true;
}

static bool buildRsaEnvelope(uint16_t kind, const uint8_t *body, size_t bodyLen,
                             uint8_t **outBuf, size_t *outLen) {
    size_t total = 12 + bodyLen;
    uint8_t *buf = (uint8_t *) malloc(total);
    if (!buf) return false;
    putBe32(buf, (uint32_t) (8 + bodyLen));
    buf[4] = 0x01;
    buf[5] = 0x00;
    buf[6] = 'R';
    buf[7] = 'S';
    buf[8] = 'A';
    buf[9] = '1';
    putBe16(buf + 10, kind);
    if (bodyLen > 0) memcpy(buf + 12, body, bodyLen);
    *outBuf = buf;
    *outLen = total;
    return true;
}

static bool readSecurityResult(AppleRfbClient *c, uint32_t *resultOut) {
    uint32_t res = 1;
    if (!streamReadU32(c, &res))
        return false;
    if (resultOut) *resultOut = res;
    if (res != 0) {
        logErr(c, "AppleRfb: authentication failed (result=%u)", res);
        c->lastErrno = EACCES;
        return false;
    }
    return true;
}

typedef bool (*SrpReadFn)(AppleRfbClient *c, uint8_t **outBuf, size_t *outLen);
typedef bool (*SrpWriteFn)(AppleRfbClient *c, const uint8_t *fields, size_t fieldsLen);

static bool isKnownSrpGroup(const uint8_t *modulus, size_t length, const BIGNUM *n, const BIGNUM *g) {
    // RFC 5054, Appendix A. The groups of 3072 bits and above use the RFC 3526 primes.
    if (length == 256) {
        static const uint8_t rfc5054Sha256[32] = {
            0x91,0xb7,0x1d,0x6b,0x40,0xd4,0x39,0x95,0x45,0x68,0xd4,0x12,0xe8,0x83,0xde,0x51,
            0x86,0xf9,0x38,0x1e,0x25,0xae,0xf3,0x6e,0x7a,0x46,0x07,0x72,0x2f,0x7e,0x15,0xca
        };
        uint8_t hash[32];
        return BN_is_word(g, 2) && computeDigest(EVP_sha256(), modulus, length, hash, sizeof(hash)) &&
               memcmp(hash, rfc5054Sha256, sizeof(hash)) == 0;
    }
    if (!BN_is_word(g, length == 1024 ? 19 : 5)) return false;
    BIGNUM *standard;
    switch (length) {
        case 384:  standard = BN_get_rfc3526_prime_3072(nullptr); break;
        case 512:  standard = BN_get_rfc3526_prime_4096(nullptr); break;
        case 768:  standard = BN_get_rfc3526_prime_6144(nullptr); break;
        case 1024: standard = BN_get_rfc3526_prime_8192(nullptr); break;
        default: return false;
    }
    bool matches = standard && BN_cmp(n, standard) == 0;
    BN_free(standard);
    return matches;
}

static bool runSrpExchange(AppleRfbClient *c, const char *password,
                           SrpReadFn readMsg, SrpWriteFn writeMsg,
                           uint8_t wrapKeyOut[16]) {
    uint8_t *challenge = nullptr;
    size_t challengeLen = 0;
    if (!readMsg(c, &challenge, &challengeLen))
        return false;

    bool ok = false;
    size_t pos = 0;
    const uint8_t *modBytes = nullptr;
    const uint8_t *genBytes = nullptr;
    const uint8_t *salt = nullptr;
    const uint8_t *srvPubBytes = nullptr;
    const uint8_t *options = nullptr;
    uint16_t modLen = 0, genLen = 0, srvPubLen = 0, optLen = 0;
    uint8_t saltLen = 0;
    uint64_t iterations = 0;

    BIGNUM *bnN = nullptr, *bnG = nullptr, *bnB = nullptr;
    BIGNUM *bnX = nullptr, *bnK = nullptr, *bnA = nullptr, *bnPubA = nullptr;
    BIGNUM *bnU = nullptr, *bnGx = nullptr, *bnKgx = nullptr, *bnBase = nullptr;
    BIGNUM *bnExp = nullptr, *bnS = nullptr;
    BN_CTX *bnCtx = nullptr;

    uint8_t *gPad = nullptr, *aPad = nullptr, *bPad = nullptr, *sPad = nullptr;
    uint8_t *resp = nullptr;
    uint8_t *verifyMsg = nullptr;
    size_t verifyLen = 0;

    auto take = [&](size_t n) -> const uint8_t * {
        if (pos + n > challengeLen) return nullptr;
        const uint8_t *p = challenge + pos;
        pos += n;
        return p;
    };

    const uint8_t *p;
    if (!take(1)) goto cleanup;
    if (!(p = take(2))) goto cleanup;
    modLen = be16(p);
    if (!(modBytes = take(modLen))) goto cleanup;
    if (!(p = take(2))) goto cleanup;
    genLen = be16(p);
    if (!(genBytes = take(genLen))) goto cleanup;
    if (!(p = take(1))) goto cleanup;
    saltLen = p[0];
    if (!(salt = take(saltLen))) goto cleanup;
    if (!(p = take(2))) goto cleanup;
    srvPubLen = be16(p);
    if (!(srvPubBytes = take(srvPubLen))) goto cleanup;
    if (!(p = take(8))) goto cleanup;
    iterations = be64(p);
    if (!(p = take(2))) goto cleanup;
    optLen = be16(p);
    if (!(options = take(optLen))) goto cleanup;

    if (modLen < 256 || modLen > 1024 || genLen == 0 || genLen > modLen ||
        srvPubLen == 0 || srvPubLen > modLen || saltLen == 0 || iterations == 0 || iterations > kMaxSrpIterations) {
        logErr(c, "AppleRfb: invalid SRP parameters (mod=%u, iter=%llu)",
               (unsigned) modLen, (unsigned long long) iterations);
        c->lastErrno = EPROTO;
        goto cleanup;
    }

    bnCtx = BN_CTX_new();
    bnN = BN_bin2bn(modBytes, modLen, nullptr);
    bnG = BN_bin2bn(genBytes, genLen, nullptr);
    bnB = BN_bin2bn(srvPubBytes, srvPubLen, nullptr);
    if (!bnCtx || !bnN || !bnG || !bnB) goto cleanup;

    if (!isKnownSrpGroup(modBytes, modLen, bnN, bnG) || BN_cmp(bnB, bnN) >= 0 || BN_is_zero(bnB)) {
        c->lastErrno = EPROTO;
        goto cleanup;
    }

    gPad = (uint8_t *) malloc(modLen);
    aPad = (uint8_t *) malloc(modLen);
    bPad = (uint8_t *) malloc(modLen);
    sPad = (uint8_t *) malloc(modLen);
    if (!gPad || !aPad || !bPad || !sPad) goto cleanup;
    if (!bnToFixedBytes(bnG, gPad, modLen) || !bnToFixedBytes(bnB, bPad, modLen))
        goto cleanup;

    {
        uint8_t derived[128];
        WipeOnExit derivedWipe{derived, sizeof(derived)};
        if (PKCS5_PBKDF2_HMAC(password, (int) strlen(password),
                              salt, (int) saltLen, (int) iterations,
                              EVP_sha512(), sizeof(derived), derived) != 1) {
            goto cleanup;
        }

        uint8_t colon = ':';
        const uint8_t *innerParts[2] = {&colon, derived};
        const size_t innerLens[2] = {1, sizeof(derived)};
        uint8_t innerHash[64];
        if (!computeSha512Parts(innerParts, innerLens, 2, innerHash)) goto cleanup;

        const uint8_t *xParts[2] = {salt, innerHash};
        const size_t xLens[2] = {saltLen, sizeof(innerHash)};
        uint8_t xHash[64];
        WipeOnExit xHashWipe{xHash, sizeof(xHash)};
        if (!computeSha512Parts(xParts, xLens, 2, xHash)) goto cleanup;
        bnX = BN_bin2bn(xHash, sizeof(xHash), nullptr);

        const uint8_t *kParts[2] = {modBytes, gPad};
        const size_t kLens[2] = {modLen, modLen};
        uint8_t kHash[64];
        if (!computeSha512Parts(kParts, kLens, 2, kHash)) goto cleanup;
        bnK = BN_bin2bn(kHash, sizeof(kHash), nullptr);

        uint8_t randPriv[64];
        WipeOnExit randPrivWipe{randPriv, sizeof(randPriv)};
        if (RAND_bytes(randPriv, sizeof(randPriv)) != 1) goto cleanup;
        bnA = BN_bin2bn(randPriv, sizeof(randPriv), nullptr);

        bnPubA = BN_new();
        if (!bnX || !bnK || !bnA || !bnPubA) goto cleanup;
        if (BN_mod_exp(bnPubA, bnG, bnA, bnN, bnCtx) != 1) goto cleanup;
        if (!bnToFixedBytes(bnPubA, aPad, modLen)) goto cleanup;

        const uint8_t *uParts[2] = {aPad, bPad};
        const size_t uLens[2] = {modLen, modLen};
        uint8_t uHash[64];
        if (!computeSha512Parts(uParts, uLens, 2, uHash)) goto cleanup;
        bnU = BN_bin2bn(uHash, sizeof(uHash), nullptr);

        bnGx = BN_new();
        bnKgx = BN_new();
        bnBase = BN_new();
        bnExp = BN_new();
        bnS = BN_new();
        if (!bnU || !bnGx || !bnKgx || !bnBase || !bnExp || !bnS) goto cleanup;

        if (BN_mod_exp(bnGx, bnG, bnX, bnN, bnCtx) != 1) goto cleanup;
        if (BN_mod_mul(bnKgx, bnK, bnGx, bnN, bnCtx) != 1) goto cleanup;
        if (BN_mod_sub(bnBase, bnB, bnKgx, bnN, bnCtx) != 1) goto cleanup;
        if (BN_mul(bnExp, bnU, bnX, bnCtx) != 1) goto cleanup;
        if (BN_add(bnExp, bnExp, bnA) != 1) goto cleanup;
        if (BN_mod_exp(bnS, bnBase, bnExp, bnN, bnCtx) != 1) goto cleanup;
        if (!bnToFixedBytes(bnS, sPad, modLen)) goto cleanup;

        uint8_t sessionKey[64];
        WipeOnExit sessionKeyWipe{sessionKey, sizeof(sessionKey)};
        if (!computeDigest(EVP_sha512(), sPad, modLen, sessionKey, 64)) goto cleanup;

        uint8_t hN[64], hG[64], hXor[64], hEmpty[64];
        if (!computeDigest(EVP_sha512(), modBytes, modLen, hN, 64)) goto cleanup;
        if (!computeDigest(EVP_sha512(), gPad, modLen, hG, 64)) goto cleanup;
        for (int i = 0; i < 64; ++i) hXor[i] = hN[i] ^ hG[i];
        if (!computeDigest(EVP_sha512(), (const uint8_t *) "", 0, hEmpty, 64)) goto cleanup;

        const uint8_t *m1Parts[6] = {hXor, hEmpty, salt, aPad, bPad, sessionKey};
        const size_t m1Lens[6] = {64, 64, saltLen, modLen, modLen, 64};
        uint8_t m1[64];
        if (!computeSha512Parts(m1Parts, m1Lens, 6, m1)) goto cleanup;

        const uint8_t *m2Parts[3] = {aPad, m1, sessionKey};
        const size_t m2Lens[3] = {modLen, 64, 64};
        uint8_t expectedM2[64];
        if (!computeSha512Parts(m2Parts, m2Lens, 3, expectedM2)) goto cleanup;

        size_t respLen = 2u + modLen + 1u + 64u + 2u + optLen + 1u + 16u;
        resp = (uint8_t *) malloc(respLen);
        if (!resp) goto cleanup;

        size_t o = 0;
        putBe16(resp + o, modLen); o += 2;
        memcpy(resp + o, aPad, modLen); o += modLen;
        resp[o++] = 64;
        memcpy(resp + o, m1, 64); o += 64;
        putBe16(resp + o, optLen); o += 2;
        if (optLen > 0) { memcpy(resp + o, options, optLen); o += optLen; }
        resp[o++] = 16;
        if (RAND_bytes(resp + o, 16) != 1) memset(resp + o, 0, 16);

        if (!writeMsg(c, resp, respLen)) goto cleanup;

        if (!readMsg(c, &verifyMsg, &verifyLen)) goto cleanup;
        if (verifyLen < 1 || verifyMsg[0] != 64 || verifyLen < 65u ||
            CRYPTO_memcmp(verifyMsg + 1, expectedM2, 64) != 0) {
            logErr(c, "AppleRfb: SRP server proof mismatch");
            c->lastErrno = EACCES;
            goto cleanup;
        }

        uint8_t sha256Key[32];
        WipeOnExit sha256KeyWipe{sha256Key, sizeof(sha256Key)};
        if (!computeDigest(EVP_sha256(), sessionKey, sizeof(sessionKey), sha256Key, 32))
            goto cleanup;
        memcpy(wrapKeyOut, sha256Key, 16);
        ok = true;
    }

cleanup:
    free(challenge);
    free(gPad);
    free(aPad);
    free(bPad);
    if (sPad) OPENSSL_cleanse(sPad, modLen);
    free(sPad);
    free(resp);
    free(verifyMsg);
    BN_free(bnN);
    BN_free(bnG);
    BN_free(bnB);
    BN_clear_free(bnX);
    BN_free(bnK);
    BN_clear_free(bnA);
    BN_free(bnPubA);
    BN_free(bnU);
    BN_free(bnGx);
    BN_free(bnKgx);
    BN_free(bnBase);
    BN_free(bnExp);
    BN_clear_free(bnS);
    BN_CTX_free(bnCtx);
    return ok;
}

static bool srp36Read(AppleRfbClient *c, uint8_t **outBuf, size_t *outLen) {
    uint32_t size = 0;
    if (!streamReadU32(c, &size)) return false;
    if (size < 4) {
        // screensharingd sends SecurityResult directly on rejected SRP proof
        logErr(c, "AppleRfb: SRP36 rejected (result=%u)", size);
        c->lastErrno = EACCES;
        return false;
    }
    if (size > 65536u) {
        c->lastErrno = EPROTO;
        return false;
    }
    uint8_t *raw = (uint8_t *) malloc(size);
    if (!raw) return false;
    if (!streamReadExact(c, raw, size)) {
        free(raw);
        return false;
    }
    size_t bodyLen = size - 4;
    uint8_t *body = (uint8_t *) malloc(bodyLen > 0 ? bodyLen : 1);
    if (!body) {
        free(raw);
        return false;
    }
    if (bodyLen > 0) memcpy(body, raw + 4, bodyLen);
    free(raw);
    *outBuf = body;
    *outLen = bodyLen;
    return true;
}

static bool srp36Write(AppleRfbClient *c, const uint8_t *fields, size_t fieldsLen) {
    size_t total = 8 + fieldsLen;
    uint8_t *msg = (uint8_t *) malloc(total);
    if (!msg) return false;
    putBe32(msg, (uint32_t) (4 + fieldsLen));
    putBe32(msg + 4, (uint32_t) fieldsLen);
    memcpy(msg + 8, fields, fieldsLen);
    bool ok = streamWrite(c, msg, total);
    free(msg);
    return ok;
}

static bool authenticateAppleSRP(AppleRfbClient *c, const char *username, const char *password) {
    uint8_t identity[256];
    size_t idLen = 0;
    if (!buildSrpIdentity(username, identity, sizeof(identity), &idLen))
        return false;

    uint8_t msg[5 + sizeof(identity)];
    msg[0] = kSecAppleSRP;
    putBe32(msg + 1, (uint32_t) idLen);
    memcpy(msg + 5, identity, idLen);
    if (!streamWrite(c, msg, 5 + idLen))
        return false;

    if (!runSrpExchange(c, password, srp36Read, srp36Write, c->wrapKey))
        return false;

    c->hasWrapKey = true;
    return readSecurityResult(c, nullptr);
}

static bool rsa33Read(AppleRfbClient *c, uint8_t **outBuf, size_t *outLen) {
    uint32_t size = 0;
    if (!streamReadU32(c, &size)) return false;
    if (size > 65536u || size < 6) {
        c->lastErrno = EPROTO;
        return false;
    }
    uint8_t *reply = (uint8_t *) malloc(size);
    if (!reply) return false;
    if (!streamReadExact(c, reply, size)) {
        free(reply);
        return false;
    }
    uint16_t innerLen = be16(reply + 4);
    if (size == 6 && innerLen == 0) {
        free(reply);
        readSecurityResult(c, nullptr);
        c->lastErrno = EACCES;
        return false;
    }
    if (innerLen < 4 || (size_t) innerLen + 6u > size) {
        free(reply);
        c->lastErrno = EPROTO;
        return false;
    }
    size_t payloadLen = innerLen - 4;
    uint8_t *payload = (uint8_t *) malloc(payloadLen > 0 ? payloadLen : 1);
    if (!payload) {
        free(reply);
        return false;
    }
    if (payloadLen > 0) memcpy(payload, reply + 10, payloadLen);
    free(reply);
    *outBuf = payload;
    *outLen = payloadLen;
    return true;
}

static bool rsa33Write(AppleRfbClient *c, const uint8_t *fields, size_t fieldsLen) {
    size_t bodyLen = 6 + fieldsLen;
    if (bodyLen < 1076 - 8) bodyLen = 1076 - 8;
    uint8_t *body = (uint8_t *) calloc(1, bodyLen);
    if (!body) return false;
    putBe16(body, (uint16_t) (4 + fieldsLen));
    putBe32(body + 2, (uint32_t) fieldsLen);
    memcpy(body + 6, fields, fieldsLen);

    uint8_t *env = nullptr;
    size_t envLen = 0;
    bool ok = buildRsaEnvelope(2, body, bodyLen, &env, &envLen) &&
              streamWrite(c, env, envLen);
    free(body);
    free(env);
    return ok;
}

static bool rsaEncryptPkcs1(const uint8_t *der, size_t derLen,
                            const uint8_t *in, size_t inLen,
                            uint8_t **outBuf, size_t *outLen) {
    const unsigned char *p = der;
    EVP_PKEY *pkey = d2i_PUBKEY(nullptr, &p, (long) derLen);
    if (!pkey) {
        p = der;
        RSA *rsa = d2i_RSAPublicKey(nullptr, &p, (long) derLen);
        if (rsa) {
            pkey = EVP_PKEY_new();
            if (!pkey || EVP_PKEY_assign_RSA(pkey, rsa) != 1) {
                RSA_free(rsa);
                EVP_PKEY_free(pkey);
                return false;
            }
        }
    }
    if (!pkey) return false;

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, nullptr);
    bool ok = false;
    uint8_t *enc = nullptr;
    size_t encLen = 0;

    if (ctx &&
        EVP_PKEY_encrypt_init(ctx) > 0 &&
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) > 0 &&
        EVP_PKEY_encrypt(ctx, nullptr, &encLen, in, inLen) > 0) {
        enc = (uint8_t *) malloc(encLen);
        if (enc && EVP_PKEY_encrypt(ctx, enc, &encLen, in, inLen) > 0) {
            *outBuf = enc;
            *outLen = encLen;
            enc = nullptr;
            ok = true;
        }
    }

    free(enc);
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

static bool authenticateAppleRSA(AppleRfbClient *c, const char *username, const char *password,
                                 bool serverOffersSRP) {
    uint8_t zero2[2] = {0, 0};
    uint8_t *reqEnv = nullptr;
    size_t reqEnvLen = 0;
    if (!buildRsaEnvelope(0, zero2, 2, &reqEnv, &reqEnvLen)) return false;

    uint8_t *firstMsg = (uint8_t *) malloc(1 + reqEnvLen);
    if (!firstMsg) {
        free(reqEnv);
        return false;
    }
    firstMsg[0] = kSecAppleRSA;
    memcpy(firstMsg + 1, reqEnv, reqEnvLen);
    free(reqEnv);

    bool ok = streamWrite(c, firstMsg, 1 + reqEnvLen);
    free(firstMsg);
    if (!ok) return false;

    uint32_t replySize = 0;
    if (!streamReadU32(c, &replySize) || replySize < 6 || replySize > 65536u) {
        c->lastErrno = EPROTO;
        return false;
    }
    uint8_t *reply = (uint8_t *) malloc(replySize);
    if (!reply) return false;
    if (!streamReadExact(c, reply, replySize)) {
        free(reply);
        return false;
    }
    uint16_t derLen = be16(reply + 4);
    if ((size_t) derLen + 6u > replySize) {
        free(reply);
        c->lastErrno = EPROTO;
        return false;
    }
    const uint8_t *der = reply + 6;

    if (serverOffersSRP) {
        uint8_t identity[256];
        size_t idLen = 0;
        if (!buildSrpIdentity(username, identity, sizeof(identity), &idLen)) {
            free(reply);
            return false;
        }
        uint8_t *encId = nullptr;
        size_t encIdLen = 0;
        ok = rsaEncryptPkcs1(der, derLen, identity, idLen, &encId, &encIdLen);
        free(reply);
        if (!ok) {
            c->lastErrno = EPROTO;
            return false;
        }

        size_t bodyLen = 2 + encIdLen + 384;
        uint8_t *body = (uint8_t *) calloc(1, bodyLen);
        if (!body) {
            free(encId);
            return false;
        }
        putBe16(body, (uint16_t) encIdLen);
        memcpy(body + 2, encId, encIdLen);
        free(encId);

        uint8_t *env = nullptr;
        size_t envLen = 0;
        ok = buildRsaEnvelope(2, body, bodyLen, &env, &envLen) &&
             streamWrite(c, env, envLen);
        free(body);
        free(env);
        if (!ok) return false;

        if (!runSrpExchange(c, password, rsa33Read, rsa33Write, c->wrapKey))
            return false;
        c->hasWrapKey = true;
        return readSecurityResult(c, nullptr);
    } else {
        // Legacy RSA1-Plain
        if (RAND_bytes(c->wrapKey, 16) != 1) {
            free(reply);
            return false;
        }
        uint8_t creds[128];
        if (!buildCredentialBlock(username, password, creds) ||
            !aes128EcbCrypt(c->wrapKey, creds, sizeof(creds), true)) {
            free(reply);
            return false;
        }
        uint8_t *encKey = nullptr;
        size_t encKeyLen = 0;
        ok = rsaEncryptPkcs1(der, derLen, c->wrapKey, 16, &encKey, &encKeyLen);
        free(reply);
        if (!ok) return false;

        size_t bodyLen = 128 + 2 + encKeyLen;
        uint8_t *body = (uint8_t *) malloc(bodyLen);
        if (!body) {
            free(encKey);
            return false;
        }
        memcpy(body, creds, 128);
        putLe16(body + 128, (uint16_t) encKeyLen);
        memcpy(body + 130, encKey, encKeyLen);
        free(encKey);

        uint8_t *env = nullptr;
        size_t envLen = 0;
        ok = buildRsaEnvelope(1, body, bodyLen, &env, &envLen) &&
             streamWrite(c, env, envLen);
        free(body);
        free(env);
        if (!ok) return false;

        uint32_t ack = 0;
        if (!streamReadU32(c, &ack)) return false;
        c->hasWrapKey = true;
        return readSecurityResult(c, nullptr);
    }
}

static bool authenticateAppleDH(AppleRfbClient *c, const char *username, const char *password) {
    if (!streamWrite(c, &kSecAppleDH, 1))
        return false;

    uint16_t gen = 0, keyLen = 0;
    if (!streamReadU16(c, &gen) || !streamReadU16(c, &keyLen))
        return false;
    if (gen < 2 || keyLen < 64 || keyLen > 1024) {
        c->lastErrno = EPROTO;
        return false;
    }

    uint8_t *primeBytes = (uint8_t *) malloc(keyLen);
    uint8_t *srvPubBytes = (uint8_t *) malloc(keyLen);
    uint8_t *privBytes = (uint8_t *) malloc(keyLen);
    uint8_t *pubBytes = (uint8_t *) malloc(keyLen);
    uint8_t *sharedBytes = (uint8_t *) malloc(keyLen);
    uint8_t *resp = (uint8_t *) malloc(128u + keyLen);

    BIGNUM *bnPrime = nullptr, *bnSrvPub = nullptr, *bnGen = nullptr;
    BIGNUM *bnPriv = nullptr, *bnPub = nullptr, *bnShared = nullptr;
    BN_CTX *bnCtx = nullptr;
    bool ok = false;

    if (!primeBytes || !srvPubBytes || !privBytes || !pubBytes || !sharedBytes || !resp)
        goto cleanup;
    if (!streamReadExact(c, primeBytes, keyLen) || !streamReadExact(c, srvPubBytes, keyLen))
        goto cleanup;
    if (RAND_bytes(privBytes, keyLen) != 1)
        goto cleanup;

    bnCtx = BN_CTX_new();
    bnPrime = BN_bin2bn(primeBytes, keyLen, nullptr);
    bnSrvPub = BN_bin2bn(srvPubBytes, keyLen, nullptr);
    bnGen = BN_new();
    bnPriv = BN_bin2bn(privBytes, keyLen, nullptr);
    bnPub = BN_new();
    bnShared = BN_new();
    if (!bnCtx || !bnPrime || !bnSrvPub || !bnGen || !bnPriv || !bnPub || !bnShared)
        goto cleanup;

    // A public value of 0, 1 or p-1 would make the shared key predictable.
    if (!BN_is_odd(bnPrime) || BN_is_zero(bnSrvPub) || BN_is_one(bnSrvPub) ||
        !BN_copy(bnShared, bnPrime) || !BN_sub_word(bnShared, 1) || BN_cmp(bnSrvPub, bnShared) >= 0) {
        c->lastErrno = EPROTO;
        goto cleanup;
    }

    BN_set_word(bnGen, gen);
    if (BN_mod_exp(bnPub, bnGen, bnPriv, bnPrime, bnCtx) != 1) goto cleanup;
    if (BN_mod_exp(bnShared, bnSrvPub, bnPriv, bnPrime, bnCtx) != 1) goto cleanup;
    if (!bnToFixedBytes(bnPub, pubBytes, keyLen)) goto cleanup;
    if (!bnToFixedBytes(bnShared, sharedBytes, keyLen)) goto cleanup;

    if (!computeDigest(EVP_md5(), sharedBytes, keyLen, c->wrapKey, 16))
        goto cleanup;

    if (!buildCredentialBlock(username, password, resp))
        goto cleanup;
    if (!aes128EcbCrypt(c->wrapKey, resp, 128, true))
        goto cleanup;
    memcpy(resp + 128, pubBytes, keyLen);

    if (!streamWrite(c, resp, 128u + keyLen))
        goto cleanup;

    c->hasWrapKey = true;
    ok = readSecurityResult(c, nullptr);

cleanup:
    free(primeBytes);
    free(srvPubBytes);
    if (privBytes) OPENSSL_cleanse(privBytes, keyLen);
    if (sharedBytes) OPENSSL_cleanse(sharedBytes, keyLen);
    free(privBytes);
    free(pubBytes);
    free(sharedBytes);
    free(resp);
    BN_free(bnPrime);
    BN_free(bnSrvPub);
    BN_free(bnGen);
    BN_clear_free(bnPriv);
    BN_free(bnPub);
    BN_clear_free(bnShared);
    BN_CTX_free(bnCtx);
    return ok;
}

/******************************************************************************
 * Session Selection (when another user is on console)
 *****************************************************************************/

static bool handleSessionSelection(AppleRfbClient *c, const char *username, const char *password) {
    uint8_t head[4];
    if (!streamReadExact(c, head, 4)) return false;
    uint16_t length = be16(head);
    if (length < 13 || length > 4096) {
        c->lastErrno = EPROTO;
        return false;
    }
    uint8_t *prompt = (uint8_t *) malloc(length + 2);
    if (!prompt) return false;
    memcpy(prompt, head, 4);
    if (!streamReadExact(c, prompt + 4, (size_t) length - 2)) {
        free(prompt);
        return false;
    }
    uint32_t flags = be32(prompt + 4);
    free(prompt);

    bool shareImmediate = (flags & 0x02u) != 0;
    bool shareAsk = (flags & 0x01u) != 0;
    bool allowOwn = (flags & 0x0cu) == 0x04u;

    uint8_t msg[2 + 200];
    size_t bodyLen = 0;

    if (shareImmediate || (!allowOwn && shareAsk)) {
        bodyLen = 72;
        memset(msg, 0, 2 + bodyLen);
        putBe16(msg, (uint16_t) bodyLen);
        putBe16(msg + 2, 1);
        msg[2 + 6] = shareImmediate ? 1u : 0u;
        size_t uLen = strlen(username);
        if (uLen > 63) uLen = 63;
        memcpy(msg + 2 + 8, username, uLen);
    } else if (allowOwn) {
        bodyLen = 200;
        memset(msg, 0, 2 + bodyLen);
        putBe16(msg, (uint16_t) bodyLen);
        putBe16(msg + 2, 2);
        msg[2 + 6] = 2;
        size_t uLen = strlen(username);
        if (uLen > 63) uLen = 63;
        memcpy(msg + 2 + 8, username, uLen);

        uint8_t creds[128];
        memset(creds, 0, sizeof(creds));
        memcpy(creds, username, uLen);
        size_t pLen = strlen(password);
        if (pLen > 63) pLen = 63;
        memcpy(creds + 64, password, pLen);
        if (!aes128EcbCrypt(c->wrapKey, creds, sizeof(creds), true))
            return false;
        memcpy(msg + 2 + 0x48, creds, sizeof(creds));
    } else {
        c->lastErrno = EACCES;
        return false;
    }

    if (!streamWrite(c, msg, 2 + bodyLen))
        return false;

    uint16_t resultLen = 0;
    if (!streamReadU16(c, &resultLen) || resultLen < 80 || resultLen > 4096) {
        c->lastErrno = EPROTO;
        return false;
    }
    uint8_t *result = (uint8_t *) malloc(resultLen);
    if (!result) return false;
    if (!streamReadExact(c, result, resultLen)) {
        free(result);
        return false;
    }
    uint8_t status = result[2];
    free(result);
    if (status != 0) {
        logErr(c, "AppleRfb: session selection rejected (status=%u)", (unsigned) status);
        c->lastErrno = EACCES;
        return false;
    }
    return true;
}

/******************************************************************************
 * Framebuffer Resize & Pixel Blitting
 *****************************************************************************/

static bool resizeFramebuffer(AppleRfbClient *c, int width, int height) {
    size_t size;
    if (!framebufferByteCount(width, height, 4, &size)) {
        c->lastErrno = EOVERFLOW;
        return false;
    }
    if (c->fb && c->fbWidth == width && c->fbHeight == height) return true;

    uint8_t *newFb = c->cbs.resizeFramebuffer(c->userCtx, width, height);
    if (!newFb) {
        c->lastErrno = ENOMEM;
        return false;
    }
    c->fb = newFb;
    __atomic_store_n(&c->fbWidth, width, __ATOMIC_RELEASE);
    __atomic_store_n(&c->fbHeight, height, __ATOMIC_RELEASE);
    c->hasReceivedPixels = false;
    c->inputFullRefreshPending = false;
    return true;
}

static void blitBgra(AppleRfbClient *c, int x, int y, int w, int h, const uint8_t *src) {
    if (!c->fb || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > c->fbWidth ? c->fbWidth : (x + w);
    int y1 = (y + h) > c->fbHeight ? c->fbHeight : (y + h);
    if (x0 >= x1 || y0 >= y1) return;

    int copyW = x1 - x0;
    c->cbs.lockFramebuffer(c->userCtx);
    for (int row = y0; row < y1; ++row) {
        const uint8_t *s = src + ((size_t) (row - y) * (size_t) w + (size_t) (x0 - x)) * 4u;
        uint8_t *d = c->fb + ((size_t) row * (size_t) c->fbWidth + (size_t) x0) * 4u;
        memcpy(d, s, (size_t) copyW * 4u);
        for (int col = 0; col < copyW; ++col) {
            d[col * 4 + 3] = 0xffu;
        }
    }
    c->cbs.unlockFramebuffer(c->userCtx);
}

static void fillBgra(AppleRfbClient *c, int x, int y, int w, int h, uint32_t bgraPixel) {
    if (!c->fb || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > c->fbWidth ? c->fbWidth : (x + w);
    int y1 = (y + h) > c->fbHeight ? c->fbHeight : (y + h);
    if (x0 >= x1 || y0 >= y1) return;

    c->cbs.lockFramebuffer(c->userCtx);
    for (int row = y0; row < y1; ++row) {
        uint32_t *d = (uint32_t *) (c->fb + ((size_t) row * (size_t) c->fbWidth + (size_t) x0) * 4u);
        for (int col = 0; col < (x1 - x0); ++col) {
            d[col] = bgraPixel;
        }
    }
    c->cbs.unlockFramebuffer(c->userCtx);
}

static void blitCopyRect(AppleRfbClient *c, int dstX, int dstY, int srcX, int srcY, int w, int h) {
    if (!c->fb || w <= 0 || h <= 0) return;
    if (srcX < 0 || srcY < 0 || dstX < 0 || dstY < 0 ||
        srcX + w > c->fbWidth || srcY + h > c->fbHeight ||
        dstX + w > c->fbWidth || dstY + h > c->fbHeight) {
        return;
    }
    c->cbs.lockFramebuffer(c->userCtx);
    size_t rowBytes = (size_t) w * 4u;
    if (dstY > srcY) {
        for (int row = h - 1; row >= 0; --row) {
            uint8_t *d = c->fb + ((size_t) (dstY + row) * (size_t) c->fbWidth + (size_t) dstX) * 4u;
            const uint8_t *s = c->fb + ((size_t) (srcY + row) * (size_t) c->fbWidth + (size_t) srcX) * 4u;
            memmove(d, s, rowBytes);
        }
    } else {
        for (int row = 0; row < h; ++row) {
            uint8_t *d = c->fb + ((size_t) (dstY + row) * (size_t) c->fbWidth + (size_t) dstX) * 4u;
            const uint8_t *s = c->fb + ((size_t) (srcY + row) * (size_t) c->fbWidth + (size_t) srcX) * 4u;
            memmove(d, s, rowBytes);
        }
    }
    c->cbs.unlockFramebuffer(c->userCtx);
}

static inline uint32_t expand5(uint32_t v) {
    return (v << 3) | (v >> 2);
}

static void blitAppleThousands(AppleRfbClient *c, int x, int y, int w, int h, const uint8_t *src) {
    if (!c->fb || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > c->fbWidth ? c->fbWidth : (x + w);
    int y1 = (y + h) > c->fbHeight ? c->fbHeight : (y + h);
    if (x0 >= x1 || y0 >= y1) return;

    c->cbs.lockFramebuffer(c->userCtx);
    for (int row = y0; row < y1; ++row) {
        uint8_t *d = c->fb + ((size_t) row * (size_t) c->fbWidth + (size_t) x0) * 4u;
        for (int col = x0; col < x1; ++col) {
            size_t si = ((size_t) (row - y) * (size_t) w + (size_t) (col - x)) * 2u;
            uint16_t v = be16(src + si);
            uint8_t r = (uint8_t) expand5((v >> 10) & 0x1fu);
            uint8_t g = (uint8_t) expand5((v >> 5) & 0x1fu);
            uint8_t b = (uint8_t) expand5(v & 0x1fu);
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = 0xffu;
            d += 4;
        }
    }
    c->cbs.unlockFramebuffer(c->userCtx);
}

static bool decodeZRLE(AppleRfbClient *c, int x, int y, int w, int h,
                       const uint8_t *data, size_t len) {
    size_t pos = 0;
    auto take = [&](size_t n) -> const uint8_t * {
        if (pos + n > len) return nullptr;
        const uint8_t *p = data + pos;
        pos += n;
        return p;
    };
    auto readCpixel = [&](uint32_t *outPx) -> bool {
        const uint8_t *p = take(3);
        if (!p) return false;
        uint8_t bgra[4] = {p[0], p[1], p[2], 0xffu};
        memcpy(outPx, bgra, 4);
        return true;
    };
    auto readRun = [&](int *outRun) -> bool {
        int run = 1;
        while (true) {
            const uint8_t *p = take(1);
            if (!p) return false;
            run += p[0];
            if (p[0] != 255) {
                *outRun = run;
                return true;
            }
            if (run > 64 * 64) return false;
        }
    };

    uint32_t palette[128];
    uint32_t tileBuf[64 * 64];

    for (int ty = y; ty < y + h; ty += 64) {
        int th = (y + h - ty) < 64 ? (y + h - ty) : 64;
        for (int tx = x; tx < x + w; tx += 64) {
            int tw = (x + w - tx) < 64 ? (x + w - tx) : 64;
            const uint8_t *hdr = take(1);
            if (!hdr) return false;

            int sub = hdr[0];
            bool rle = (sub & 0x80) != 0;
            int palSize = sub & 0x7f;

            for (int i = 0; i < palSize; ++i) {
                if (!readCpixel(&palette[i])) return false;
            }

            if (!rle && palSize == 1) {
                fillBgra(c, tx, ty, tw, th, palette[0]);
                continue;
            }

            int count = tw * th;
            if (!rle && palSize == 0) {
                for (int i = 0; i < count; ++i) {
                    if (!readCpixel(&tileBuf[i])) return false;
                }
            } else if (!rle) {
                if (palSize > 16) return false;
                int bpi = (palSize <= 2) ? 1 : (palSize <= 4 ? 2 : 4);
                int rowBytes = (tw * bpi + 7) / 8;
                const uint8_t *packed = take((size_t) rowBytes * (size_t) th);
                if (!packed) return false;
                uint8_t mask = (uint8_t) ((1 << bpi) - 1);
                for (int row = 0; row < th; ++row) {
                    const uint8_t *rp = packed + (size_t) row * (size_t) rowBytes;
                    for (int col = 0; col < tw; ++col) {
                        int bit = col * bpi;
                        uint8_t idx = (rp[bit / 8] >> (8 - bpi - (bit % 8))) & mask;
                        if (idx >= palSize) return false;
                        tileBuf[row * tw + col] = palette[idx];
                    }
                }
            } else {
                int filled = 0;
                while (filled < count) {
                    uint32_t px = 0;
                    int run = 1;
                    if (palSize == 0) {
                        if (!readCpixel(&px) || !readRun(&run)) return false;
                    } else {
                        const uint8_t *ib = take(1);
                        if (!ib) return false;
                        int idx = ib[0] & 0x7f;
                        if (idx >= palSize) return false;
                        px = palette[idx];
                        if ((ib[0] & 0x80) != 0) {
                            if (!readRun(&run)) return false;
                        }
                    }
                    if (filled + run > count) return false;
                    for (int i = 0; i < run; ++i)
                        tileBuf[filled + i] = px;
                    filled += run;
                }
            }

            blitBgra(c, tx, ty, tw, th, (const uint8_t *) tileBuf);
        }
    }
    return true;
}

/******************************************************************************
 * Apple Metadata & Control Message Handlers
 *****************************************************************************/

static bool handleRekey(AppleRfbClient *c) {
    uint8_t body[kRekeyBodySize];
    if (!streamReadExact(c, body, sizeof(body)))
        return false;

    uint32_t generation = be32(body);
    uint8_t key[16], iv[16];
    memcpy(key, body + 4, 16);
    memcpy(iv, body + 20, 16);

    if (!aes128EcbCrypt(c->wrapKey, key, 16, false) ||
        !aes128EcbCrypt(c->wrapKey, iv, 16, false)) {
        logErr(c, "AppleRfb: rekey unwrap failed");
        c->lastErrno = EPROTO;
        return false;
    }

    pthread_mutex_lock(&c->writeMutex);
    memcpy(c->wrapKey, key, 16);
    pthread_mutex_unlock(&c->writeMutex);
    memcpy(c->nextKey, key, 16);
    memcpy(c->nextIv, iv, 16);

    if (c->decCtx) EVP_CIPHER_CTX_free(c->decCtx);
    c->decCtx = EVP_CIPHER_CTX_new();
    if (!c->decCtx ||
        EVP_DecryptInit_ex(c->decCtx, EVP_aes_128_cbc(), nullptr, key, iv) != 1 ||
        EVP_CIPHER_CTX_set_padding(c->decCtx, 0) != 1) {
        c->lastErrno = EIO;
        return false;
    }
    c->recordReadActive = true;
    c->readPlainPos = 0;
    c->readPlainLen = 0;

    if (!c->pendingRekey) {
        // Mid-session rekey: rotate outgoing cipher too
        pthread_mutex_lock(&c->writeMutex);
        if (c->encCtx) EVP_CIPHER_CTX_free(c->encCtx);
        c->encCtx = EVP_CIPHER_CTX_new();
        bool encOk = c->encCtx &&
                     EVP_EncryptInit_ex(c->encCtx, EVP_aes_128_cbc(), nullptr, key, iv) == 1 &&
                     EVP_CIPHER_CTX_set_padding(c->encCtx, 0) == 1;
        pthread_mutex_unlock(&c->writeMutex);
        if (!encOk) {
            c->lastErrno = EIO;
            return false;
        }
    }

    c->pendingRekey = false;
    logInfo(c, "AppleRfb: rekey processed (generation=%u)", generation);
    return true;
}

static bool handleCursorImage(AppleRfbClient *c, uint16_t xHot, uint16_t yHot,
                              uint16_t width, uint16_t height) {
    uint32_t id = 0;
    if (!streamReadU32(c, &id)) return false;
    if (id <= 999u) {
        // System cursor reference — keep current cursor
        return true;
    }

    uint32_t compLen = 0;
    if (!streamReadU32(c, &compLen)) return false;
    if (compLen > 1024u * 1024u) {
        c->lastErrno = EPROTO;
        return false;
    }

    if (compLen == 0) {
        // SELECT cached cursor
        for (size_t i = 0; i < c->cursorCount; ++i) {
            if (c->cursors[i].id == id) {
                if (c->cbs.onCursorShape && c->cursors[i].bgra) {
                    c->cbs.onCursorShape(c->userCtx, c->cursors[i].bgra,
                                         c->cursors[i].width, c->cursors[i].height,
                                         c->cursors[i].xHot, c->cursors[i].yHot);
                }
                return true;
            }
        }
        return true;
    }

    uint8_t *comp = (uint8_t *) malloc(compLen);
    if (!comp) return false;
    if (!streamReadExact(c, comp, compLen)) {
        free(comp);
        return false;
    }

    if (width == 0 || height == 0 || width > kMaxCursorSize || height > kMaxCursorSize) {
        free(comp);
        return true;
    }

    size_t pixelCount = (size_t) width * (size_t) height;
    uint8_t *raw = nullptr;
    size_t rawLen = 0;
    bool ok = inflateOneShot(comp, compLen, pixelCount * 5u, &raw, &rawLen);
    free(comp);
    if (!ok || rawLen != pixelCount * 5u) {
        free(raw);
        return true; // Non-fatal
    }

    uint8_t *bgra = (uint8_t *) malloc(pixelCount * 4u);
    if (!bgra) {
        free(raw);
        return false;
    }
    const uint8_t *alpha = raw + pixelCount * 4u;
    for (size_t i = 0; i < pixelCount; ++i) {
        bgra[i * 4 + 0] = raw[i * 4 + 0];
        bgra[i * 4 + 1] = raw[i * 4 + 1];
        bgra[i * 4 + 2] = raw[i * 4 + 2];
        bgra[i * 4 + 3] = alpha[i];
    }
    free(raw);

    // Store in cache
    CachedCursor *slot = nullptr;
    for (size_t i = 0; i < c->cursorCount; ++i) {
        if (c->cursors[i].id == id) {
            slot = &c->cursors[i];
            free(slot->bgra);
            break;
        }
    }
    if (!slot) {
        if (c->cursorCount < kMaxCursorCache) {
            slot = &c->cursors[c->cursorCount++];
        } else {
            slot = &c->cursors[c->cursorEvictIdx];
            c->cursorEvictIdx = (c->cursorEvictIdx + 1) % kMaxCursorCache;
            free(slot->bgra);
        }
    }
    slot->id = id;
    slot->width = width;
    slot->height = height;
    slot->xHot = xHot;
    slot->yHot = yHot;
    slot->bgra = bgra;

    if (c->cbs.onCursorShape) {
        c->cbs.onCursorShape(c->userCtx, bgra, width, height, xHot, yHot);
    }
    return true;
}

static bool handleDisplayLayout(AppleRfbClient *c) {
    uint16_t len = 0;
    if (!streamReadU16(c, &len)) return false;

    uint8_t *payload = (uint8_t *) malloc(len > 0 ? len : 1);
    if (!payload) return false;
    if (len > 0 && !streamReadExact(c, payload, len)) {
        free(payload);
        return false;
    }

    AppleDisplayLayout layout;
    bool parsed = appleRfbParseDisplayLayout(payload, len, &layout);
    free(payload);
    if (!parsed) {
        logErr(c, "AppleRfb: failed to parse DisplayLayout (len=%u)", (unsigned) len);
        return true; // Tolerate minor skew without tearing down session
    }

    c->layout = layout;
    c->hasLayout = true;
    double selectedScale = 1.0;
    if (layout.count > 0) {
        selectedScale = (layout.displays[0].scale > 0.0) ? layout.displays[0].scale : 1.0;
        if (layout.currentDisplayId != APPLE_ALL_DISPLAYS_WIRE) {
            for (uint16_t i = 0; i < layout.count; ++i) {
                if (layout.displays[i].id == layout.currentDisplayId && layout.displays[i].scale > 0.0) {
                    selectedScale = layout.displays[i].scale;
                    break;
                }
            }
        }
    }
    pthread_mutex_lock(&c->writeMutex);
    c->pointerScale = selectedScale;
    pthread_mutex_unlock(&c->writeMutex);

    logInfo(c, "AppleRfb: display layout fb=%ux%u current=0x%x displays=%u",
            layout.fbWidth, layout.fbHeight, layout.currentDisplayId, layout.count);

    if (layout.fbWidth > 0 && layout.fbHeight > 0) {
        if (!resizeFramebuffer(c, layout.fbWidth, layout.fbHeight))
            return false;
    }

    bool supportsSel = serverAccepts(c, 0x0d);
    if (c->cbs.onDisplaysChanged) {
        c->cbs.onDisplaysChanged(c->userCtx, &layout, supportsSel);
    }

    // Auto-select a single display (1 of up to 6) whenever the host exposes multiple displays
    // so the viewer never stays on a combined multi-monitor framebuffer.
    if (c->running && !c->initialDisplayApplied && supportsSel && layout.count > 1) {
        int selectableCount = (layout.count > APPLE_MAX_SELECTABLE_DISPLAYS)
                              ? APPLE_MAX_SELECTABLE_DISPLAYS
                              : (int) layout.count;
        int requestedIndex = c->config.initialDisplayIndex;
        if (requestedIndex < 1 || requestedIndex > selectableCount) {
            requestedIndex = 1;
        }
        c->initialDisplayApplied = true;
        uint32_t targetId = layout.displays[requestedIndex - 1].id;
        if (targetId != 0 && layout.currentDisplayId != targetId) {
            logInfo(c, "AppleRfb: auto-selecting single display #%d (id=0x%x)",
                    requestedIndex, targetId);
            if (!appleRfbSelectDisplay(c, targetId))
                return false;
        }
    }

    if (c->running) {
        if (serverAccepts(c, 0x15)) {
            uint8_t ap[8];
            buildAutoPasteboardMessage(ap, true);
            if (!streamWrite(c, ap, sizeof(ap))) return false;
        }
        // Re-arm server updates on every 0x451 (required by screensharingd for cursor/updates)
        if (!sendUpdateArm(c, true))
            return false;
    }

    return true;
}

static bool handleKeyboardSource(AppleRfbClient *c) {
    uint16_t payloadLen = 0;
    if (!streamReadU16(c, &payloadLen)) return false;

    uint8_t *payload = (uint8_t *) malloc(payloadLen > 0 ? payloadLen : 1);
    if (!payload) return false;
    if (payloadLen > 0 && !streamReadExact(c, payload, payloadLen)) {
        free(payload);
        return false;
    }

    char sourceId[256];
    bool secureInput = false;
    bool parsed = appleRfbParseKeyboardSource(payload, payloadLen,
                                              sourceId, sizeof(sourceId), &secureInput);
    free(payload);
    if (!parsed) {
        logErr(c, "AppleRfb: failed to parse KeyboardSource (len=%u)", (unsigned) payloadLen);
        return true;
    }

    snprintf(c->keyboardSourceId, sizeof(c->keyboardSourceId), "%s", sourceId);
    c->keyboardSecureInput = secureInput;
    c->hasKeyboardSource = true;

    logInfo(c, "AppleRfb: keyboard source='%s' secure=%d supportsSel=%d",
            sourceId, secureInput ? 1 : 0, serverAccepts(c, 0x1a) ? 1 : 0);

    if (c->cbs.onKeyboardSourceChanged) {
        c->cbs.onKeyboardSourceChanged(c->userCtx, sourceId, secureInput, serverAccepts(c, 0x1a));
    }
    return true;
}

static bool handleDisplayInfo(AppleRfbClient *c, uint16_t width, uint16_t height) {
    uint8_t header[10];
    if (!streamReadExact(c, header, sizeof(header))) return false;
    uint16_t count = be16(header + 8);
    if (!streamDiscard(c, (size_t) count * 0x1cu)) return false;
    if (width > 0 && height > 0) {
        if (!resizeFramebuffer(c, width, height)) return false;
        if (c->running && !sendUpdateArm(c, true)) return false;
    }
    return true;
}

static bool handleFramebufferUpdate(AppleRfbClient *c) {
    uint8_t pad = 0;
    uint16_t rectCount = 0;
    if (!streamReadU8(c, &pad) || !streamReadU16(c, &rectCount))
        return false;

    bool hadPixels = false;

    for (uint16_t i = 0; i < rectCount; ++i) {
        uint8_t rh[12];
        if (!streamReadExact(c, rh, sizeof(rh))) return false;

        uint16_t rx = be16(rh + 0);
        uint16_t ry = be16(rh + 2);
        uint16_t rw = be16(rh + 4);
        uint16_t rhgt = be16(rh + 6);
        int32_t enc = (int32_t) be32(rh + 8);

        if (enc == kEncLastRect)
            break;

        size_t pixelBytes = 0;
        if (enc == kEncRaw || enc == kEncZlib || enc == kEncAppleThousands ||
            enc == kEncZRLE || enc == kEncCopyRect) {
            if (rx + rw > c->fbWidth || ry + rhgt > c->fbHeight ||
                !framebufferByteCount(rw, rhgt, enc == kEncAppleThousands ? 2 : 4, &pixelBytes)) {
                c->lastErrno = EPROTO;
                return false;
            }
        }

        auto notePixels = [&] {
            hadPixels = true;
            if (c->cbs.onFramebufferRect)
                c->cbs.onFramebufferRect(c->userCtx, ry, rhgt);
        };
        switch (enc) {
            case kEncRaw: {
                uint8_t *buf = ensureScratch(c, pixelBytes);
                if (!buf) return false;
                if (!streamReadExact(c, buf, pixelBytes)) return false;
                blitBgra(c, rx, ry, rw, rhgt, buf);
                notePixels();
                break;
            }
            case kEncCopyRect: {
                uint8_t pos[4];
                if (!streamReadExact(c, pos, 4)) return false;
                blitCopyRect(c, rx, ry, be16(pos), be16(pos + 2), rw, rhgt);
                notePixels();
                break;
            }
            case kEncZlib: {
                uint32_t compLen = 0;
                if (!streamReadU32(c, &compLen) || compLen > kMaxCompressedRect) {
                    c->lastErrno = EPROTO;
                    return false;
                }
                uint8_t *comp = ensureScratch(c, compLen > 0 ? compLen : 1);
                if (!comp) return false;
                if (compLen > 0 && !streamReadExact(c, comp, compLen)) return false;

                uint8_t *raw = nullptr;
                size_t rawLen = 0;
                if (!zlibDecoderInflate(&c->zlibDec, comp, compLen, pixelBytes, true, &raw, &rawLen)) {
                    logErr(c, "AppleRfb: Zlib inflate failed");
                    c->lastErrno = EPROTO;
                    return false;
                }
                blitBgra(c, rx, ry, rw, rhgt, raw);
                zlibDecoderTrim(&c->zlibDec);
                notePixels();
                break;
            }
            case kEncAppleThousands: {
                uint32_t compLen = 0;
                if (!streamReadU32(c, &compLen) || compLen > kMaxCompressedRect) {
                    c->lastErrno = EPROTO;
                    return false;
                }
                uint8_t *comp = ensureScratch(c, compLen > 0 ? compLen : 1);
                if (!comp) return false;
                if (compLen > 0 && !streamReadExact(c, comp, compLen)) return false;

                uint8_t *raw = nullptr;
                size_t rawLen = 0;
                if (!zlibDecoderInflate(&c->thousandsDec, comp, compLen, pixelBytes, true, &raw, &rawLen)) {
                    logErr(c, "AppleRfb: AppleThousands inflate failed");
                    c->lastErrno = EPROTO;
                    return false;
                }
                blitAppleThousands(c, rx, ry, rw, rhgt, raw);
                zlibDecoderTrim(&c->thousandsDec);
                notePixels();
                break;
            }
            case kEncZRLE: {
                uint32_t compLen = 0;
                if (!streamReadU32(c, &compLen) || compLen > kMaxCompressedRect) {
                    c->lastErrno = EPROTO;
                    return false;
                }
                uint8_t *comp = ensureScratch(c, compLen > 0 ? compLen : 1);
                if (!comp) return false;
                if (compLen > 0 && !streamReadExact(c, comp, compLen)) return false;

                uint8_t *raw = nullptr;
                size_t rawLen = 0;
                size_t tiles = ((rw + 63u) / 64u) * ((rhgt + 63u) / 64u);
                size_t limit = pixelBytes + tiles * (1u + 127u * 3u);
                if (limit > kMaxFramebufferBytes) limit = kMaxFramebufferBytes;
                if (!zlibDecoderInflate(&c->zrleDec, comp, compLen, limit, false, &raw, &rawLen)) {
                    logErr(c, "AppleRfb: ZRLE inflate failed");
                    c->lastErrno = EPROTO;
                    return false;
                }
                bool zrleOk = decodeZRLE(c, rx, ry, rw, rhgt, raw, rawLen);
                zlibDecoderTrim(&c->zrleDec);
                if (!zrleOk) {
                    logErr(c, "AppleRfb: ZRLE decode failed");
                    c->lastErrno = EPROTO;
                    return false;
                }
                notePixels();
                break;
            }
            case kEncDesktopSize: {
                if (!resizeFramebuffer(c, rw, rhgt)) return false;
                if (c->running && !sendUpdateArm(c, true)) return false;
                break;
            }
            case kEncExtendedDesktopSize: {
                uint8_t screenCount = 0;
                if (!streamReadU8(c, &screenCount)) return false;
                if (!streamDiscard(c, 3u + (size_t) screenCount * 16u)) return false;
                if (ry == 0) {
                    if (!resizeFramebuffer(c, rw, rhgt)) return false;
                    if (c->running && !sendUpdateArm(c, true)) return false;
                }
                break;
            }
            case kEncDesktopName: {
                uint32_t nameLen = 0;
                if (!streamReadU32(c, &nameLen) || nameLen > 65536u) return false;
                char *name = (char *) malloc(nameLen + 1);
                if (!name) return false;
                if (nameLen > 0 && !streamReadExact(c, (uint8_t *) name, nameLen)) {
                    free(name);
                    return false;
                }
                name[nameLen] = '\0';
                free(c->desktopName);
                c->desktopName = name;
                break;
            }
            case kEncCursorPosition:
            case kEncPointerPos: {
                if (c->cbs.onCursorPos) {
                    c->cbs.onCursorPos(c->userCtx, (int) rx, (int) ry);
                }
                break;
            }
            case kEncCursorImage: {
                if (!handleCursorImage(c, rx, ry, rw, rhgt)) return false;
                break;
            }
            case kEncRekey: {
                if (!handleRekey(c)) return false;
                break;
            }
            case kEncDisplayLayout: {
                if (!handleDisplayLayout(c)) return false;
                break;
            }
            case kEncDisplayInfo: {
                if (!handleDisplayInfo(c, rw, rhgt)) return false;
                break;
            }
            case kEncKeyboardSource: {
                if (!handleKeyboardSource(c)) return false;
                break;
            }
            case kEncUserInfo:
            case kEncVendorKeysyms:
            case kEncDeviceInfo:
            case kEncMediaStream: {
                uint16_t payloadLen = 0;
                if (!streamReadU16(c, &payloadLen)) return false;
                if (!streamDiscard(c, payloadLen)) return false;
                break;
            }
            default:
                logErr(c, "AppleRfb: unsupported encoding %d", enc);
                c->lastErrno = EPROTO;
                return false;
        }
    }

    if (hadPixels) {
        uint64_t now = nowMonotonicMs();
        if (c->lastInputRecoveryReqMs != 0 && now >= c->lastInputRecoveryReqMs &&
            now <= c->lastInputRecoveryReqMs + 1500ULL) {
            c->lastInputRecoveryReqMs = 0;
            if (serverAccepts(c, 0x09) && !c->forceIncrementalPull) {
                c->inputStallRecoveries++;
                if (c->inputStallRecoveries >= 2) {
                    c->forceIncrementalPull = true;
                    logInfo(c, "AppleRfb: auto-push stalled after input; enabling incremental pull mode");
                }
            }
        }
        c->hasReceivedPixels = true;
        c->inputFullRefreshPending = false;
        __atomic_store_n(&c->hasInputSinceLastPixel, 0, __ATOMIC_RELEASE);
        c->lastPixelUpdateMs = now;
        if (c->cbs.onFramebufferUpdated) {
            c->cbs.onFramebufferUpdated(c->userCtx);
        }
    }

    // Request next incremental update when server lacks AutoFrameBufferUpdate (0x09)
    // or when watchdog promoted this session to pull mode after an auto-push stall.
    if (c->running && !__atomic_load_n(&c->paused, __ATOMIC_ACQUIRE) &&
        c->fbWidth > 0 && c->fbHeight > 0 &&
        (!serverAccepts(c, 0x09) || (c->forceIncrementalPull && hadPixels))) {
        uint8_t req[10];
        buildFbUpdateRequest(req, true, c->fbWidth, c->fbHeight);
        if (!streamWrite(c, req, sizeof(req))) return false;
    }

    return true;
}

static bool deflateOneShot(const uint8_t *in, size_t inLen, uint8_t **outBuf, size_t *outLen) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit(&zs, Z_BEST_COMPRESSION) != Z_OK)
        return false;

    uLong bound = deflateBound(&zs, (uLong) inLen);
    if (bound < 64) bound = 64;
    uint8_t *buf = (uint8_t *) malloc(bound);
    if (!buf) {
        deflateEnd(&zs);
        return false;
    }

    zs.next_in = (Bytef *) in;
    zs.avail_in = (uInt) inLen;
    zs.next_out = buf;
    zs.avail_out = (uInt) bound;

    int rc = deflate(&zs, Z_FINISH);
    size_t produced = (size_t) bound - (size_t) zs.avail_out;
    deflateEnd(&zs);

    if (rc != Z_STREAM_END) {
        free(buf);
        return false;
    }

    *outBuf = buf;
    *outLen = produced;
    return true;
}

static bool buildUtf8PasteboardArchive(const char *utf8, size_t len, bool transient,
                                       uint8_t **outBuf, size_t *outLen) {
    static const char kUtf8Type[] = "public.utf8-plain-text";
    static const char kTransientType[] = "org.nspasteboard.TransientType";
    const size_t utf8TypeLen = sizeof(kUtf8Type) - 1u;
    const size_t transTypeLen = sizeof(kTransientType) - 1u;

    size_t total = 4u + (16u + utf8TypeLen + len);
    if (transient) {
        total += 16u + transTypeLen + 1u;
    }

    uint8_t *buf = (uint8_t *) malloc(total);
    if (!buf) return false;

    size_t o = 0;
    putBe32(buf + o, transient ? 2u : 1u); o += 4;

    putBe32(buf + o, (uint32_t) utf8TypeLen); o += 4;
    memcpy(buf + o, kUtf8Type, utf8TypeLen); o += utf8TypeLen;
    putBe32(buf + o, 0u); o += 4; // reserved
    putBe32(buf + o, 0u); o += 4; // tagCount
    putBe32(buf + o, (uint32_t) len); o += 4;
    if (len > 0) {
        memcpy(buf + o, utf8, len);
        o += len;
    }

    if (transient) {
        putBe32(buf + o, (uint32_t) transTypeLen); o += 4;
        memcpy(buf + o, kTransientType, transTypeLen); o += transTypeLen;
        putBe32(buf + o, 0u); o += 4;
        putBe32(buf + o, 0u); o += 4;
        putBe32(buf + o, 1u); o += 4;
        buf[o++] = 0x01u;
    }

    *outBuf = buf;
    *outLen = o;
    return true;
}

static void cacheSavedClipboardArchive(AppleRfbClient *c, const uint8_t *archive, size_t len) {
    if (!c || !c->clipMutexInit || !archive || len == 0 || len > kMaxSavedClipboardBytes)
        return;
    uint8_t *copy = (uint8_t *) malloc(len);
    if (!copy) return;
    memcpy(copy, archive, len);
    pthread_mutex_lock(&c->clipMutex);
    free(c->savedClipboardArchive);
    c->savedClipboardArchive = copy;
    c->savedClipboardArchiveLen = len;
    pthread_mutex_unlock(&c->clipMutex);
}

static bool sendApplePasteboardArchive(AppleRfbClient *c, const uint8_t *archive, size_t archiveLen) {
    uint8_t *comp = nullptr;
    size_t compLen = 0;
    if (!deflateOneShot(archive, archiveLen, &comp, &compLen))
        return false;

    size_t total = 16u + compLen;
    uint8_t *msg = (uint8_t *) malloc(total);
    if (!msg) {
        free(comp);
        return false;
    }
    memset(msg, 0, 16);
    msg[0] = 0x1f;
    putBe32(msg + 4, 0u); // echo = 0
    putBe32(msg + 8, (uint32_t) archiveLen);
    putBe32(msg + 12, (uint32_t) compLen);
    memcpy(msg + 16, comp, compLen);
    free(comp);

    if (c->clipMutexInit) {
        pthread_mutex_lock(&c->clipMutex);
        c->ownPasteboardEchoUntilMs = nowMonotonicMs() + kPasteboardEchoSupprMs;
        pthread_mutex_unlock(&c->clipMutex);
    }

    bool ok = streamWrite(c, msg, total);
    free(msg);
    return ok;
}

static bool handleAppleClipboardSend(AppleRfbClient *c) {
    uint8_t hdr[15];
    if (!streamReadExact(c, hdr, sizeof(hdr))) return false;
    uint32_t uncompSize = be32(hdr + 7);
    uint32_t compSize = be32(hdr + 11);

    if (compSize > 16u * 1024u * 1024u) {
        c->lastErrno = EPROTO;
        return false;
    }

    if (compSize == 0) return true;
    uint8_t *comp = (uint8_t *) malloc(compSize);
    if (!comp) return false;
    if (!streamReadExact(c, comp, compSize)) {
        free(comp);
        return false;
    }

    if (uncompSize == 0 || uncompSize > 4u * 1024u * 1024u) {
        free(comp);
        return true;
    }

    uint8_t *archive = nullptr;
    size_t archLen = 0;
    bool ok = inflateOneShot(comp, compSize, uncompSize, &archive, &archLen);
    free(comp);
    if (!ok || !archive) {
        free(archive);
        return true;
    }

    // Parse Apple pasteboard archive: inspect transient marker and extract UTF-8 plain text
    size_t pos = 0;
    auto readU32 = [&](uint32_t *out) -> bool {
        if (archLen - pos < 4) return false;
        *out = be32(archive + pos);
        pos += 4;
        return true;
    };

    const char *utf8Ptr = nullptr;
    uint32_t utf8Len = 0;
    bool isTransient = false;

    while (pos < archLen) {
        uint32_t flavorCount = 0;
        if (!readU32(&flavorCount) || flavorCount > 1024u) break;
        for (uint32_t f = 0; f < flavorCount; ++f) {
            uint32_t typeLen = 0;
            if (!readU32(&typeLen) || typeLen > 4096u || typeLen > archLen - pos) {
                pos = archLen;
                break;
            }
            const char *typeStr = (const char *) (archive + pos);
            pos += typeLen;

            uint32_t reserved = 0, tagCount = 0;
            if (!readU32(&reserved) || !readU32(&tagCount) || tagCount > 256u) {
                pos = archLen;
                break;
            }
            for (uint32_t t = 0; t < tagCount; ++t) {
                uint32_t kLen = 0, vLen = 0;
                if (!readU32(&kLen) || kLen > archLen - pos) { pos = archLen; break; }
                pos += kLen;
                if (!readU32(&vLen) || vLen > archLen - pos) { pos = archLen; break; }
                pos += vLen;
            }
            uint32_t dataLen = 0;
            if (!readU32(&dataLen) || dataLen > archLen - pos) {
                pos = archLen;
                break;
            }
            if (!utf8Ptr && dataLen > 0 && typeLen == 22 &&
                memcmp(typeStr, "public.utf8-plain-text", 22) == 0) {
                utf8Ptr = (const char *) (archive + pos);
                utf8Len = dataLen;
            } else if (typeLen == 30 &&
                       memcmp(typeStr, "org.nspasteboard.TransientType", 30) == 0) {
                isTransient = true;
            }
            pos += dataLen;
        }
    }

    if (!isTransient) {
        cacheSavedClipboardArchive(c, archive, archLen);
        if (utf8Ptr && utf8Len > 0 && c->cbs.onCutText && clipboardIsAuthenticated(c)) {
            c->cbs.onCutText(c->userCtx, utf8Ptr, (int) utf8Len, true);
        }
    }

    free(archive);
    return true;
}

static bool handleMiscStatus(AppleRfbClient *c) {
    uint8_t pad = 0;
    uint16_t bodyLen = 0;
    if (!streamReadU8(c, &pad) || !streamReadU16(c, &bodyLen)) return false;

    uint8_t *body = (uint8_t *) malloc(bodyLen > 0 ? bodyLen : 1);
    if (!body) return false;
    if (bodyLen > 0 && !streamReadExact(c, body, bodyLen)) {
        free(body);
        return false;
    }

    if (bodyLen >= 4) {
        uint16_t cmd = be16(body + 2);
        if (cmd == 2 && c->running) {
            // StatusPasteboardChanged -> request clipboard via 0x0b unless suppressing our own echo
            bool suppress = false;
            if (c->clipMutexInit) {
                pthread_mutex_lock(&c->clipMutex);
                suppress = (nowMonotonicMs() < c->ownPasteboardEchoUntilMs);
                pthread_mutex_unlock(&c->clipMutex);
            }
            if (!suppress) {
                uint8_t fetch[8] = {0x0b, 0, 0, 0, 0, 0, 0, 0};
                streamWrite(c, fetch, sizeof(fetch));
            }
        } else if (cmd == 6 && c->running && !c->paused) {
            // StatusDisplaysWake -> re-arm updates
            sendUpdateArm(c, true);
        }
    }

    free(body);
    return true;
}

static void maybeKickStalledFramebuffer(AppleRfbClient *c);

bool appleRfbHandleServerMessage(AppleRfbClient *c) {
    if (!c) return false;

    uint8_t msgType = 0;
    if (!streamReadU8(c, &msgType))
        return false;

    bool ok = false;
    switch (msgType) {
        case 0x00: // FramebufferUpdate
            ok = handleFramebufferUpdate(c);
            break;

        case 0x01: { // SetColourMapEntries
            uint8_t hdr[5];
            if (!streamReadExact(c, hdr, sizeof(hdr))) return false;
            uint16_t count = be16(hdr + 3);
            ok = streamDiscard(c, (size_t) count * 6u);
            break;
        }

        case 0x02: // Bell
            if (c->cbs.onBell) c->cbs.onBell(c->userCtx);
            ok = true;
            break;

        case 0x03: { // ServerCutText
            uint8_t hdr[7];
            if (!streamReadExact(c, hdr, sizeof(hdr))) return false;
            uint32_t len = be32(hdr + 3);
            if (len > 4u * 1024u * 1024u) {
                c->lastErrno = EPROTO;
                return false;
            }
            char *text = (char *) malloc(len > 0 ? len : 1);
            if (!text) return false;
            if (len > 0 && !streamReadExact(c, (uint8_t *) text, len)) {
                free(text);
                return false;
            }
            if (c->cbs.onCutText && len > 0 && clipboardIsAuthenticated(c)) {
                c->cbs.onCutText(c->userCtx, text, (int) len, false);
            }
            free(text);
            ok = true;
            break;
        }

        case 0x14: // Apple MiscStatus
            ok = handleMiscStatus(c);
            break;

        case 0x15: { // Apple Accessibility (14-byte LE header + payload)
            uint8_t hdr[14];
            if (!streamReadExact(c, hdr, sizeof(hdr))) return false;
            uint16_t len = le16(hdr + 12);
            ok = streamDiscard(c, len);
            break;
        }

        case 0x1e: { // Apple DropDestination
            uint8_t hdr[7];
            if (!streamReadExact(c, hdr, sizeof(hdr))) return false;
            uint32_t pathLen = be32(hdr + 3);
            if (pathLen > 65536u) return false;
            ok = streamDiscard(c, pathLen);
            break;
        }

        case 0x1f: // Apple ClipboardSend
            ok = handleAppleClipboardSend(c);
            break;

        case 0x20: { // Apple Drag
            uint8_t hdr[15];
            if (!streamReadExact(c, hdr, sizeof(hdr))) return false;
            uint32_t compSize = be32(hdr + 11);
            if (compSize > 16u * 1024u * 1024u) return false;
            ok = streamDiscard(c, compSize);
            break;
        }

        default:
            logErr(c, "AppleRfb: unknown server message type 0x%02x", (unsigned) msgType);
            c->lastErrno = EPROTO;
            return false;
    }

    if (ok) {
        maybeKickStalledFramebuffer(c);
    }
    return ok;
}

/******************************************************************************
 * Connection Lifecycle
 *****************************************************************************/

AppleRfbConnectResult appleRfbConnect(int sock, const AppleRfbConfig *config,
                                      const AppleRfbCallbacks *callbacks, void *userCtx,
                                      AppleRfbClient **outClient, int *outErrno) {
    if (!config || !callbacks || !outClient)
        return APPLE_RFB_ERROR;

    AppleRfbClient *c = (AppleRfbClient *) calloc(1, sizeof(AppleRfbClient));
    if (!c) {
        if (outErrno) *outErrno = ENOMEM;
        return APPLE_RFB_ERROR;
    }

    c->sock = sock;
    c->config = *config;
    c->cbs = *callbacks;
    c->userCtx = userCtx;
    c->pointerScale = 1.0;
    c->viewOnly = config->viewOnly;
    c->virtualLogicalWidth = config->virtualWidth > 0 ? config->virtualWidth : 1920;
    c->virtualLogicalHeight = config->virtualHeight > 0 ? config->virtualHeight : 1080;

    if (pthread_mutex_init(&c->writeMutex, nullptr) != 0) {
        free(c);
        return APPLE_RFB_ERROR;
    }
    c->writeMutexInit = true;

    if (pthread_mutex_init(&c->clipMutex, nullptr) != 0) {
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }
    c->clipMutexInit = true;

    c->readPlainBuf = (uint8_t *) malloc(kRecordMaxSize);
    c->writeWireBuf = (uint8_t *) malloc(2u + kRecordMaxSize);
    if (!c->readPlainBuf || !c->writeWireBuf) {
        appleRfbDestroy(c);
        if (outErrno) *outErrno = ENOMEM;
        return APPLE_RFB_ERROR;
    }

    // 1. Read 12-byte server version banner
    uint8_t banner[12];
    if (!streamReadExact(c, banner, sizeof(banner))) {
        if (outErrno) *outErrno = c->lastErrno;
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }
    if (memcmp(banner, "RFB 003.889\n", 12) != 0 && memcmp(banner, "RFB 003.888\n", 12) != 0) {
        appleRfbDestroy(c);
        return APPLE_RFB_FALLBACK;
    }

    // Reply RFB 003.889\n to activate Apple's native Screen Sharing dialect
    if (!streamWrite(c, (const uint8_t *) "RFB 003.889\n", 12)) {
        if (outErrno) *outErrno = c->lastErrno;
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }

    // 2. Read offered security types
    uint8_t secCount = 0;
    if (!streamReadU8(c, &secCount)) {
        if (outErrno) *outErrno = c->lastErrno;
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }
    if (secCount == 0) {
        uint32_t reasonLen = 0;
        if (streamReadU32(c, &reasonLen) && reasonLen <= 4096u) {
            streamDiscard(c, reasonLen);
        }
        if (outErrno) *outErrno = EACCES;
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }

    uint8_t offered[256];
    if (!streamReadExact(c, offered, secCount)) {
        if (outErrno) *outErrno = c->lastErrno;
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }

    bool has36 = false, has33 = false, has30 = false, has2 = false, has1 = false;
    for (uint8_t i = 0; i < secCount; ++i) {
        if (offered[i] == kSecAppleSRP) has36 = true;
        if (offered[i] == kSecAppleRSA) has33 = true;
        if (offered[i] == kSecAppleDH) has30 = true;
        if (offered[i] == kSecVncAuth) has2 = true;
        if (offered[i] == kSecNone) has1 = true;
    }

    char username[128] = {0};
    char password[128] = {0};
    WipeOnExit passwordWipe{password, sizeof(password)};
    bool authOk = false;

    if (!has36 && !has33 && !has30) {
        if (has1 && !has2 && config->securityType == 0) {
            // Single-socket NoAuth support (e.g. lightweight RFB 3.889 mock servers)
            uint8_t noneType = kSecNone;
            if (!streamWrite(c, &noneType, 1) || !readSecurityResult(c, nullptr)) {
                if (outErrno) *outErrno = c->lastErrno ? c->lastErrno : EACCES;
                appleRfbDestroy(c);
                return APPLE_RFB_ERROR;
            }
            authOk = true;
        } else {
            logInfo(c, "AppleRfb: no Apple account auth offered (has2=%d, has1=%d); falling back",
                    has2 ? 1 : 0, has1 ? 1 : 0);
            appleRfbDestroy(c);
            return APPLE_RFB_FALLBACK;
        }
    } else {
        // 3. Get macOS credentials
        if (!has36 && !config->authenticatedTunnel) {
            if (outErrno) *outErrno = EPROTONOSUPPORT;
            appleRfbDestroy(c);
            return APPLE_RFB_ERROR;
        }
        if (!c->cbs.getCredential ||
            !c->cbs.getCredential(c->userCtx, username, sizeof(username), password, sizeof(password))) {
            if (outErrno) *outErrno = EACCES;
            appleRfbDestroy(c);
            return APPLE_RFB_ERROR;
        }

        // If user left username blank on Auto security and server offers VncAuth, fall back
        if (username[0] == '\0' && config->securityType == 0 && has2) {
            logInfo(c, "AppleRfb: empty username with VncAuth available; falling back to VncAuth");
            appleRfbDestroy(c);
            return APPLE_RFB_FALLBACK;
        }

        // 4. Authenticate (prefer SRP 36 -> RSA-SRP 33 -> DH 30)
        if (has36) {
            logInfo(c, "AppleRfb: authenticating with Apple SRP (36)");
            authOk = authenticateAppleSRP(c, username, password);
        } else if (has33) {
            logInfo(c, "AppleRfb: authenticating with Apple RSA (33)");
            authOk = authenticateAppleRSA(c, username, password, has36);
        } else if (has30) {
            logInfo(c, "AppleRfb: authenticating with Apple DH (30)");
            authOk = authenticateAppleDH(c, username, password);
        }

        if (!authOk || !c->hasWrapKey) {
            if (outErrno) *outErrno = c->lastErrno ? c->lastErrno : EACCES;
            appleRfbDestroy(c);
            return APPLE_RFB_ERROR;
        }
    }

    // 5. ClientInit (0xC1 = Apple extension + session selection + shared; 0x01 for NoAuth)
    uint8_t clientInit = c->hasWrapKey ? 0xc1 : 0x01;
    if (!streamWrite(c, &clientInit, 1)) {
        if (outErrno) *outErrno = c->lastErrno;
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }

    // 6. ServerInit (with 22-byte Apple extended capability header)
    uint8_t siHdr[24];
    if (!streamReadExact(c, siHdr, sizeof(siHdr))) {
        if (outErrno) *outErrno = c->lastErrno;
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }
    int initW = be16(siHdr + 0);
    int initH = be16(siHdr + 2);
    uint32_t nameLen = be32(siHdr + 20);
    if (nameLen > 1024u * 1024u) {
        if (outErrno) *outErrno = EPROTO;
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }

    uint8_t *nameBuf = (uint8_t *) malloc(nameLen + 1);
    if (!nameBuf) {
        if (outErrno) *outErrno = ENOMEM;
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }
    if (nameLen > 0 && !streamReadExact(c, nameBuf, nameLen)) {
        free(nameBuf);
        if (outErrno) *outErrno = c->lastErrno;
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }
    nameBuf[nameLen] = 0;

    if (nameLen >= 22 && nameBuf[0] == 0 && nameBuf[1] == 0) {
        c->extended = true;
        c->serverFlags = be32(nameBuf + 2);
        memcpy(c->serverCommands, nameBuf + 6, 16);
        size_t realNameLen = nameLen - 22;
        c->desktopName = (char *) malloc(realNameLen + 1);
        if (c->desktopName) {
            memcpy(c->desktopName, nameBuf + 22, realNameLen);
            c->desktopName[realNameLen] = '\0';
        }
    } else {
        c->desktopName = (char *) malloc(nameLen + 1);
        if (c->desktopName) {
            memcpy(c->desktopName, nameBuf, nameLen + 1);
        }
    }
    free(nameBuf);

    logInfo(c, "AppleRfb: ServerInit '%s' %dx%d flags=0x%x maxVirtual=%d dispSel=%d virtDisp=%d",
            c->desktopName ? c->desktopName : "", initW, initH, c->serverFlags,
            appleRfbMaxVirtualDisplays(c),
            serverAccepts(c, 0x0d) ? 1 : 0,
            serverAccepts(c, 0x1d) ? 1 : 0);

    if (c->serverFlags & kServerFlagObserveOnly) {
        c->viewOnly = true;
    }

    if (!resizeFramebuffer(c, initW > 0 ? initW : 1, initH > 0 ? initH : 1)) {
        if (outErrno) *outErrno = c->lastErrno;
        appleRfbDestroy(c);
        return APPLE_RFB_ERROR;
    }

    // 7. Handle SessionSelect if another user is logged in at console
    if ((c->serverFlags & kServerFlagSessionSelect) != 0) {
        logInfo(c, "AppleRfb: handling SessionSelect");
        if (!handleSessionSelection(c, username, password)) {
            if (outErrno) *outErrno = c->lastErrno;
            appleRfbDestroy(c);
            return APPLE_RFB_ERROR;
        }
    }

    // Wipe password from stack once auth & session selection are complete
    memset(password, 0, sizeof(password));

    // 8. Apple cleartext prelude: ViewerInfo -> SetMode -> AutoPasteboard -> SetEncryption(1)
    if (c->hasWrapKey) {
        uint8_t viewerInfo[66];
        buildViewerInfoMessage(viewerInfo);
        if (!streamWrite(c, viewerInfo, sizeof(viewerInfo))) goto io_error;

        {
            uint8_t setMode[4];
            buildSetModeMessage(setMode, c->viewOnly);
            if (!streamWrite(c, setMode, sizeof(setMode))) goto io_error;
        }

        if (serverAccepts(c, 0x15)) {
            uint8_t ap[8];
            buildAutoPasteboardMessage(ap, true);
            if (!streamWrite(c, ap, sizeof(ap))) goto io_error;
        }

        {
            const uint8_t setEnc1[12] = {0x12, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1};
            if (!streamWrite(c, setEnc1, sizeof(setEnc1))) goto io_error;
        }
    }

    // 9. SetPixelFormat (BGRA 32bpp)
    {
        uint8_t spf[20];
        buildSetPixelFormatBGRA(spf);
        if (!streamWrite(c, spf, sizeof(spf))) goto io_error;
    }

    // 10. Optional SetDisplayConfiguration (0x1d) for a single Virtual Display
    if (c->config.displayMode != APPLE_DISPLAY_STANDARD && serverAccepts(c, 0x1d)) {
        int requested = 1;
        double density = (c->config.virtualDensity == 2.0) ? 2.0 : 1.0;
        uint8_t vdMsg[512];
        size_t vdLen = 0;
        if (appleRfbBuildVirtualDisplayMessage(requested,
                                               c->virtualLogicalWidth,
                                               c->virtualLogicalHeight,
                                               density,
                                               3840, 2160,
                                               60.0, false,
                                               vdMsg, sizeof(vdMsg), &vdLen)) {
            logInfo(c, "AppleRfb: requesting %d virtual display (%dx%d @ %.0fx)",
                    requested, c->virtualLogicalWidth, c->virtualLogicalHeight, density);
            if (!streamWrite(c, vdMsg, vdLen)) goto io_error;
            c->activeVirtualDisplays = requested;
        }
    }

    // 11. SetEncodings + initial FramebufferUpdateRequest + AutoFrameBufferUpdate
    if (!sendEncodings(c)) goto io_error;
    if (!sendUpdateArm(c, true)) goto io_error;

    if (c->hasWrapKey) {
        // 12. Drain messages until 1103 (0x44f) Rekey activates the record layer
        c->pendingRekey = true;
        while (c->pendingRekey) {
            if (!appleRfbHandleServerMessage(c)) goto io_error;
        }

        // 13. Send SetEncryption(2) in cleartext, then enable outgoing AES-128-CBC record layer
        {
            const uint8_t setEnc2[8] = {0x12, 0, 0, 2, 0, 1, 0, 0};
            if (!streamWrite(c, setEnc2, sizeof(setEnc2))) goto io_error;
        }

        {
            pthread_mutex_lock(&c->writeMutex);
            c->encCtx = EVP_CIPHER_CTX_new();
            bool encOk = c->encCtx &&
                         EVP_EncryptInit_ex(c->encCtx, EVP_aes_128_cbc(), nullptr, c->nextKey, c->nextIv) == 1 &&
                         EVP_CIPHER_CTX_set_padding(c->encCtx, 0) == 1;
            if (encOk) {
                c->recordWriteActive = true;
            }
            pthread_mutex_unlock(&c->writeMutex);
            if (!encOk) {
                c->lastErrno = EIO;
                goto io_error;
            }
        }
        logInfo(c, "AppleRfb: encrypted record layer active");
    }

    c->running = true;
    c->lastPixelUpdateMs = nowMonotonicMs();
    c->lastWatchdogReqMs = 0;

    // If a DisplayLayout already arrived during rekey before running=true, apply single-display
    // selection (1 of up to 6) or re-arm framebuffer updates for the actual DisplayLayout dimensions now.
    {
        bool sentInitialSelect = false;
        if (c->hasLayout && !c->initialDisplayApplied && serverAccepts(c, 0x0d) && c->layout.count > 1) {
            int selectableCount = (c->layout.count > APPLE_MAX_SELECTABLE_DISPLAYS)
                                  ? APPLE_MAX_SELECTABLE_DISPLAYS
                                  : (int) c->layout.count;
            int requestedIndex = c->config.initialDisplayIndex;
            if (requestedIndex < 1 || requestedIndex > selectableCount) {
                requestedIndex = 1;
            }
            c->initialDisplayApplied = true;
            uint32_t targetId = c->layout.displays[requestedIndex - 1].id;
            if (targetId != 0 && c->layout.currentDisplayId != targetId) {
                if (appleRfbSelectDisplay(c, targetId)) {
                    sentInitialSelect = true;
                }
            }
        }
        if (c->hasLayout && !sentInitialSelect) {
            if (serverAccepts(c, 0x15)) {
                uint8_t ap[8];
                buildAutoPasteboardMessage(ap, true);
                streamWrite(c, ap, sizeof(ap));
            }
            sendUpdateArm(c, true);
        }
    }

    *outClient = c;
    return APPLE_RFB_OK;

io_error:
    memset(password, 0, sizeof(password));
    if (outErrno) *outErrno = c->lastErrno;
    appleRfbDestroy(c);
    return APPLE_RFB_ERROR;
}

static void maybeKickStalledFramebuffer(AppleRfbClient *c) {
    if (!c || !c->running || __atomic_load_n(&c->paused, __ATOMIC_ACQUIRE) ||
        c->fbWidth <= 0 || c->fbHeight <= 0)
        return;

    uint64_t now = nowMonotonicMs();
    if (now == 0) return;

    // Case 1: Never received pixels yet after connect/resize -> request full refresh
    if (!c->hasReceivedPixels) {
        uint64_t baseline = (c->lastWatchdogReqMs != 0) ? c->lastWatchdogReqMs : c->lastPixelUpdateMs;
        if (now >= baseline + 600ULL) {
            c->lastWatchdogReqMs = now;
            sendUpdateArm(c, true);
        }
        return;
    }

    // Case 2a: Input was sent, but no pixel update followed within 180ms -> recover immediately
    int inputPending = __atomic_load_n(&c->hasInputSinceLastPixel, __ATOMIC_ACQUIRE);
    uint64_t lastInputMs = __atomic_load_n(&c->lastInputSentMs, __ATOMIC_ACQUIRE);
    if (inputPending &&
        now >= lastInputMs + 180ULL &&
        (c->lastWatchdogReqMs == 0 || now >= c->lastWatchdogReqMs + 250ULL)) {
        __atomic_store_n(&c->hasInputSinceLastPixel, 0, __ATOMIC_RELEASE);
        c->lastWatchdogReqMs = now;
        c->lastInputRecoveryReqMs = now;
        c->inputFullRefreshPending = true;
        sendUpdateArm(c, false);
        return;
    }

    // Case 2b: Incremental recovery after input produced zero pixels for 650ms ->
    // escalate once to a non-incremental full refresh so macOS sends the complete frame.
    if (c->inputFullRefreshPending &&
        c->lastInputRecoveryReqMs != 0 &&
        now >= c->lastInputRecoveryReqMs + 650ULL) {
        c->inputFullRefreshPending = false;
        c->lastWatchdogReqMs = now;
        sendUpdateArm(c, true);
        return;
    }

    // Case 3: Periodic incremental keep-fresh (every 1.5s when quiet) so updates from
    // other clients or remote animations never freeze even if 0x09 push paused on macOS.
    if (now >= c->lastPixelUpdateMs + 1500ULL && now >= c->lastWatchdogReqMs + 1500ULL) {
        c->lastWatchdogReqMs = now;
        uint8_t req[10];
        buildFbUpdateRequest(req, true, c->fbWidth, c->fbHeight);
        streamWrite(c, req, sizeof(req));
    }
}

int appleRfbWaitForMessage(AppleRfbClient *c, int timeoutUsecs) {
    if (!c) return -1;
    if (c->recordReadActive && c->readPlainPos < c->readPlainLen)
        return 1;

    struct pollfd pfd;
    pfd.fd = c->sock;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int timeoutMs = timeoutUsecs / 1000;
    if (timeoutUsecs > 0 && timeoutMs == 0) timeoutMs = 1;
    int rc = poll(&pfd, 1, timeoutMs);
    if (rc < 0) {
        if (errno == EINTR) return 0;
        c->lastErrno = errno;
        return -1;
    }
    if (rc == 0) {
        maybeKickStalledFramebuffer(c);
    }
    return rc;
}

/******************************************************************************
 * Client-to-Server Actions
 *****************************************************************************/

bool appleRfbSendKeyEvent(AppleRfbClient *c, uint32_t keysym, bool down) {
    if (!c || c->viewOnly) return false;
    markInputActivity(c, nowMonotonicMs());
    uint8_t msg[8] = {0x04, down ? (uint8_t) 1 : (uint8_t) 0, 0, 0, 0, 0, 0, 0};
    putBe32(msg + 4, keysym);
    return streamWrite(c, msg, sizeof(msg));
}

bool appleRfbSendPointerEvent(AppleRfbClient *c, int x, int y, uint8_t mask) {
    if (!c || c->viewOnly) return false;

    double scale = 1.0;
    uint8_t keyCopy[16];
    bool hasKey = false;
    pthread_mutex_lock(&c->writeMutex);
    scale = c->pointerScale;
    hasKey = c->hasWrapKey;
    if (hasKey) memcpy(keyCopy, c->wrapKey, sizeof(keyCopy));
    pthread_mutex_unlock(&c->writeMutex);

    int wireX = (scale > 0.0 && scale != 1.0) ? (int) llround((double) x / scale) : x;
    int wireY = (scale > 0.0 && scale != 1.0) ? (int) llround((double) y / scale) : y;

    uint8_t wireMask = appleRfbMapPointerButtons(mask);
    uint8_t wheelBits = wireMask & (uint8_t) ~0x07u;
    uint8_t btnMask = wireMask & 0x07u;
    uint8_t prevButtons = c->lastPointerButtons;
    bool isScroll = (wheelBits != 0);
    bool buttonsChanged = (!isScroll && btnMask != prevButtons);
    uint64_t now = nowMonotonicMs();

    // Scroll wheel pulses (0x08/0x10/0x20/0x40) must go via plain 0x05 PointerEvent
    // (screensharingd scrolls on 0x05 with masks 8 or 16, not 0x10 EncryptedPointer).
    if (isScroll) {
        c->lastPointerX = wireX;
        c->lastPointerY = wireY;
        c->hasLastPointerPos = true;
        markInputActivity(c, now);

        uint8_t msg[6];
        msg[0] = 0x05;
        msg[1] = wheelBits;
        putBe16(msg + 2, clampU16(wireX));
        putBe16(msg + 4, clampU16(wireY));
        return streamWrite(c, msg, sizeof(msg));
    }

    // Before pressing a mouse button at a new position (touchscreen tap),
    // move macOS's pointer to the target coordinates first so hit-testing and
    // window focus always register cleanly.
    if (buttonsChanged && prevButtons == 0 && btnMask != 0 &&
        (!c->hasLastPointerPos || wireX != c->lastPointerX || wireY != c->lastPointerY)) {
        uint8_t moveMsg[6];
        moveMsg[0] = 0x05;
        moveMsg[1] = 0;
        putBe16(moveMsg + 2, clampU16(wireX));
        putBe16(moveMsg + 4, clampU16(wireY));
        if (!streamWrite(c, moveMsg, sizeof(moveMsg)))
            return false;
    }

    int clickCount = 0;
    if (buttonsChanged) {
        bool leftPressedNow = ((btnMask & 0x01u) != 0) && ((prevButtons & 0x01u) == 0);
        bool leftReleasedNow = ((btnMask & 0x01u) == 0) && ((prevButtons & 0x01u) != 0);
        if (leftPressedNow) {
            int dx = wireX - c->lastLeftDownX;
            int dy = wireY - c->lastLeftDownY;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (c->lastLeftDownMs != 0 && now >= c->lastLeftDownMs &&
                (now - c->lastLeftDownMs) <= 450ULL && dx <= 10 && dy <= 10) {
                c->leftClickCount = (c->leftClickCount < 7) ? (c->leftClickCount + 1) : 7;
            } else {
                c->leftClickCount = 1;
            }
            c->lastLeftDownMs = now;
            c->lastLeftDownX = wireX;
            c->lastLeftDownY = wireY;
            clickCount = c->leftClickCount;
        } else if (leftReleasedNow) {
            clickCount = (c->leftClickCount > 0) ? c->leftClickCount : 1;
        } else {
            clickCount = 1;
        }
    }

    c->lastPointerButtons = btnMask;
    c->lastPointerX = wireX;
    c->lastPointerY = wireY;
    c->hasLastPointerPos = true;
    markInputActivity(c, now);

    // On macOS Screen Sharing (0x10 supported + wrapKey), send button state changes
    // via 0x10 EncryptedPointerMessage so screensharingd marks this viewer as active
    // click/drag owner and passes exact clickCount to ScreensharingAgent.
    if (buttonsChanged && serverAccepts(c, 0x10) && hasKey) {
        uint8_t encMsg[18];
        if (appleRfbBuildEncryptedPointerMessage(keyCopy, btnMask, clickCount, wireX, wireY, encMsg)) {
            return streamWrite(c, encMsg, sizeof(encMsg));
        }
    }

    uint8_t msg[6];
    msg[0] = 0x05;
    msg[1] = btnMask;
    putBe16(msg + 2, clampU16(wireX));
    putBe16(msg + 4, clampU16(wireY));
    return streamWrite(c, msg, sizeof(msg));
}

bool appleRfbSendCutText(AppleRfbClient *c, const char *text, int len, bool isUtf8) {
    if (!c || c->viewOnly || !text || len < 0) return false;
    if (isUtf8 && supportsApplePasteboard(c)) {
        uint8_t *archive = nullptr;
        size_t archLen = 0;
        if (!buildUtf8PasteboardArchive(text, (size_t) len, false, &archive, &archLen))
            return false;
        cacheSavedClipboardArchive(c, archive, archLen);
        bool ok = sendApplePasteboardArchive(c, archive, archLen);
        free(archive);
        return ok;
    }
    size_t total = 8u + (size_t) len;
    uint8_t *msg = (uint8_t *) malloc(total);
    if (!msg) return false;
    msg[0] = 0x06;
    msg[1] = msg[2] = msg[3] = 0;
    putBe32(msg + 4, (uint32_t) len);
    if (len > 0) memcpy(msg + 8, text, (size_t) len);
    bool ok = streamWrite(c, msg, total);
    free(msg);
    return ok;
}

bool appleRfbSelectKeyboardSource(AppleRfbClient *c, const char *sourceId, bool clearAfter) {
    if (!c || c->viewOnly || !sourceId || !serverAccepts(c, 0x1a))
        return false;

    size_t idLen = strlen(sourceId);
    if (idLen == 0 || idLen > 512u)
        return false;

    uint8_t buf[8u + 512u + 8u + 8u];
    size_t firstLen = 0;
    if (!appleRfbBuildKeyboardSourceMessage(sourceId, buf, sizeof(buf), &firstLen))
        return false;

    // Follow with VoidSymbol key-up (0x04) to make screensharingd apply TISSelectInputSource
    size_t pos = firstLen;
    buf[pos + 0] = 0x04;
    buf[pos + 1] = 0;
    buf[pos + 2] = 0;
    buf[pos + 3] = 0;
    putBe32(buf + pos + 4, kKeySymVoid);
    pos += 8u;

    if (clearAfter) {
        size_t clearLen = 0;
        if (!appleRfbBuildKeyboardSourceMessage("", buf + pos, sizeof(buf) - pos, &clearLen))
            return false;
        pos += clearLen;
    }

    logInfo(c, "AppleRfb: SelectKeyboardSource '%s' (clearAfter=%d)", sourceId, clearAfter ? 1 : 0);
    markInputActivity(c, nowMonotonicMs());
    return streamWrite(c, buf, pos);
}

bool appleRfbSendMacKeyCode(AppleRfbClient *c, uint16_t macKeyCode, uint32_t keysym, bool down) {
    if (!c || c->viewOnly)
        return false;

    if (serverAccepts(c, 0x10) && c->hasWrapKey) {
        uint8_t keyCopy[16];
        pthread_mutex_lock(&c->writeMutex);
        memcpy(keyCopy, c->wrapKey, sizeof(keyCopy));
        pthread_mutex_unlock(&c->writeMutex);

        uint8_t msg[18];
        if (appleRfbBuildEncryptedKeyMessage(keyCopy, macKeyCode, keysym, down, msg)) {
            markInputActivity(c, nowMonotonicMs());
            return streamWrite(c, msg, sizeof(msg));
        }
    }

    return appleRfbSendKeyEvent(c, keysym, down);
}

bool appleRfbToggleInputSource(AppleRfbClient *c) {
    if (!c || c->viewOnly)
        return false;

    logInfo(c, "AppleRfb: ToggleInputSource (Control+Space)");
    return appleRfbSendMacKeyCode(c, kMacKeyCodeControl, 0xffe3u, true) &&
           appleRfbSendMacKeyCode(c, kMacKeyCodeSpace,   0x0020u, true) &&
           appleRfbSendMacKeyCode(c, kMacKeyCodeSpace,   0x0020u, false) &&
           appleRfbSendMacKeyCode(c, kMacKeyCodeControl, 0xffe3u, false);
}

static size_t decodeNextUtf8Codepoint(const uint8_t *s, size_t len, uint32_t *cpOut) {
    if (len == 0) return 0;
    uint8_t c0 = s[0];
    if ((c0 & 0x80u) == 0) {
        *cpOut = c0;
        return 1;
    }
    if ((c0 & 0xe0u) == 0xc0u && len >= 2 && (s[1] & 0xc0u) == 0x80u) {
        *cpOut = ((uint32_t) (c0 & 0x1fu) << 6) | (uint32_t) (s[1] & 0x3fu);
        return 2;
    }
    if ((c0 & 0xf0u) == 0xe0u && len >= 3 &&
        (s[1] & 0xc0u) == 0x80u && (s[2] & 0xc0u) == 0x80u) {
        *cpOut = ((uint32_t) (c0 & 0x0fu) << 12) |
                 ((uint32_t) (s[1] & 0x3fu) << 6) |
                 (uint32_t) (s[2] & 0x3fu);
        return 3;
    }
    if ((c0 & 0xf8u) == 0xf0u && len >= 4 &&
        (s[1] & 0xc0u) == 0x80u && (s[2] & 0xc0u) == 0x80u && (s[3] & 0xc0u) == 0x80u) {
        *cpOut = ((uint32_t) (c0 & 0x07u) << 18) |
                 ((uint32_t) (s[1] & 0x3fu) << 12) |
                 ((uint32_t) (s[2] & 0x3fu) << 6) |
                 (uint32_t) (s[3] & 0x3fu);
        return 4;
    }
    *cpOut = 0xfffdu;
    return 1;
}

bool appleRfbSendTextDiff(AppleRfbClient *c, int backspaces, const char *utf8, int len) {
    if (!c || c->viewOnly)
        return false;
    if (backspaces < 0) backspaces = 0;
    if (!utf8 || len < 0) len = 0;
    if (backspaces == 0 && len == 0)
        return true;

    markInputActivity(c, nowMonotonicMs());

    // Upper bound: each backspace = 16 bytes (2 KeyEvents), each UTF-8 byte <= 1 codepoint = 16 bytes
    size_t maxEventsBytes = ((size_t) backspaces + (size_t) len) * 16u;
    uint8_t *buf = (uint8_t *) malloc(maxEventsBytes > 0 ? maxEventsBytes : 1u);
    if (!buf) return false;
    size_t out = 0;

    auto emitKey = [&](uint32_t keysym) {
        buf[out + 0] = 0x04; buf[out + 1] = 1; buf[out + 2] = 0; buf[out + 3] = 0;
        putBe32(buf + out + 4, keysym);
        out += 8;
        buf[out + 0] = 0x04; buf[out + 1] = 0; buf[out + 2] = 0; buf[out + 3] = 0;
        putBe32(buf + out + 4, keysym);
        out += 8;
    };

    for (int i = 0; i < backspaces; ++i) {
        emitKey(0xff08u); // XK_BackSpace
    }

    const uint8_t *p = (const uint8_t *) utf8;
    size_t rem = (size_t) len;
    while (rem > 0) {
        uint32_t cp = 0;
        size_t adv = decodeNextUtf8Codepoint(p, rem, &cp);
        if (adv == 0) break;
        p += adv;
        rem -= adv;

        // Collapse CRLF into a single XK_Return
        if (cp == '\r' && rem > 0 && p[0] == '\n') {
            p += 1;
            rem -= 1;
        }

        uint32_t keysym = 0;
        if (cp == '\n' || cp == '\r') {
            keysym = 0xff0du; // XK_Return
        } else if (cp == '\t') {
            keysym = 0xff09u; // XK_Tab
        } else if (cp == '\b') {
            keysym = 0xff08u; // XK_BackSpace
        } else if (cp <= 0xffu) {
            keysym = cp;
        } else if (cp <= 0xffffu) {
            keysym = 0x01000000u | cp;
        } else {
            // Non-BMP codepoint cannot be encoded in macOS 16-bit UniChar KeyEvent
            continue;
        }

        emitKey(keysym);
    }

    bool ok = (out == 0) ? true : streamWrite(c, buf, out);
    free(buf);
    return ok;
}

bool appleRfbSendUnicodeText(AppleRfbClient *c, const char *utf8, int len) {
    if (!c || c->viewOnly || !utf8 || len <= 0)
        return false;
    return appleRfbSendTextDiff(c, 0, utf8, len);
}

bool appleRfbPasteText(AppleRfbClient *c, const char *utf8, int len) {
    if (!c || c->viewOnly || !utf8 || len <= 0)
        return false;
    markInputActivity(c, nowMonotonicMs());

    if (!supportsApplePasteboard(c)) {
        return appleRfbSendTextDiff(c, 0, utf8, len);
    }

    uint8_t *archive = nullptr;
    size_t archLen = 0;
    if (!buildUtf8PasteboardArchive(utf8, (size_t) len, true, &archive, &archLen))
        return false;

    bool sentClip = sendApplePasteboardArchive(c, archive, archLen);
    free(archive);
    if (!sentClip)
        return false;

    // Allow screensharingd -> ScreensharingAgent -> pboard IPC to commit the
    // transient pasteboard item before injecting Cmd+V.
    usleep(15000);

    uint8_t keyCopy[16];
    bool hasKey = false;
    if (serverAccepts(c, 0x10)) {
        pthread_mutex_lock(&c->writeMutex);
        hasKey = c->hasWrapKey;
        if (hasKey) memcpy(keyCopy, c->wrapKey, sizeof(keyCopy));
        pthread_mutex_unlock(&c->writeMutex);
    }

    // Prefer layout-independent physical Command(55) + V(9) via 0x10 EncryptedKey so
    // Cmd+V works reliably even when macOS is currently in a Korean/CJK Input Method.
    if (hasKey) {
        uint8_t encBatch[72];
        if (appleRfbBuildEncryptedKeyMessage(keyCopy, kMacKeyCodeCommand, 0xffebu, true,  encBatch + 0) &&
            appleRfbBuildEncryptedKeyMessage(keyCopy, kMacKeyCodeV,       0x0076u, true,  encBatch + 18) &&
            appleRfbBuildEncryptedKeyMessage(keyCopy, kMacKeyCodeV,       0x0076u, false, encBatch + 36) &&
            appleRfbBuildEncryptedKeyMessage(keyCopy, kMacKeyCodeCommand, 0xffebu, false, encBatch + 54)) {
            return streamWrite(c, encBatch, sizeof(encBatch));
        }
    }

    uint8_t keyBatch[32];
    size_t o = 0;
    auto putKey04 = [&](bool down, uint32_t sym) {
        keyBatch[o + 0] = 0x04;
        keyBatch[o + 1] = down ? 1u : 0u;
        keyBatch[o + 2] = 0;
        keyBatch[o + 3] = 0;
        putBe32(keyBatch + o + 4, sym);
        o += 8u;
    };
    putKey04(true,  0xffebu); // XK_Super_L (Command) down
    putKey04(true,  0x0076u); // 'v' down
    putKey04(false, 0x0076u); // 'v' up
    putKey04(false, 0xffebu); // XK_Super_L (Command) up

    return streamWrite(c, keyBatch, sizeof(keyBatch));
}

bool appleRfbRestoreClipboard(AppleRfbClient *c) {
    if (!c || c->viewOnly || !c->clipMutexInit || !supportsApplePasteboard(c))
        return false;

    uint8_t *copy = nullptr;
    size_t len = 0;
    pthread_mutex_lock(&c->clipMutex);
    if (c->savedClipboardArchive && c->savedClipboardArchiveLen > 0) {
        copy = (uint8_t *) malloc(c->savedClipboardArchiveLen);
        if (copy) {
            memcpy(copy, c->savedClipboardArchive, c->savedClipboardArchiveLen);
            len = c->savedClipboardArchiveLen;
        }
    }
    pthread_mutex_unlock(&c->clipMutex);

    if (!copy || len == 0) {
        free(copy);
        return false;
    }
    bool ok = sendApplePasteboardArchive(c, copy, len);
    free(copy);
    return ok;
}

bool appleRfbSelectDisplay(AppleRfbClient *c, uint32_t displayId) {
    if (!c || !serverAccepts(c, 0x0d))
        return false;
    // Enforce single-display selection only (never select combined All Displays).
    if (displayId == 0u || displayId == APPLE_ALL_DISPLAYS_WIRE)
        return false;

    uint8_t msg[8];
    appleRfbBuildSetDisplayMessage(displayId, msg);
    logInfo(c, "AppleRfb: SelectDisplay id=0x%x (combineAll=%u)", displayId, (unsigned) msg[1]);
    return streamWrite(c, msg, sizeof(msg));
}

bool appleRfbSetDesktopSize(AppleRfbClient *c, int width, int height) {
    if (!c || c->activeVirtualDisplays <= 0 || !serverAccepts(c, 0x1d))
        return false;
    if (width < 800) width = 800;
    if (height < 525) height = 525;
    if (width > 3840) width = 3840;
    if (height > 2160) height = 2160;
    if (width == c->virtualLogicalWidth && height == c->virtualLogicalHeight)
        return true;

    double density = (c->config.virtualDensity == 2.0) ? 2.0 : 1.0;
    uint8_t vdMsg[512];
    size_t vdLen = 0;
    if (!appleRfbBuildVirtualDisplayMessage(c->activeVirtualDisplays,
                                            width, height, density,
                                            3840, 2160, 60.0, false,
                                            vdMsg, sizeof(vdMsg), &vdLen)) {
        return false;
    }
    c->virtualLogicalWidth = width;
    c->virtualLogicalHeight = height;
    return streamWrite(c, vdMsg, vdLen);
}

bool appleRfbRefreshFramebuffer(AppleRfbClient *c) {
    if (!c || __atomic_load_n(&c->paused, __ATOMIC_ACQUIRE)) return false;
    return sendUpdateArm(c, false);
}

void appleRfbSetPaused(AppleRfbClient *c, bool paused) {
    if (!c) return;
    __atomic_store_n(&c->paused, paused, __ATOMIC_RELEASE);
    if (c->running) {
        sendUpdateArm(c, !paused);
    }
}

void appleRfbSetViewOnly(AppleRfbClient *c, bool viewOnly) {
    if (!c) return;
    if (c->serverFlags & kServerFlagObserveOnly)
        viewOnly = true;
    if (c->viewOnly == viewOnly) return;
    c->viewOnly = viewOnly;
    if (c->running) {
        uint8_t msg[4];
        buildSetModeMessage(msg, viewOnly);
        streamWrite(c, msg, sizeof(msg));
    }
}

/******************************************************************************
 * Getters & Teardown
 *****************************************************************************/

const char *appleRfbGetDesktopName(const AppleRfbClient *c) {
    return (c && c->desktopName) ? c->desktopName : "";
}

int appleRfbGetWidth(const AppleRfbClient *c) {
    return c ? c->fbWidth : 0;
}

int appleRfbGetHeight(const AppleRfbClient *c) {
    return c ? c->fbHeight : 0;
}

bool appleRfbIsEncrypted(const AppleRfbClient *c) {
    return c && c->recordReadActive && c->recordWriteActive;
}

bool appleRfbSupportsDisplaySelection(const AppleRfbClient *c) {
    return c && serverAccepts(c, 0x0d);
}

bool appleRfbSupportsVirtualDisplay(const AppleRfbClient *c) {
    return c && serverAccepts(c, 0x1d);
}

bool appleRfbSupportsKeyboardSync(const AppleRfbClient *c) {
    return c && c->extended && (serverAccepts(c, 0x1a) || (serverAccepts(c, 0x10) && c->hasWrapKey));
}

bool appleRfbSupportsUtf8Pasteboard(const AppleRfbClient *c) {
    return supportsApplePasteboard(c);
}

int appleRfbMaxVirtualDisplays(const AppleRfbClient *c) {
    return (c && c->extended) ? (int) (c->serverFlags >> 5) : 0;
}

void appleRfbDestroy(AppleRfbClient *c) {
    if (!c) return;

    if (c->decCtx) EVP_CIPHER_CTX_free(c->decCtx);
    if (c->encCtx) EVP_CIPHER_CTX_free(c->encCtx);
    if (c->writeMutexInit) pthread_mutex_destroy(&c->writeMutex);
    if (c->clipMutexInit) pthread_mutex_destroy(&c->clipMutex);

    zlibDecoderFree(&c->zlibDec);
    zlibDecoderFree(&c->zrleDec);
    zlibDecoderFree(&c->thousandsDec);

    for (size_t i = 0; i < c->cursorCount; ++i) {
        free(c->cursors[i].bgra);
    }

    free(c->savedClipboardArchive);
    free(c->desktopName);
    free(c->readPlainBuf);
    free(c->writeWireBuf);
    free(c->scratchBuf);
    OPENSSL_cleanse(c, sizeof(*c));
    free(c);
}
