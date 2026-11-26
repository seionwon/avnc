/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AVNC_AES_CBC_DECRYPT_H
#define AVNC_AES_CBC_DECRYPT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * AES-128-CBC decryption on the CPU's AES instructions.
 *
 * Every byte a macOS server sends passes through this cipher, and the bundled crypto
 * library only has a portable C implementation of it. Callers must check
 * aesCbcHardwareAvailable() and keep their software path for CPUs without the instructions.
 */

#if defined(__aarch64__)

#include <arm_neon.h>

#if defined(__ANDROID__) || defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_AES
#define HWCAP_AES (1 << 3)
#endif
static inline bool aesCbcHardwareAvailable() {
    return (getauxval(AT_HWCAP) & HWCAP_AES) != 0;
}
#elif defined(__APPLE__)
static inline bool aesCbcHardwareAvailable() { return true; } // Every Apple arm64 CPU has them
#else
static inline bool aesCbcHardwareAvailable() { return false; }
#endif

#define AVNC_AES_TARGET __attribute__((target("aes")))

struct AesCbcDecryptor {
    uint8_t roundKeys[11][16]; // Equivalent inverse cipher schedule, first round first
    uint8_t iv[16];            // Last ciphertext block seen; the chain continues across calls
};

AVNC_AES_TARGET
static inline uint32_t aesSubWord(uint32_t word) {
    // With four equal columns ShiftRows changes nothing, leaving SubBytes of the word.
    uint8x16_t v = vreinterpretq_u8_u32(vdupq_n_u32(word));
    v = vaeseq_u8(v, vdupq_n_u8(0));
    return vgetq_lane_u32(vreinterpretq_u32_u8(v), 0);
}

AVNC_AES_TARGET
static inline void aesCbcDecryptInit(AesCbcDecryptor *d, const uint8_t key[16], const uint8_t iv[16]) {
    static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    uint32_t w[44]; // Little-endian words, so byte 0 of a column is the low byte
    memcpy(w, key, 16);
    for (int i = 4; i < 44; ++i) {
        uint32_t t = w[i - 1];
        if (i % 4 == 0)
            t = aesSubWord((t >> 8) | (t << 24)) ^ rcon[i / 4 - 1];
        w[i] = w[i - 4] ^ t;
    }

    memcpy(d->roundKeys[0], &w[40], 16);
    for (int round = 1; round < 10; ++round) {
        uint8x16_t k = vld1q_u8((const uint8_t *) &w[(10 - round) * 4]);
        vst1q_u8(d->roundKeys[round], vaesimcq_u8(k));
    }
    memcpy(d->roundKeys[10], &w[0], 16);
    memcpy(d->iv, iv, 16);
    memset(w, 0, sizeof(w));
}

/** Decrypts [len] bytes in place. [len] must be a multiple of 16. */
AVNC_AES_TARGET
static inline void aesCbcDecrypt(AesCbcDecryptor *d, uint8_t *data, size_t len) {
    uint8x16_t rk[11];
    for (int i = 0; i < 11; ++i) rk[i] = vld1q_u8(d->roundKeys[i]);
    uint8x16_t prev = vld1q_u8(d->iv);

    // Blocks decrypt independently in CBC, so four run through the pipeline together.
    while (len >= 64) {
        uint8x16_t c0 = vld1q_u8(data), c1 = vld1q_u8(data + 16);
        uint8x16_t c2 = vld1q_u8(data + 32), c3 = vld1q_u8(data + 48);
        uint8x16_t b0 = c0, b1 = c1, b2 = c2, b3 = c3;
        for (int i = 0; i < 9; ++i) {
            b0 = vaesimcq_u8(vaesdq_u8(b0, rk[i]));
            b1 = vaesimcq_u8(vaesdq_u8(b1, rk[i]));
            b2 = vaesimcq_u8(vaesdq_u8(b2, rk[i]));
            b3 = vaesimcq_u8(vaesdq_u8(b3, rk[i]));
        }
        b0 = veorq_u8(vaesdq_u8(b0, rk[9]), rk[10]);
        b1 = veorq_u8(vaesdq_u8(b1, rk[9]), rk[10]);
        b2 = veorq_u8(vaesdq_u8(b2, rk[9]), rk[10]);
        b3 = veorq_u8(vaesdq_u8(b3, rk[9]), rk[10]);
        vst1q_u8(data, veorq_u8(b0, prev));
        vst1q_u8(data + 16, veorq_u8(b1, c0));
        vst1q_u8(data + 32, veorq_u8(b2, c1));
        vst1q_u8(data + 48, veorq_u8(b3, c2));
        prev = c3;
        data += 64;
        len -= 64;
    }
    while (len >= 16) {
        uint8x16_t c = vld1q_u8(data);
        uint8x16_t b = c;
        for (int i = 0; i < 9; ++i)
            b = vaesimcq_u8(vaesdq_u8(b, rk[i]));
        b = veorq_u8(vaesdq_u8(b, rk[9]), rk[10]);
        vst1q_u8(data, veorq_u8(b, prev));
        prev = c;
        data += 16;
        len -= 16;
    }
    vst1q_u8(d->iv, prev);
}

#undef AVNC_AES_TARGET

#else // !__aarch64__

struct AesCbcDecryptor {
    uint8_t unused;
};
static inline bool aesCbcHardwareAvailable() { return false; }
static inline void aesCbcDecryptInit(AesCbcDecryptor *, const uint8_t *, const uint8_t *) {}
static inline void aesCbcDecrypt(AesCbcDecryptor *, uint8_t *, size_t) {}

#endif

#endif // AVNC_AES_CBC_DECRYPT_H
