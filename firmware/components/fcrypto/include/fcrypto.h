/* Simple symmetric encryption for v0.1.
 *
 * This is deliberately not the Noise XX handshake the design spec describes.
 * That needs a per-contact key exchange, forward secrecy, and identity keys
 * that are actually signed, none of which exist yet, and doing them fast is
 * exactly how a privacy tool gets someone hurt.
 *
 * What this is instead: every board on the same "network" holds one shared
 * 32 byte key, entered as a passphrase in the flasher and written to every
 * board that should be able to talk to the others. Every message is sealed
 * with that key under XChaCha20-Poly1305. A passive listener without the key
 * reads nothing. Anyone who HAS the key, which includes everyone else on that
 * network, can read everything and sign nothing, so it does not stop one
 * member of the network from reading another's traffic or claiming to be
 * someone they are not. Say this plainly wherever the key exists in the UI.
 */
#ifndef FCRYPTO_H
#define FCRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FCRYPTO_KEY_LEN   32
#define FCRYPTO_NONCE_LEN 24
#define FCRYPTO_TAG_LEN   16

/** Deterministic: the same passphrase always derives the same key, on every
 *  board, with no coordination beyond typing the same words. */
void fcrypto_derive_key(const char *passphrase, size_t len, uint8_t out[FCRYPTO_KEY_LEN]);

/** Short fingerprint of a key, for showing two boards were given the same one
 *  without showing the key itself. Not secret: derived one way from the key. */
void fcrypto_key_fingerprint(const uint8_t key[FCRYPTO_KEY_LEN], char *out, size_t cap);

void fcrypto_encrypt(const uint8_t key[FCRYPTO_KEY_LEN], const uint8_t nonce[FCRYPTO_NONCE_LEN],
                     const uint8_t *plain, size_t plain_len,
                     uint8_t *cipher_out, uint8_t tag_out[FCRYPTO_TAG_LEN]);

/** Returns false on any tampering or wrong key. Does not write partial output
 *  on failure, so the caller never has to remember to discard it. */
bool fcrypto_decrypt(const uint8_t key[FCRYPTO_KEY_LEN], const uint8_t nonce[FCRYPTO_NONCE_LEN],
                     const uint8_t *cipher, size_t cipher_len,
                     const uint8_t tag[FCRYPTO_TAG_LEN], uint8_t *plain_out);

#ifdef __cplusplus
}
#endif
#endif /* FCRYPTO_H */
