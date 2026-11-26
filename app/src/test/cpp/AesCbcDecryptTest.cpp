/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host test (arm64): the hardware AES-128-CBC decryptor must match OpenSSL byte for byte.
 * Build: clang++ -std=c++17 -O2 -I app/src/main/cpp -I $(brew --prefix openssl@3)/include \
 *        app/src/test/cpp/AesCbcDecryptTest.cpp -L $(brew --prefix openssl@3)/lib -lcrypto -o /tmp/aes-test
 */

#include "AesCbcDecrypt.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main() {
    if (!aesCbcHardwareAvailable()) {
        puts("AesCbcDecryptTest: skipped (no AES instructions)");
        return 0;
    }

    // FIPS-197 style known answer: one block, zero IV.
    {
        const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
        uint8_t block[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
        const uint8_t plain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
        const uint8_t iv[16] = {0};
        AesCbcDecryptor d;
        aesCbcDecryptInit(&d, key, iv);
        aesCbcDecrypt(&d, block, 16);
        if (memcmp(block, plain, 16) != 0) {
            puts("FAILED: known answer");
            return 1;
        }
    }

    // Random keys and record sizes, with the chain carried across records as the record layer does.
    srand(7);
    for (int round = 0; round < 300; ++round) {
        uint8_t key[16], iv[16];
        for (int i = 0; i < 16; ++i) { key[i] = (uint8_t) rand(); iv[i] = (uint8_t) rand(); }

        EVP_CIPHER_CTX *enc = EVP_CIPHER_CTX_new(), *ref = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(enc, EVP_aes_128_cbc(), nullptr, key, iv);
        EVP_CIPHER_CTX_set_padding(enc, 0);
        EVP_DecryptInit_ex(ref, EVP_aes_128_cbc(), nullptr, key, iv);
        EVP_CIPHER_CTX_set_padding(ref, 0);
        AesCbcDecryptor d;
        aesCbcDecryptInit(&d, key, iv);

        for (int record = 0; record < 12; ++record) {
            size_t len = 16u * (size_t) (1 + rand() % 4095); // up to the 65520-byte record limit
            uint8_t *plain = (uint8_t *) malloc(len), *wire = (uint8_t *) malloc(len), *expect = (uint8_t *) malloc(len);
            for (size_t i = 0; i < len; ++i) plain[i] = (uint8_t) rand();
            int n = 0;
            EVP_EncryptUpdate(enc, wire, &n, plain, (int) len);
            EVP_DecryptUpdate(ref, expect, &n, wire, (int) len);
            aesCbcDecrypt(&d, wire, len);
            bool ok = memcmp(wire, expect, len) == 0 && memcmp(wire, plain, len) == 0;
            free(plain); free(wire); free(expect);
            if (!ok) {
                printf("FAILED: round %d record %d len %zu\n", round, record, len);
                return 1;
            }
        }
        EVP_CIPHER_CTX_free(enc);
        EVP_CIPHER_CTX_free(ref);
    }

    // Throughput on this machine.
    {
        const size_t len = 65520, loops = 4000;
        uint8_t *buf = (uint8_t *) calloc(1, len);
        uint8_t key[16] = {1}, iv[16] = {2};
        AesCbcDecryptor d;
        aesCbcDecryptInit(&d, key, iv);
        double t0 = now();
        for (size_t i = 0; i < loops; ++i) aesCbcDecrypt(&d, buf, len);
        double dt = now() - t0;
        printf("hardware AES-128-CBC decrypt: %.0f MB/s\n", len * loops / dt / 1e6);
        free(buf);
    }

    puts("AesCbcDecryptTest: all passed");
    return 0;
}
