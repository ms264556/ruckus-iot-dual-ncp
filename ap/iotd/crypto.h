/* The primitives the ESPHome native API's Noise transport needs (Noise_NNpsk0_25519_ChaChaPoly_SHA256):
 * SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104), ChaCha20-Poly1305 (RFC 8439) and X25519 (RFC 7748).
 * Small portable C, no tables beyond SHA-256's constants; test/crypto_test.py checks them against Python's
 * cryptography package.
 */
#ifndef IOTD_CRYPTO_H
#define IOTD_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN 32
#define AEAD_KEY_LEN 32
#define AEAD_NONCE_LEN 12
#define AEAD_TAG_LEN 16
#define X25519_LEN 32

struct sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
};
void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t n);
void sha256_final(struct sha256 *s, uint8_t out[SHA256_LEN]);
void sha256(const void *data, size_t n, uint8_t out[SHA256_LEN]);

/* HMAC-SHA256 of the concatenation of two inputs (either may be empty). */
void hmac_sha256(const uint8_t *key, size_t key_len, const void *a, size_t a_len, const void *b, size_t b_len,
                 uint8_t out[SHA256_LEN]);

/* RFC 8439 AEAD. out holds n bytes of ciphertext followed by the 16-byte tag. */
void aead_encrypt(const uint8_t key[AEAD_KEY_LEN], const uint8_t nonce[AEAD_NONCE_LEN], const uint8_t *ad,
                  size_t ad_len, const uint8_t *in, size_t n, uint8_t *out);
/* in is n bytes of ciphertext followed by the tag; false (out untouched) when the tag does not verify. */
bool aead_decrypt(const uint8_t key[AEAD_KEY_LEN], const uint8_t nonce[AEAD_NONCE_LEN], const uint8_t *ad,
                  size_t ad_len, const uint8_t *in, size_t n, uint8_t *out);

/* out = scalar * point (both 32 bytes, RFC 7748 encoding); point NULL means the base point 9. */
void x25519(uint8_t out[X25519_LEN], const uint8_t scalar[X25519_LEN], const uint8_t *point);

/* n bytes from /dev/urandom; false when it cannot be read. */
bool random_bytes(uint8_t *out, size_t n);

/* Constant-time comparison. */
bool equal_ct(const uint8_t *a, const uint8_t *b, size_t n);

#endif
