/* Who you have personally confirmed.
 *
 * This is NOT cryptographic verification. Nothing in this build is signed, so
 * there is no signature to check and nothing for the board to prove on its
 * own. What this is instead: a record that a human, standing in the room
 * with another person, read the fingerprint words aloud, agreed they matched,
 * and pressed Confirm. That is the trust model described in the design
 * spec, "verified in person, once", and it needs no cryptography to be real.
 * It only needs the board to remember it, which it did not do until now.
 */
#ifndef CONTACTS_H
#define CONTACTS_H

#include <stdbool.h>
#include "fcp.h"

#define CONTACTS_MAX 64

/** True if this fingerprint has been confirmed before, survives a reboot. */
bool contacts_is_confirmed(const uint8_t id[FCP_ID_LEN]);

/** Records a confirmation. Returns false only if the contact list is full. */
bool contacts_confirm(const uint8_t id[FCP_ID_LEN]);

#endif /* CONTACTS_H */
