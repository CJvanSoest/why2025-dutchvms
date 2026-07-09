/*
 * BadgeVMS crypto wrapper — mbedtls-backed one-shot primitives.
 *
 * MeshCore uses AES-128 ECB (per block) for both group-chat and DM ciphertext,
 * with HMAC-SHA256 truncated to 2 bytes as integrity check.
 *
 * Pointer-translation note: ESP32 hardware AES goes through DMA, which needs
 * physical addresses. PIE ELF apps pass virtual addresses that DMA cannot
 * resolve, so we copy all in/out buffers through firmware-local stack
 * allocations before calling mbedtls.
 */
#include "badgevms/crypto.h"

#include "esp_log.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include <stdio.h>

#include <string.h>

static char const TAG[] = "bv_crypto";

static void log_hex(char const *label, uint8_t const *buf, size_t len) {
    char   hex[3 * 16 + 1];
    size_t pos = 0;
    size_t n   = len < 16 ? len : 16;
    for (size_t i = 0; i < n; i++) {
        snprintf(hex + pos, sizeof(hex) - pos, "%02x ", buf[i]);
        pos += 3;
    }
    if (pos > 0)
        hex[pos - 1] = '\0';
    ESP_LOGD(TAG, "  %-8s %s", label, hex);
}

int bv_aes128_ecb_decrypt_block(uint8_t const key[16], uint8_t const in[16], uint8_t out[16]) {
    uint8_t local_key[16];
    uint8_t local_in[16];
    uint8_t local_out[16];
    memcpy(local_key, key, 16);
    memcpy(local_in, in, 16);

    ESP_LOGD(TAG, "aes_decrypt_block:");
    log_hex("key", local_key, 16);
    log_hex("cipher", local_in, 16);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int rc = mbedtls_aes_setkey_dec(&ctx, local_key, 128);
    if (rc == 0) {
        rc = mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, local_in, local_out);
    }
    mbedtls_aes_free(&ctx);

    log_hex("plain", local_out, 16);
    ESP_LOGD(TAG, "  rc=%d", rc);

    memcpy(out, local_out, 16);
    return rc;
}

int bv_aes128_ecb_encrypt_block(uint8_t const key[16], uint8_t const in[16], uint8_t out[16]) {
    uint8_t local_key[16];
    uint8_t local_in[16];
    uint8_t local_out[16];
    memcpy(local_key, key, 16);
    memcpy(local_in, in, 16);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int rc = mbedtls_aes_setkey_enc(&ctx, local_key, 128);
    if (rc == 0) {
        rc = mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, local_in, local_out);
    }
    mbedtls_aes_free(&ctx);

    memcpy(out, local_out, 16);
    return rc;
}

int bv_sha256(uint8_t const *data, size_t len, uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int rc = mbedtls_sha256_starts(&ctx, 0);
    if (rc == 0)
        rc = mbedtls_sha256_update(&ctx, data, len);
    if (rc == 0)
        rc = mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
    return rc;
}

int bv_hmac_sha256(uint8_t const *key, size_t key_len, uint8_t const *data, size_t data_len, uint8_t out[32]) {
    ESP_LOGD(TAG, "hmac_sha256: key_len=%u data_len=%u", (unsigned)key_len, (unsigned)data_len);
    log_hex("key", key, key_len);
    log_hex("data", data, data_len);

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_info_t const *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int                      rc   = mbedtls_md_setup(&ctx, info, 1);
    if (rc == 0)
        rc = mbedtls_md_hmac_starts(&ctx, key, key_len);
    if (rc == 0)
        rc = mbedtls_md_hmac_update(&ctx, data, data_len);
    if (rc == 0)
        rc = mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);

    log_hex("mac", out, 32);
    ESP_LOGD(TAG, "  rc=%d", rc);
    return rc;
}
