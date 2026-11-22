/*
 * Copyright (c) 2020  Gaurav Ujjwal.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

#include <jni.h>
#include <GLES2/gl2.h>
#include <rfb/rfbclient.h>
#include <openssl/x509v3.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "AppleRfb.h"
#include "ClientEx.h"
#include "FramebufferLimits.h"
#include "Utility.h"


/******************************************************************************
 * Library Initialization
 *****************************************************************************/

struct JniContext {
    JavaVM *vm;                     //JVM Instance
    jclass managedCls;              //Managed `VncClient` class
    jmethodID cbFramebufferUpdated; //Cached reference to managed callback

    [[nodiscard]]
    JNIEnv *getEnvUnchecked() const {
        JNIEnv *env = nullptr;

        if (vm != nullptr && vm->GetEnv((void **) &env, JNI_VERSION_1_6) == JNI_OK)
            return env;

        return nullptr;
    }

    [[nodiscard]]
    JNIEnv *getEnv() const {
        JNIEnv *env = getEnvUnchecked();
        if (env)
            return env;

        abort(); // Just crash if we can't get JNIEnv
    }
};

static JniContext context{};

/**
 * Called when our library is loaded.
 */
JNIEXPORT jint
JNI_OnLoad(JavaVM *vm, void *) {
    context.vm = vm;

    if (context.getEnvUnchecked() == nullptr)
        return JNI_ERR;

    return JNI_VERSION_1_6;
}

JNIEXPORT void
JNI_OnUnload(JavaVM *, void *) {
    if (context.managedCls) {
        context.getEnv()->DeleteGlobalRef(context.managedCls);
        context.managedCls = nullptr;
    }
}


extern "C"
JNIEXPORT void JNICALL
Java_com_gaurav_avnc_vnc_VncClient_initLibrary(JNIEnv *env, jclass clazz) {
    if (context.managedCls)
        context.getEnv()->DeleteGlobalRef(context.managedCls);

    context.managedCls = (jclass) env->NewGlobalRef(clazz);
    context.cbFramebufferUpdated = env->GetMethodID(context.managedCls, "cbFinishedFrameBufferUpdate", "()V");
    //TODO: Cache more method IDs so we don't have to repeatedly search them

    rfbClientLog = &log_info;
    rfbClientErr = &log_error;
}


/******************************************************************************
 * rfbClient Callbacks
 *****************************************************************************/

static char *onGetPassword(rfbClient *client) {
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    auto mid = env->GetMethodID(cls, "cbGetPassword", "()Ljava/lang/String;");
    auto jPassword = (jstring) env->CallObjectMethod(obj, mid);
    if (env->ExceptionCheck())
        return nullptr;

    return getNativeStrCopy(env, jPassword);
}

static rfbCredential *getUserCredential(rfbClient *client) {
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    //Retrieve credentials
    jmethodID mid = env->GetMethodID(cls, "cbGetCredential",
                                     "()Lcom/gaurav/avnc/vnc/UserCredential;");
    jobject jCredential = env->CallObjectMethod(obj, mid);
    if (!jCredential || env->ExceptionCheck()) {
        return nullptr;
    }

    //Extract username & password
    auto jCredentialCls = env->GetObjectClass(jCredential);
    auto usernameField = env->GetFieldID(jCredentialCls, "username", "Ljava/lang/String;");
    auto jUsername = env->GetObjectField(jCredential, usernameField);

    auto passwordField = env->GetFieldID(jCredentialCls, "password", "Ljava/lang/String;");
    auto jPassword = env->GetObjectField(jCredential, passwordField);

    //Create native rfbCredential
    auto credential = (rfbCredential *) malloc(sizeof(rfbCredential));
    if (!credential) return nullptr;
    credential->userCredential.username = getNativeStrCopy(env, (jstring) jUsername);
    credential->userCredential.password = getNativeStrCopy(env, (jstring) jPassword);

    return credential;
}

static rfbCredential *onGetCredential(rfbClient *client, int credentialType) {
    if (credentialType == rfbCredentialTypeX509)
        return static_cast<rfbCredential *>(calloc(1, sizeof(rfbCredential)));
    auto ex = getClientExtension(client);
    if (credentialType != rfbCredentialTypeUser ||
        !(ex->authenticatedTunnel || (client->subAuthScheme == rfbVeNCryptX509Plain && ex->verifiedCertificate))) {
        ex->authenticationError = EPROTONOSUPPORT;
        return nullptr;
    }
    return getUserCredential(client);
}

