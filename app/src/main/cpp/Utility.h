/*
 * Copyright (c) 2022  Gaurav Ujjwal.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

#ifndef AVNC_UTILITY_H
#define AVNC_UTILITY_H

#include <stdarg.h>
#include <netdb.h>
#include <errno.h>
#include <android/log.h>


/******************************************************************************
 * Utilities
 *****************************************************************************/

/**
 * Returns a native copy of the given jstring.
 * Caller is responsible for releasing the memory.
 */
static char *getNativeStrCopy(JNIEnv *env, jstring jStr) {
    const char *cStr = env->GetStringUTFChars(jStr, nullptr);
    char *str = strdup(cStr);
    env->ReleaseStringUTFChars(jStr, cStr);
    return str;
}


/******************************************************************************
 * Logging
 *****************************************************************************/
/**
 * NewStringUTF() requires Modified UTF-8 and may abort or misread on anything else.
 * Names sent by a server are untrusted, so they are decoded here with replacement.
 */
static jstring newStringFromUntrustedUtf8(JNIEnv *env, const char *str) {
    const auto *s = (const unsigned char *) (str ? str : "");
    size_t len = strlen((const char *) s);
    if (len > 1024) len = 1024;

    jchar out[1024];
    jsize n = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0xFFFD;
        size_t need = 0;
        unsigned char b = s[i];
        if (b < 0x80) { cp = b; need = 0; }
        else if (b >= 0xC2 && b <= 0xDF) { cp = b & 0x1Fu; need = 1; }
        else if (b >= 0xE0 && b <= 0xEF) { cp = b & 0x0Fu; need = 2; }
        else if (b >= 0xF0 && b <= 0xF4) { cp = b & 0x07u; need = 3; }
        else { need = SIZE_MAX; }

        bool valid = need != SIZE_MAX && i + need < len;
        for (size_t k = 1; valid && k <= need; ++k) {
            if ((s[i + k] & 0xC0u) != 0x80u) valid = false;
            else cp = (cp << 6) | (s[i + k] & 0x3Fu);
        }
        if (valid && ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
                      (cp >= 0xD800 && cp <= 0xDFFF)))
            valid = false;

        if (!valid) {
            out[n++] = 0xFFFD;
            i += 1;
        } else if (cp >= 0x10000) {
            if (n + 2 > 1024) break;
            cp -= 0x10000;
            out[n++] = (jchar) (0xD800 + (cp >> 10));
            out[n++] = (jchar) (0xDC00 + (cp & 0x3FFu));
            i += need + 1;
        } else {
            out[n++] = (jchar) (cp < 0x20 ? 0xFFFD : cp);
            i += need + 1;
        }
    }
    return env->NewString(out, n);
}

const char *LOG_TAG = "NativeVnc";

void log_info(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, fmt, args);
    va_end(args);
}

void log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    __android_log_vprint(ANDROID_LOG_ERROR, LOG_TAG, fmt, args);
    va_end(args);
}

/**
 * Converts given errno value to its description.
 */
static const char *errnoToStr(int e) {

    // LibVNC is patched to report `getaddrinfo` errors as negative 'errno'.
    // See ConnectClientToTcpAddr6WithTimeout() in sockets.c
    if (e < -1000) {
        return gai_strerror((-e) - 1000);
    }

    switch (e) {
        case ENETDOWN:
        case ENETRESET:
        case ENETUNREACH:
        case EHOSTDOWN:
        case EHOSTUNREACH:
        case ETIMEDOUT:
        case ENOMEM:
        case EPROTO:
        case EIO:
            return strerror(e);

        case ECONNREFUSED:
            return "VNC Server is not running, or port number is incorrect";

        case ECONNABORTED:
            return "Connection aborted";

        case ECONNRESET:
            return "Connection closed abruptly by remote host";

        case EACCES:
            return "Authentication failed";
        case EPROTONOSUPPORT:
            return "This account login requires a verified SSH tunnel, X.509 TLS, or Apple SRP.";
        case EBADMSG:
            return "Server certificate verification failed. Check the hostname, validity and saved fingerprint.";
        case EOVERFLOW:
            return "Remote framebuffer exceeds the supported size.";

        case 0:
            return "Connection closed by server";

        default:
            // In this case we don't want to display errno description to user
            // because it is more likely to be misleading (e.g. EINTR, EAGAIN).
            // BUT add it to logs in case LibVNC didn't.
            log_error("errnoToStr: (%d %s)", errno, strerror(errno));
            return "";
    }
}

#endif //AVNC_UTILITY_H
