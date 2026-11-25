/*
 * Copyright (c) 2026  AVNC Contributors.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

#ifndef AVNC_APPLE_RFB_H
#define AVNC_APPLE_RFB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Display configuration modes matching macOS Screen Sharing options.
 */
typedef enum {
    APPLE_DISPLAY_STANDARD = 0,   // Physical displays (single monitor selectable, up to 6)
    APPLE_DISPLAY_VIRTUAL_1 = 1   // High Performance: 1 Virtual Display
} AppleDisplayMode;

/**
 * Result codes returned by appleRfbConnect().
 */
typedef enum {
    APPLE_RFB_OK = 0,
    APPLE_RFB_FALLBACK = 1, // Server/auth requires standard LibVNCClient path
    APPLE_RFB_ERROR = -1
} AppleRfbConnectResult;

#define APPLE_ALL_DISPLAYS_WIRE 0xffffffffu
#define APPLE_MAX_LAYOUT_DISPLAYS 25
#define APPLE_MAX_SELECTABLE_DISPLAYS 6

#define APPLE_DISPLAY_FLAG_MAIN     0x01u
#define APPLE_DISPLAY_FLAG_MIRRORED 0x02u
#define APPLE_DISPLAY_FLAG_VIRTUAL  0x04u

typedef struct {
    uint32_t id;
    int16_t logicalX;
    int16_t logicalY;
    uint16_t logicalWidth;
    uint16_t logicalHeight;
    int16_t backingX;
    int16_t backingY;
    uint16_t backingWidth;
    uint16_t backingHeight;
    double density;
    double scale;
    uint32_t flags;
} AppleDisplayInfo;

typedef struct {
    uint16_t version;
    uint16_t desktopWidth;
    uint16_t desktopHeight;
    uint16_t fbWidth;
    uint16_t fbHeight;
    uint32_t currentDisplayId; // APPLE_ALL_DISPLAYS_WIRE when showing all displays
    uint32_t sessionState;
    uint16_t count;
    AppleDisplayInfo displays[APPLE_MAX_LAYOUT_DISPLAYS];
} AppleDisplayLayout;

typedef struct {
    int securityType;         // 0 = Auto, 30 = macOS/ARD
    bool authenticatedTunnel;
    int imageQuality;         // 0..9
    bool useRawEncoding;
    bool useLocalCursor;
    bool viewOnly;
    AppleDisplayMode displayMode;
    int initialDisplayIndex;  // 1..6 (single display only; <= 0 defaults to 1)
    int virtualWidth;         // Logical width in points (default 1920)
    int virtualHeight;        // Logical height in points (default 1080)
    double virtualDensity;    // 1.0 or 2.0 (default 1.0)
} AppleRfbConfig;

typedef struct {
    /**
     * Retrieve username & password for Apple account auth.
     * Returns true on success.
     */
    bool (*getCredential)(void *userCtx, char *usernameOut, size_t usernameCap,
                          char *passwordOut, size_t passwordCap);

    /**
     * Allocate or reallocate framebuffer of width * height * 4 bytes (BGRA).
     * Returns pointer to newly allocated buffer, or NULL on error.
     */
    uint8_t *(*resizeFramebuffer)(void *userCtx, int width, int height);

    /**
     * Lock/unlock framebuffer around pixel writes (paired with GL texture upload).
     */
    void (*lockFramebuffer)(void *userCtx);
    void (*unlockFramebuffer)(void *userCtx);

    void (*onFramebufferUpdated)(void *userCtx);
    void (*onCursorShape)(void *userCtx, const uint8_t *bgra, uint16_t width, uint16_t height,
                          uint16_t xHot, uint16_t yHot);
    void (*onCursorPos)(void *userCtx, int x, int y);
    void (*onDisplaysChanged)(void *userCtx, const AppleDisplayLayout *layout,
                              bool supportsSelection);
    void (*onKeyboardSourceChanged)(void *userCtx, const char *sourceId,
                                    bool secureInput, bool supportsSelection);
    void (*onCutText)(void *userCtx, const char *text, int len, bool isUtf8);
    void (*onBell)(void *userCtx);
    void (*logInfo)(const char *fmt, ...);
    void (*logError)(const char *fmt, ...);
    // Optional destination row range of each decoded pixel rectangle.
    void (*onFramebufferRect)(void *userCtx, int y, int height);
} AppleRfbCallbacks;

typedef struct AppleRfbClient AppleRfbClient;

/**
 * Peeks 12 bytes from connected socket (without consuming them) to check whether
 * the server advertises Apple's RFB 003.889 / 003.888 protocol version.
 * Returns 1 if Apple banner detected, 0 if non-Apple RFB banner, -1 on I/O error.
 */
int appleRfbPeekIsAppleBanner(int sock, int timeoutMs);

/**
 * Performs Apple RFB 3.889 handshake, authentication, ServerInit, optional
 * virtual display configuration (0x1d), encodings negotiation, and 0x44f rekey.
 *
 * On APPLE_RFB_OK, the caller transfers socket ownership to *outClient (do not
 * close sock separately; appleRfbDestroy() does NOT close sock so rfbClientCleanup
 * can close it, or caller may close sock after destroy).
 */
AppleRfbConnectResult appleRfbConnect(int sock, const AppleRfbConfig *config,
                                      const AppleRfbCallbacks *callbacks, void *userCtx,
                                      AppleRfbClient **outClient, int *outErrno);