static rfbBool onVerifyServerCertificate(rfbClient *client, const unsigned char *der, int der_len) {
    auto ex = getClientExtension(client);
    ex->verifiedCertificate = false;
    const unsigned char *p = der;
    X509 *certificate = der_len > 0 ? d2i_X509(nullptr, &p, der_len) : nullptr;
    const char *host = ex->certificateHost;
    unsigned char ip[16];
    bool valid = false;
    if (certificate && p == der + der_len && host && *host &&
        X509_check_purpose(certificate, X509_PURPOSE_SSL_SERVER, 0) == 1) {
        bool isIp = inet_pton(AF_INET, host, ip) == 1 || inet_pton(AF_INET6, host, ip) == 1;
        valid = isIp ? X509_check_ip_asc(certificate, host, 0) == 1 :
                       X509_check_host(certificate, host, 0, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr) == 1;
    }
    X509_free(certificate);
    if (!valid) {
        ex->authenticationError = EBADMSG;
        return FALSE;
    }
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    jbyteArray bytes = env->NewByteArray(der_len);
    if (!bytes) {
        rfbClientErr("onVerifyServerCertificate: Unable to allocate memory for DER");
        return FALSE;
    }
    env->SetByteArrayRegion(bytes, 0, der_len, reinterpret_cast<const jbyte *>(der));

    jmethodID mid = env->GetMethodID(cls, "cbVerifyServerCertificate", "([B)Z");
    auto result = env->CallBooleanMethod(obj, mid, bytes);
    env->DeleteLocalRef(bytes);
    ex->verifiedCertificate = result == JNI_TRUE && !env->ExceptionCheck();
    if (!ex->verifiedCertificate) ex->authenticationError = EBADMSG;
    return ex->verifiedCertificate ? TRUE : FALSE;
}

static void onBell(rfbClient *client) {
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    jmethodID mid = env->GetMethodID(cls, "cbBell", "()V");
    env->CallVoidMethod(obj, mid);
}

static void onGotXCutText(rfbClient *client, const char *text, int len, bool is_utf8) {
    if (!text || len <= 0) return;

    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    jbyteArray bytes = env->NewByteArray(len);
    if (!bytes) {
        rfbClientErr("onGotXCutText: Unable to allocate memory for cut text");
        return;
    }
    env->SetByteArrayRegion(bytes, 0, len, reinterpret_cast<const jbyte *>(text));

    jmethodID mid = env->GetMethodID(cls, "cbGotXCutText", "([BZ)V");
    env->CallVoidMethod(obj, mid, bytes, is_utf8);
    env->DeleteLocalRef(bytes);
}

static void onGotXCutTextLatin1(rfbClient *client, const char *text, int len) {
    onGotXCutText(client, text, len, false);
}

static void onGotXCutTextUTF8(rfbClient *client, const char *text, int len) {
    if (text && len > 0 && text[len - 1] == '\0')
        --len; // LibVNCClient includes terminating NULL in length for UTF8
    onGotXCutText(client, text, len, true);
}

static rfbBool onHandleCursorPos(rfbClient *client, int x, int y) {
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    jmethodID mid = env->GetMethodID(cls, "cbHandleCursorPos", "(II)V");
    env->CallVoidMethod(obj, mid, x, y);

    return TRUE;
}

static void onGotFrameBufferUpdate(rfbClient *client, int, int y, int, int height) {
    auto ex = getClientExtension(client);
    // UltraZip packs multiple destinations into a rectangle with zero header height.
    if (height == 0)
        ex->pendingRows.include(0, client->height, client->height);
    else
        ex->pendingRows.include(y, y + height, client->height);
}

static void onFinishedFrameBufferUpdate(rfbClient *client) {
    auto ex = getClientExtension(client);
    if (ex->pendingRows.end == 0)
        return;

    LOCK(ex->mutex);
    ex->framebufferTexture.publish(ex->pendingRows);
    UNLOCK(ex->mutex);
    ex->pendingRows = {};

    auto obj = getManagedClient(client);
    auto env = context.getEnv();

    env->CallVoidMethod(obj, context.cbFramebufferUpdated);
}

