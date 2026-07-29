#include "fcrypto.h"

#include <stdio.h>
#include <string.h>

#include "monocypher.h"

void fcrypto_derive_key(const char *passphrase, size_t len, uint8_t out[FCRYPTO_KEY_LEN]) {
    /* A one-shot unkeyed BLAKE2b hash of the passphrase. This is not a
     * password KDF (no iteration, no salt), which is fine for what this
     * protects: the key does not gate access to anything stored, it is the
     * shared secret two boards must independently derive from the same typed
     * words. Argon2 exists in Monocypher for the very different job of
     * slowing down guessing against a stored, attacker-visible verifier. */
    crypto_blake2b(out, FCRYPTO_KEY_LEN, (const uint8_t *)passphrase, len);
}

void fcrypto_key_fingerprint(const uint8_t key[FCRYPTO_KEY_LEN], char *out, size_t cap) {
    uint8_t h[4];
    /* Keyed so the fingerprint cannot be recomputed from a passphrase guess
     * without also trying every guess through this same construction, and
     * domain separated from fcrypto_derive_key's output so this hash reveals
     * nothing about the key itself beyond "two boards typed the same thing". */
    crypto_blake2b_keyed(h, sizeof h, key, FCRYPTO_KEY_LEN,
                         (const uint8_t *)"fcontrol-fp", 11);
    snprintf(out, cap, "%02x%02x%02x%02x", h[0], h[1], h[2], h[3]);
}

void fcrypto_encrypt(const uint8_t key[FCRYPTO_KEY_LEN], const uint8_t nonce[FCRYPTO_NONCE_LEN],
                     const uint8_t *plain, size_t plain_len,
                     uint8_t *cipher_out, uint8_t tag_out[FCRYPTO_TAG_LEN]) {
    crypto_aead_lock(cipher_out, tag_out, key, nonce, NULL, 0, plain, plain_len);
}

bool fcrypto_decrypt(const uint8_t key[FCRYPTO_KEY_LEN], const uint8_t nonce[FCRYPTO_NONCE_LEN],
                     const uint8_t *cipher, size_t cipher_len,
                     const uint8_t tag[FCRYPTO_TAG_LEN], uint8_t *plain_out) {
    /* crypto_aead_unlock writes nothing meaningful to plain_out on failure by
     * its own contract, but the caller should never see stale bytes from a
     * previous call either. */
    if (crypto_aead_unlock(plain_out, tag, key, nonce, NULL, 0, cipher, cipher_len) != 0) {
        if (cipher_len > 0) memset(plain_out, 0, cipher_len);
        return false;
    }
    return true;
}
