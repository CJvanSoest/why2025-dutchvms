/*
 * BadgeVMS crypto wrapper API.
 * Backed by mbedtls in the firmware. Exposed to ELF apps as simple
 * one-shot primitives so apps don't need mbedtls headers.
 *
 * Primitives are MeshCore-shaped: AES-128 ECB block ops + HMAC-SHA256.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BV_AES_BLOCK_SIZE 16

/*
 * AES-128 ECB decrypt a single 16-byte block.
 * key:  16 bytes
 * in:   16 bytes ciphertext
 * out:  16 bytes plaintext (may equal in for in-place)
 * Returns 0 on success.
 */
int bv_aes128_ecb_decrypt_block(uint8_t const key[16],
                                uint8_t const in[16],
                                uint8_t out[16]);

/*
 * AES-128 ECB encrypt a single 16-byte block.
 */
int bv_aes128_ecb_encrypt_block(uint8_t const key[16],
                                uint8_t const in[16],
                                uint8_t out[16]);

/*
 * SHA-256 one-shot.
 * Returns 0 on success.
 */
int bv_sha256(uint8_t const *data, size_t len, uint8_t out[32]);

/*
 * HMAC-SHA256 one-shot.
 * key:  any length
 * out:  32 bytes
 * Returns 0 on success.
 */
int bv_hmac_sha256(uint8_t const *key, size_t key_len,
                   uint8_t const *data, size_t data_len,
                   uint8_t out[32]);

#ifdef __cplusplus
}
#endif