/**
 * We need to use our own allocator to know when frame size has changed.
 * and to acquire framebuffer lock during modification.
 */
static rfbBool onMallocFrameBuffer(rfbClient *client) {

    const auto width = client->width;
    const auto height = client->height;
    size_t allocSize;
    if (client->format.bitsPerPixel != 32 || !framebufferByteCount(width, height, 4, &allocSize)) {
        rfbClientErr("Unsupported framebuffer dimensions or pixel format\n");
        errno = EOVERFLOW;
        return FALSE;
    }
    auto ex = getClientExtension(client);

    LOCK(ex->mutex);
    {

        if (client->frameBuffer)
            free(client->frameBuffer);

        client->frameBuffer = static_cast<uint8_t *>(malloc(allocSize));

        if (client->frameBuffer) {
            ex->framebufferTexture.resize(width, height);
            memset(client->frameBuffer, 0, allocSize); //Clear any garbage
        } else {
            ex->framebufferTexture.resize(0, 0);
        }
        ex->pendingRows = {};
    }
    UNLOCK(ex->mutex);

    if (client->frameBuffer == nullptr) {
        rfbClientErr("CRITICAL: frameBuffer allocation failed\n");
        return FALSE;
    }

    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    auto mid = env->GetMethodID(cls, "cbFramebufferSizeChanged", "(II)V");
    env->CallVoidMethod(obj, mid, width, height);

    return TRUE;
}

static void onGotCursorShape(rfbClient *client, int xHot, int yHot, int width, int height, int /*bytesPerPixel*/) {
    auto ex = getClientExtension(client);
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    LOCK(ex->mutex);
    updateCursor(ex->cursor, client->rcSource, client->rcMask, (uint16_t) width, (uint16_t) height,
                 (uint16_t) xHot, (uint16_t) yHot);
    UNLOCK(ex->mutex);

    jmethodID mid = env->GetMethodID(cls, "cbHandleCursorInfo", "(IIII)V");
    env->CallVoidMethod(obj, mid, width, height, xHot, yHot);
}

/**
 * Hooks callbacks to rfbClient.
 */
static void setCallbacks(rfbClient *client) {
    client->GetPassword = onGetPassword;
    client->GetCredential = onGetCredential;
    client->VerifyServerCertificate = onVerifyServerCertificate;
    client->Bell = onBell;
    client->GotXCutText = onGotXCutTextLatin1;
    client->GotXCutTextUTF8 = onGotXCutTextUTF8;
    client->HandleCursorPos = onHandleCursorPos;
    client->GotFrameBufferUpdate = onGotFrameBufferUpdate;
    client->FinishedFrameBufferUpdate = onFinishedFrameBufferUpdate;
    client->MallocFrameBuffer = onMallocFrameBuffer;
    client->GotCursorShape = onGotCursorShape;
}


/******************************************************************************
 * Apple RFB 3.889 Callbacks
 *****************************************************************************/

static bool appleGetCredential(void *userCtx, char *usernameOut, size_t usernameCap,
                               char *passwordOut, size_t passwordCap) {
    auto client = (rfbClient *) userCtx;
    rfbCredential *cred = getUserCredential(client);
    if (!cred) return false;

    const char *user = cred->userCredential.username ? cred->userCredential.username : "";
    const char *pass = cred->userCredential.password ? cred->userCredential.password : "";
    snprintf(usernameOut, usernameCap, "%s", user);
    snprintf(passwordOut, passwordCap, "%s", pass);

    free(cred->userCredential.username);
    free(cred->userCredential.password);
    free(cred);
    return true;
}

static uint8_t *appleResizeFramebuffer(void *userCtx, int width, int height) {
    auto client = (rfbClient *) userCtx;
    client->width = width;
    client->height = height;
    if (!onMallocFrameBuffer(client))
        return nullptr;
    return client->frameBuffer;
}

static void appleLockFramebuffer(void *userCtx) {
    auto ex = getClientExtension((rfbClient *) userCtx);
    LOCK(ex->mutex);
}

static void appleUnlockFramebuffer(void *userCtx) {
    auto ex = getClientExtension((rfbClient *) userCtx);
    UNLOCK(ex->mutex);
}

static void appleOnFramebufferUpdated(void *userCtx) {
    onFinishedFrameBufferUpdate((rfbClient *) userCtx);
}

