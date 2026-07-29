/* The shared network key. See the note in fcrypto.h for what this is and is
 * not: one symmetric key held by every board on the same "network", not a
 * per-contact key. Anyone holding it can read everything encrypted under it
 * and nothing here proves who sent a given message.
 */
#ifndef NETKEY_H
#define NETKEY_H

#include <stddef.h>
#include <stdint.h>

#include "fcrypto.h"

/** Loads the stored key, or generates and stores a random one on first boot
 *  so a board fresh out of the box still encrypts, even though it cannot talk
 *  to anyone until it shares a key with them. */
void netkey_load(uint8_t key[FCRYPTO_KEY_LEN]);

/** Derives a key from a passphrase and stores it, replacing whatever was
 *  there. Two boards given the same passphrase derive the identical key with
 *  no other coordination. */
void netkey_set_passphrase(const char *passphrase, size_t len);

/** Replaces the stored key with a fresh random one, for starting a new,
 *  private network that nobody else could guess their way into by trying a
 *  passphrase you might also use elsewhere. */
void netkey_set_random(void);

/** Four byte fingerprint of the currently stored key, for the settings screen
 *  so two people can confirm out loud that they typed the same passphrase
 *  without either of them reading the key itself over the phone. */
void netkey_fingerprint(char *out, size_t cap);

#endif /* NETKEY_H */
