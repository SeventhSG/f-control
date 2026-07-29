/* Who this board is.
 *
 * In this build the identity is a random 32 byte value generated once and kept
 * in NVS. It is NOT a keypair and nothing is signed, so the fingerprint proves
 * only that a beacon carries a stable, self-chosen label, not that it belongs
 * to any particular person. Anybody can claim any identity in this build. This
 * is separate from message confidentiality, which is handled by the shared
 * network key in netkey.h and does not depend on this file at all.
 */
#ifndef IDENT_H
#define IDENT_H

#include <stdint.h>
#include "fcp.h"

#define IDENT_KEY_LEN   32
#define IDENT_NAME_MAX  FCP_NAME_MAX
#define IDENT_WORDS     4

typedef struct {
    uint8_t  key[IDENT_KEY_LEN];        /* random, stands in for a public key */
    uint8_t  id[FCP_ID_LEN];            /* first 6 bytes of BLAKE2b(key) */
    char     name[IDENT_NAME_MAX + 1];
    uint16_t boot_id;                   /* fresh every boot */
    uint32_t seq;                       /* beacon counter within this boot */
} ident_t;

/** Load from NVS, generating and storing a new identity on first boot. */
void ident_load(ident_t *out);

/** Persist a new display name. Returns false if it is too long. */
bool ident_set_name(ident_t *id, const char *name);

/** Overrides the auto-generated access point name. Empty string clears the
 *  override and returns to the fingerprint-derived default. */
bool ident_set_ap_name(const char *name);

/** Human readable fingerprint, e.g. "horse amber nine river". */
void ident_words(const ident_t *id, char *out, size_t cap);

/** The same rendering for somebody else's id, for the roster. */
void ident_words_of(const uint8_t id[FCP_ID_LEN], char *out, size_t cap);

/** Access point name derived from the identity, e.g. "f-control-a91b". */
void ident_ap_name(const ident_t *id, char *out, size_t cap);

#endif /* IDENT_H */