static void appleOnFramebufferRect(void *userCtx, int y, int height) {
    auto client = (rfbClient *) userCtx;
    getClientExtension(client)->pendingRows.include(y, y + height, client->height);
}

static void appleOnCursorShape(void *userCtx, const uint8_t *bgra, uint16_t width, uint16_t height,
                               uint16_t xHot, uint16_t yHot) {
    auto client = (rfbClient *) userCtx;
    auto ex = getClientExtension(client);
    if (!ex->cursor)
        return;

    LOCK(ex->mutex);
    updateCursorBGRA(ex->cursor, bgra, width, height, xHot, yHot);
    UNLOCK(ex->mutex);

    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;
    jmethodID mid = env->GetMethodID(cls, "cbHandleCursorInfo", "(IIII)V");
    env->CallVoidMethod(obj, mid, (jint) width, (jint) height, (jint) xHot, (jint) yHot);
}

static void appleOnCursorPos(void *userCtx, int x, int y) {
    onHandleCursorPos((rfbClient *) userCtx, x, y);
}

static void appleOnDisplaysChanged(void *userCtx, const AppleDisplayLayout *layout,
                                   bool supportsSelection) {
    auto client = (rfbClient *) userCtx;
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    jint count = (jint) layout->count;
    if (count > APPLE_MAX_SELECTABLE_DISPLAYS)
        count = APPLE_MAX_SELECTABLE_DISPLAYS;
    jlongArray ids = env->NewLongArray(count);
    jintArray widths = env->NewIntArray(count);
    jintArray heights = env->NewIntArray(count);
    jintArray flags = env->NewIntArray(count);
    if (!ids || !widths || !heights || !flags) {
        if (ids) env->DeleteLocalRef(ids);
        if (widths) env->DeleteLocalRef(widths);
        if (heights) env->DeleteLocalRef(heights);
        if (flags) env->DeleteLocalRef(flags);
        return;
    }

    jlong idVals[APPLE_MAX_LAYOUT_DISPLAYS];
    jint wVals[APPLE_MAX_LAYOUT_DISPLAYS];
    jint hVals[APPLE_MAX_LAYOUT_DISPLAYS];
    jint flagVals[APPLE_MAX_LAYOUT_DISPLAYS];

    for (jint i = 0; i < count; ++i) {
        const AppleDisplayInfo &d = layout->displays[i];
        idVals[i] = (jlong) (uint64_t) d.id;
        wVals[i] = (jint) (d.logicalWidth > 0 ? d.logicalWidth : d.backingWidth);
        hVals[i] = (jint) (d.logicalHeight > 0 ? d.logicalHeight : d.backingHeight);
        flagVals[i] = (jint) d.flags;
    }

    env->SetLongArrayRegion(ids, 0, count, idVals);
    env->SetIntArrayRegion(widths, 0, count, wVals);
    env->SetIntArrayRegion(heights, 0, count, hVals);
    env->SetIntArrayRegion(flags, 0, count, flagVals);

    jlong currentId = (layout->currentDisplayId == APPLE_ALL_DISPLAYS_WIRE)
                      ? 0L
                      : (jlong) (uint64_t) layout->currentDisplayId;

    jmethodID mid = env->GetMethodID(cls, "cbDisplaysChanged", "(JZ[J[I[I[I)V");
    env->CallVoidMethod(obj, mid, currentId, (jboolean) supportsSelection, ids, widths, heights, flags);

    env->DeleteLocalRef(ids);
    env->DeleteLocalRef(widths);
    env->DeleteLocalRef(heights);
    env->DeleteLocalRef(flags);
}

static void appleOnKeyboardSourceChanged(void *userCtx, const char *sourceId,
                                         bool secureInput, bool supportsSelection) {
    auto client = (rfbClient *) userCtx;
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    jstring jSourceId = env->NewStringUTF(sourceId ? sourceId : "");
    if (!jSourceId) return;

    jmethodID mid = env->GetMethodID(cls, "cbKeyboardSourceChanged", "(Ljava/lang/String;ZZ)V");
    env->CallVoidMethod(obj, mid, jSourceId, (jboolean) secureInput, (jboolean) supportsSelection);
    env->DeleteLocalRef(jSourceId);
}

static void appleOnCutText(void *userCtx, const char *text, int len, bool isUtf8) {
    onGotXCutText((rfbClient *) userCtx, text, len, isUtf8);
}