/**
 * Waits for server data (returning immediately if decrypted record bytes are
 * already buffered). Returns >0 if readable, 0 on timeout, -1 on error.
 */
int appleRfbWaitForMessage(AppleRfbClient *client, int timeoutUsecs);

/**
 * Reads and dispatches one RFB / Apple server message. Returns false on error/EOF.
 */
bool appleRfbHandleServerMessage(AppleRfbClient *client);

bool appleRfbSendKeyEvent(AppleRfbClient *client, uint32_t keysym, bool down);
bool appleRfbSendPointerEvent(AppleRfbClient *client, int x, int y, uint8_t mask);
bool appleRfbSendCutText(AppleRfbClient *client, const char *text, int len, bool isUtf8);

/**
 * Selects a macOS TIS input source via Apple 0x1a SelectKeyboardSource + VoidSymbol key-up.
 * When clearAfter is true, sends a trailing 0x1a("") to clear the server override latch.
 */
bool appleRfbSelectKeyboardSource(AppleRfbClient *client, const char *sourceId, bool clearAfter);

/**
 * Sends a macOS physical ANSI virtual keyCode via 0x10 EncryptedKeyMessage (or 0x04 fallback).
 */
bool appleRfbSendMacKeyCode(AppleRfbClient *client, uint16_t macKeyCode, uint32_t keysym, bool down);

/**
 * Toggles macOS input source by sending Control+Space via 0x10 physical keyCodes.
 */
bool appleRfbToggleInputSource(AppleRfbClient *client);

/**
 * Sends backspaces followed by UTF-8 BMP text via atomic 0x04 KeyEvents using Apple
 * Unicode keysyms (0x01000000 | codepoint) in a single stream write.
 */
bool appleRfbSendTextDiff(AppleRfbClient *client, int backspaces, const char *utf8, int len);

/**
 * Sends UTF-8 BMP text via 0x04 KeyEvents using Apple Unicode keysyms (0x01000000 | codepoint).
 */
bool appleRfbSendUnicodeText(AppleRfbClient *client, const char *utf8, int len);

/**
 * Delivers UTF-8 text via transient Apple 0x1f pasteboard + Cmd+V (works under any macOS IME).
 */
bool appleRfbPasteText(AppleRfbClient *client, const char *utf8, int len);

/**
 * Restores the last non-transient remote clipboard archive after transient PasteText calls.
 */
bool appleRfbRestoreClipboard(AppleRfbClient *client);

/**
 * Selects a specific display (by CGDirectDisplayID) or all displays (0 or 0xffffffff).
 * Mirrors macOS Screen Sharing's "View -> Display 1 / Display 2 / All Displays".
 */
bool appleRfbSelectDisplay(AppleRfbClient *client, uint32_t displayId);

/**
 * Updates virtual display resolution via 0x1d SetDisplayConfiguration when supported.
 */
bool appleRfbSetDesktopSize(AppleRfbClient *client, int width, int height);

bool appleRfbRefreshFramebuffer(AppleRfbClient *client);
void appleRfbSetPaused(AppleRfbClient *client, bool paused);
void appleRfbSetViewOnly(AppleRfbClient *client, bool viewOnly);

const char *appleRfbGetDesktopName(const AppleRfbClient *client);
int appleRfbGetWidth(const AppleRfbClient *client);
int appleRfbGetHeight(const AppleRfbClient *client);
bool appleRfbIsEncrypted(const AppleRfbClient *client);
bool appleRfbSupportsDisplaySelection(const AppleRfbClient *client);
/** Whether a layout already contains virtual displays (i.e. a High Performance viewer is active). */
bool appleRfbLayoutHasVirtualDisplay(const AppleDisplayLayout *layout);
bool appleRfbSupportsVirtualDisplay(const AppleRfbClient *client);
bool appleRfbSupportsKeyboardSync(const AppleRfbClient *client);
bool appleRfbSupportsUtf8Pasteboard(const AppleRfbClient *client);
int appleRfbMaxVirtualDisplays(const AppleRfbClient *client);

void appleRfbDestroy(AppleRfbClient *client);

/* Pure wire-format helpers exposed for unit testing */
void appleRfbBuildSetDisplayMessage(uint32_t displayId, uint8_t out[8]);
bool appleRfbBuildVirtualDisplayMessage(int displayCount, int logicalWidth, int logicalHeight,
                                        double density, int maxWidth, int maxHeight,
                                        double refreshRate, bool hdr,
                                        uint8_t *out, size_t outCap, size_t *outLen);
bool appleRfbParseDisplayLayout(const uint8_t *payload, size_t len, AppleDisplayLayout *out);
bool appleRfbParseKeyboardSource(const uint8_t *payload, size_t len,
                                 char *sourceIdOut, size_t sourceIdCap,
                                 bool *secureInputOut);
bool appleRfbBuildKeyboardSourceMessage(const char *sourceId,
                                        uint8_t *out, size_t outCap, size_t *outLen);
bool appleRfbBuildEncryptedKeyMessage(const uint8_t wrapKey[16],
                                      uint16_t macKeyCode, uint32_t keysym, bool down,
                                      uint8_t out[18]);
bool appleRfbBuildEncryptedPointerMessage(const uint8_t wrapKey[16],
                                          uint8_t appleButtons, int clickCount,
                                          int wireX, int wireY, uint8_t out[18]);
uint8_t appleRfbMapPointerButtons(uint8_t standardMask);

#ifdef __cplusplus
}
#endif

#endif // AVNC_APPLE_RFB_H