static void appleOnBell(void *userCtx) {
    onBell((rfbClient *) userCtx);
}

static AppleRfbCallbacks makeAppleCallbacks() {
    AppleRfbCallbacks cbs{};
    cbs.getCredential = appleGetCredential;
    cbs.resizeFramebuffer = appleResizeFramebuffer;
    cbs.lockFramebuffer = appleLockFramebuffer;
    cbs.unlockFramebuffer = appleUnlockFramebuffer;
    cbs.onFramebufferUpdated = appleOnFramebufferUpdated;
    cbs.onFramebufferRect = appleOnFramebufferRect;
    cbs.onCursorShape = appleOnCursorShape;
    cbs.onCursorPos = appleOnCursorPos;
    cbs.onDisplaysChanged = appleOnDisplaysChanged;
    cbs.onKeyboardSourceChanged = appleOnKeyboardSourceChanged;
    cbs.onCutText = appleOnCutText;
    cbs.onBell = appleOnBell;
    cbs.logInfo = log_info;
    cbs.logError = log_error;
    return cbs;
}


/******************************************************************************
 * Native method Implementation
 *****************************************************************************/

extern "C"
JNIEXPORT jlong JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeClientCreate(JNIEnv *env, jobject thiz) {
    rfbClient *client = rfbGetClient(8, 3, 4);
    if (client == nullptr)
        return 0;

    auto ex = assignClientExtension(client);
    if (!ex)
        return 0;

    setCallbacks(client);
    client->canHandleNewFBSize = TRUE;

    //Attach reference to managed object
    auto obj = env->NewGlobalRef(thiz);
    setManagedClient(client, obj);

    return (jlong) client;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeConfigure(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr,
                                                   jint securityType, jboolean use_local_cursor, jint image_quality,
                                                   jboolean use_raw_encoding) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);

    ex->configuredSecurityType = securityType;
    ex->useLocalCursor = (use_local_cursor == JNI_TRUE);
    ex->imageQuality = image_quality;
    ex->useRawEncoding = (use_raw_encoding == JNI_TRUE);

    // 0 means all auth types
    if (securityType != 0) {
        uint32_t auth[1] = {static_cast<uint32_t>(securityType)};
        SetClientAuthSchemes(client, auth, 1);
    }

    if (use_local_cursor) {
        client->appData.useRemoteCursor = TRUE;
        ex->cursor = newCursor();
    }

    client->appData.qualityLevel = image_quality;
    if (use_raw_encoding)
        client->appData.encodingsString = "raw";

    // Change pixel format to match with the default format used by most VNC
    // servers. Technically, we should not have to this as VNC servers have to
    // respect whatever pixel format client prefers. But there are some wierd
    // servers which ignore pixel format sent by client. As a result, users
    // blame AVNC for not rendering the colors properly.
    // Note: This change affects how OpenGL Texture for framebuffer is rendered,
    // and it only works for 24 bit-per-pixel density.
    client->format.redShift = 16;
    client->format.greenShift = 8;
    client->format.blueShift = 0;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeConfigureAppleDisplay(JNIEnv * /*env*/, jobject /*thiz*/,
                                                               jlong client_ptr, jint display_mode,
                                                               jint initial_display) {
    auto ex = getClientExtension((rfbClient *) client_ptr);
    ex->appleDisplayMode = display_mode;
    ex->appleInitialDisplay = initial_display;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSetDest(JNIEnv *env, jobject /*thiz*/, jlong client_ptr,
                                                 jstring host, jint port) {
    auto client = (rfbClient *) client_ptr;
    client->destHost = getNativeStrCopy(env, host);
    client->destPort = port;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeInit(JNIEnv *env, jobject /*thiz*/, jlong client_ptr,
                                              jstring host, jint port, jstring certificateHost, jboolean authenticatedTunnel) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);

    free(ex->certificateHost);
    ex->certificateHost = getNativeStrCopy(env, certificateHost);
    ex->authenticatedTunnel = authenticatedTunnel == JNI_TRUE;
    ex->verifiedCertificate = false;
    ex->authenticationError = 0;
    client->serverHost = getNativeStrCopy(env, host);
    client->serverPort = port < 100 ? port + 5900 : port;

    if (!rfbClientConnect(client)) {
        return JNI_FALSE;
    }

    // Detect macOS Screen Sharing (RFB 003.889 / 003.888) via MSG_PEEK without
    // consuming bytes, so non-Apple VNC servers proceed on the exact same socket.
    bool allowApple = !client->destHost &&
                      (ex->configuredSecurityType == 0 || ex->configuredSecurityType == 30);
    if (allowApple && appleRfbPeekIsAppleBanner(client->sock, 5000) == 1) {
        AppleRfbConfig cfg{};
        cfg.authenticatedTunnel = ex->authenticatedTunnel;
        cfg.securityType = ex->configuredSecurityType;
        cfg.imageQuality = ex->imageQuality;
        cfg.useRawEncoding = ex->useRawEncoding;
        cfg.useLocalCursor = ex->useLocalCursor;
        cfg.viewOnly = ex->viewOnly;
        cfg.displayMode = (AppleDisplayMode) ex->appleDisplayMode;
        cfg.initialDisplayIndex = ex->appleInitialDisplay;
        cfg.virtualWidth = 1920;
        cfg.virtualHeight = 1080;
        cfg.virtualDensity = 1.0;

        AppleRfbCallbacks cbs = makeAppleCallbacks();
        AppleRfbClient *appleClient = nullptr;
        int err = 0;
        AppleRfbConnectResult res = appleRfbConnect(client->sock, &cfg, &cbs, client, &appleClient, &err);

        if (res == APPLE_RFB_OK) {
            ex->appleClient = appleClient;
            client->serverMajor = 3;
            client->serverMinor = 889;
            return JNI_TRUE;
        }
        if (res == APPLE_RFB_ERROR) {
            errno = err;
            return JNI_FALSE;
        }

        // APPLE_RFB_FALLBACK: e.g. server only offered VncAuth (type 2).
        // Reconnect cleanly and hand off to standard LibVNCClient.
        if (client->sock != RFB_INVALID_SOCKET) {
            close(client->sock);
            client->sock = RFB_INVALID_SOCKET;
        }
        if (!rfbClientConnect(client)) {
            return JNI_FALSE;
        }
    }

    if (rfbClientInitialise(client)) {
        return JNI_TRUE;
    }

    if (ex->authenticationError) errno = ex->authenticationError;
    return JNI_FALSE;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeCleanup(JNIEnv *env, jobject /*thiz*/,
                                                 jlong client_ptr) {
    auto client = (rfbClient *) client_ptr;

    if (client->frameBuffer) {
        free(client->frameBuffer);
        client->frameBuffer = nullptr;
    }

    auto managedClient = getManagedClient(client);
    env->DeleteGlobalRef(managedClient);

    freeClientExtension(client);
    rfbClientCleanup(client);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeProcessServerMessage(JNIEnv * /*env*/, jobject /*thiz*/,
                                                              jlong client_ptr) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);

    if (ex->appleClient) {
        auto waitResult = appleRfbWaitForMessage(ex->appleClient, 250000);
        if (waitResult == 0)
            return JNI_TRUE;
        if (waitResult > 0 && appleRfbHandleServerMessage(ex->appleClient))
            return JNI_TRUE;
        return JNI_FALSE;
    }

    auto waitResult = WaitForMessage(client, 1000000);

    if (waitResult == 0) // Timeout
        return JNI_TRUE;

    if (waitResult > 0 && HandleRFBServerMessage(client))
        return JNI_TRUE;

    return JNI_FALSE;
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeGetLastErrorStr(JNIEnv *env, jobject/*thiz*/) {
    auto str = errnoToStr(errno);
    return env->NewStringUTF(str);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSendKeyEvent(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr,
                                                      jint key_sym, jint xt_code, jboolean is_down) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);
    rfbBool down = is_down ? TRUE : FALSE;

    // Remap Alt key to Meta key for MacOS
    auto isServerMacOS = client->serverMajor == 3 && client->serverMinor == 889;
    if (isServerMacOS) {
        if (key_sym == 0xffe9  /* Left alt  */) key_sym = 0xffe7  /* Left meta  */;
        if (key_sym == 0xffea  /* Right alt */) key_sym = 0xffe8  /* Right meta */;
    }

    if (ex->appleClient) {
        return appleRfbSendKeyEvent(ex->appleClient, (uint32_t) key_sym, is_down == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
    }

    if (xt_code > 0 && SendExtendedKeyEvent(client, key_sym, xt_code, down))
        return JNI_TRUE;
    else
        return SendKeyEvent(client, key_sym, down);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSendPointerEvent(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr,
                                                          jint x, jint y, jint mask) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);
    if (ex->appleClient) {
        return appleRfbSendPointerEvent(ex->appleClient, x, y, (uint8_t) mask) ? JNI_TRUE : JNI_FALSE;
    }
    return (jboolean) SendPointerEvent(client, x, y, mask);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSendCutText(JNIEnv *env, jobject /*thiz*/, jlong client_ptr, jbyteArray bytes,
                                                     jboolean is_utf8) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);
    auto textBuffer = env->GetByteArrayElements(bytes, nullptr);
    auto textLen = env->GetArrayLength(bytes);
    auto textChars = reinterpret_cast<char *>(textBuffer);

    rfbBool result = FALSE;
    if (ex->appleClient) {
        result = appleRfbSendCutText(ex->appleClient, textChars, textLen, is_utf8 == JNI_TRUE) ? TRUE : FALSE;
    } else {
        result = is_utf8
                 ? SendClientCutTextUTF8(client, textChars, textLen)
                 : SendClientCutText(client, textChars, textLen);
    }

    env->ReleaseByteArrayElements(bytes, textBuffer, JNI_ABORT);
    return (jboolean) result;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeIsUTF8CutTextSupported(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);
    if (ex->appleClient) {
        return appleRfbSupportsUtf8Pasteboard(ex->appleClient) ? JNI_TRUE : JNI_FALSE;
    }
    return (jboolean) (client->extendedClipboardServerCapabilities != 0);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeIsAppleKeyboardSyncSupported(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr) {
    auto ex = getClientExtension((rfbClient *) client_ptr);
    if (!ex->appleClient) return JNI_FALSE;
    return appleRfbSupportsKeyboardSync(ex->appleClient) ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSelectKeyboardSource(JNIEnv *env, jobject /*thiz*/,
                                                              jlong client_ptr, jstring source_id,
                                                              jboolean clear_after) {
    auto ex = getClientExtension((rfbClient *) client_ptr);
    if (!ex->appleClient || !source_id) return JNI_FALSE;
    auto idStr = env->GetStringUTFChars(source_id, nullptr);
    if (!idStr) return JNI_FALSE;
    bool ok = appleRfbSelectKeyboardSource(ex->appleClient, idStr, clear_after == JNI_TRUE);
    env->ReleaseStringUTFChars(source_id, idStr);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeToggleInputSource(JNIEnv * /*env*/, jobject /*thiz*/,
                                                           jlong client_ptr) {
    auto ex = getClientExtension((rfbClient *) client_ptr);
    if (!ex->appleClient) return JNI_FALSE;
    return appleRfbToggleInputSource(ex->appleClient) ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSendTextDiff(JNIEnv *env, jobject /*thiz*/,
                                                      jlong client_ptr, jint backspaces,
                                                      jbyteArray utf8_bytes) {
    auto ex = getClientExtension((rfbClient *) client_ptr);
    if (!ex->appleClient) return JNI_FALSE;
    if (!utf8_bytes) {
        return appleRfbSendTextDiff(ex->appleClient, backspaces, nullptr, 0) ? JNI_TRUE : JNI_FALSE;
    }
    auto buf = env->GetByteArrayElements(utf8_bytes, nullptr);
    auto len = env->GetArrayLength(utf8_bytes);
    bool ok = appleRfbSendTextDiff(ex->appleClient, backspaces, reinterpret_cast<const char *>(buf), len);
    env->ReleaseByteArrayElements(utf8_bytes, buf, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSendUnicodeText(JNIEnv *env, jobject /*thiz*/,
                                                         jlong client_ptr, jbyteArray utf8_bytes) {
    auto ex = getClientExtension((rfbClient *) client_ptr);
    if (!ex->appleClient || !utf8_bytes) return JNI_FALSE;
    auto buf = env->GetByteArrayElements(utf8_bytes, nullptr);
    auto len = env->GetArrayLength(utf8_bytes);
    bool ok = appleRfbSendUnicodeText(ex->appleClient, reinterpret_cast<const char *>(buf), len);
    env->ReleaseByteArrayElements(utf8_bytes, buf, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativePasteText(JNIEnv *env, jobject /*thiz*/,
                                                   jlong client_ptr, jbyteArray utf8_bytes) {
    auto ex = getClientExtension((rfbClient *) client_ptr);
    if (!ex->appleClient || !utf8_bytes) return JNI_FALSE;
    auto buf = env->GetByteArrayElements(utf8_bytes, nullptr);
    auto len = env->GetArrayLength(utf8_bytes);
    bool ok = appleRfbPasteText(ex->appleClient, reinterpret_cast<const char *>(buf), len);
    env->ReleaseByteArrayElements(utf8_bytes, buf, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeRestoreClipboard(JNIEnv * /*env*/, jobject /*thiz*/,
                                                          jlong client_ptr) {
    auto ex = getClientExtension((rfbClient *) client_ptr);
    if (!ex->appleClient) return JNI_FALSE;
    return appleRfbRestoreClipboard(ex->appleClient) ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSetDesktopSize(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr,
                                                        jint width, jint height) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);
    if (ex->appleClient) {
        return appleRfbSetDesktopSize(ex->appleClient, width, height) ? JNI_TRUE : JNI_FALSE;
    }
    return (jboolean) SendExtDesktopSize(client, width, height);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSelectDisplay(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr,
                                                       jlong display_id) {
    auto ex = getClientExtension((rfbClient *) client_ptr);
    if (!ex->appleClient) return JNI_FALSE;
    return appleRfbSelectDisplay(ex->appleClient, (uint32_t) display_id) ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSetViewOnly(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr,
                                                     jboolean view_only) {
    auto ex = getClientExtension((rfbClient *) client_ptr);
    ex->viewOnly = (view_only == JNI_TRUE);
    if (ex->appleClient) {
        appleRfbSetViewOnly(ex->appleClient, ex->viewOnly);
    }
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeRefreshFrameBuffer(JNIEnv * /*env*/, jobject /*thiz*/, jlong clientPtr) {
    auto client = (rfbClient *) clientPtr;
    auto ex = getClientExtension(client);
    if (ex->appleClient) {
        return appleRfbRefreshFramebuffer(ex->appleClient) ? JNI_TRUE : JNI_FALSE;
    }
    return (jboolean) SendFramebufferUpdateRequest(client, 0, 0, client->width, client->height, TRUE);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativePauseFramebufferUpdates(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr,
                                                                 jboolean pause) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);
    client->pauseFramebufferUpdates = pause;
    if (ex->appleClient) {
        appleRfbSetPaused(ex->appleClient, pause == JNI_TRUE);
    }
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeGetDesktopName(JNIEnv *env, jobject /*thiz*/, jlong client_ptr) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);
    if (ex->appleClient) {
        return env->NewStringUTF(appleRfbGetDesktopName(ex->appleClient));
    }
    return env->NewStringUTF(client->desktopName ? client->desktopName : "");
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeGetWidth(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);
    if (ex->appleClient) return appleRfbGetWidth(ex->appleClient);
    return client->width;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeGetHeight(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);
    if (ex->appleClient) return appleRfbGetHeight(ex->appleClient);
    return client->height;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeIsEncrypted(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);
    if (ex->appleClient) {
        return appleRfbIsEncrypted(ex->appleClient) ? JNI_TRUE : JNI_FALSE;
    }
    return static_cast<jboolean>(client->tlsSession ? JNI_TRUE : JNI_FALSE);
}

extern "C"
JNIEXPORT jlong JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeUploadFrameTexture(JNIEnv * /*env*/, jobject /*thiz*/,
                                                            jlong client_ptr, jlong uploadedRevision) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);

    LOCK(ex->mutex);
    const jlong revision = ex->framebufferTexture.upload(client->frameBuffer, uploadedRevision);
    UNLOCK(ex->mutex);
    return revision;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeUploadCursorTexture(JNIEnv * /*env*/, jobject /*thiz*/, jlong client_ptr) {
    auto client = (rfbClient *) client_ptr;
    auto ex = getClientExtension(client);
    jboolean uploaded = JNI_FALSE;

    LOCK(ex->mutex);
    auto cursor = ex->cursor;
    if (cursor && cursor->buffer && cursor->width && cursor->height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cursor->width, cursor->height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, cursor->buffer);
        uploaded = glGetError() == GL_NO_ERROR;
    }
    UNLOCK(ex->mutex);
    return uploaded;
}
