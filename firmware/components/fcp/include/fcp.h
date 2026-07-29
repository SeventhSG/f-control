/* FCP, the f-control wire protocol, version 1.
 *
 * This layer turns bytes into structs and back. It performs no I/O, no
 * allocation, and no cryptography. Everything it touches arrived from a radio
 * and is therefore hostile until proven otherwise, so every decode path is
 * bounds checked and every failure is a return value rather than a trap.
 *
 * Portable C11 with no ESP-IDF dependency, so the whole thing compiles and is
 * tested on a development machine with no hardware attached.
 *
 * Wire format is little endian. See docs/superpowers/specs.
 */
#ifndef FCP_H
#define FCP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FCP_MAGIC              0xFCu
#define FCP_VERSION            0x01u

/* ESP-NOW sets the ceiling. LoRa frames are smaller and are fragmented by a
 * layer above this one. */
#define FCP_MAX_FRAME          250u

#define FCP_HDR_LEN            16u   /* broadcast */
#define FCP_HDR_LEN_ADDRESSED  22u   /* with dst_id */

#define FCP_ID_LEN             6u    /* first 6 bytes of SHA-256(ed25519_pub) */
#define FCP_NAME_MAX           20u
#define FCP_SIG_LEN            64u
#define FCP_PUB_LEN            32u

#define FCP_BEACON_PAYLOAD_LEN 155u
#define FCP_NONCE_LEN          12u
#define FCP_TAG_LEN            16u

/* 250 total, minus a 22 byte addressed header, minus nonce, minus tag, leaves
 * 200. We cap plaintext at 180 and keep 20 in reserve so the format can grow
 * without a version bump. */
#define FCP_PLAINTEXT_MAX      180u

/* A packet arriving with more hops than this is malformed or malicious. */
#define FCP_HOP_LIMIT_MAX      8u

typedef enum {
    FCP_T_BEACON  = 0x01,
    FCP_T_REQUEST = 0x02,
    FCP_T_ACCEPT  = 0x03,
    FCP_T_CONFIRM = 0x04,
    FCP_T_MSG     = 0x05,
    FCP_T_ACK     = 0x06,
} fcp_type_t;

#define FCP_FLAG_ADDRESSED 0x01u

typedef enum {
    FCP_OK = 0,
    FCP_E_SHORT,      /* ran out of bytes */
    FCP_E_MAGIC,      /* not an FCP frame at all */
    FCP_E_VERSION,    /* a newer f-control is on the air */
    FCP_E_TYPE,       /* unknown packet type */
    FCP_E_FLAGS,      /* flags contradict the type */
    FCP_E_LEN,        /* payload length wrong for the type */
    FCP_E_NAME,       /* name_len outside 0..FCP_NAME_MAX */
    FCP_E_HOPS,       /* hop fields out of range */
    FCP_E_SPACE,      /* output buffer too small */
    FCP_E_NULL,       /* a required pointer was NULL */
} fcp_err_t;

typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint8_t  flags;
    uint32_t msg_id;
    uint8_t  hop_limit;
    uint8_t  hop_count;
    uint8_t  src_id[FCP_ID_LEN];
    uint8_t  dst_id[FCP_ID_LEN];  /* meaningful only with FCP_FLAG_ADDRESSED */
} fcp_hdr_t;

typedef struct {
    uint8_t  ed25519_pub[FCP_PUB_LEN];
    uint8_t  x25519_pub[FCP_PUB_LEN];
    uint8_t  name_len;
    char     name[FCP_NAME_MAX];   /* not NUL terminated on the wire */
    uint16_t boot_id;              /* random per boot, resets seq tracking */
    uint32_t seq;                  /* increments per beacon within one boot */
    uint8_t  signature[FCP_SIG_LEN];
} fcp_beacon_t;

/* ---- header ----------------------------------------------------------- */

/** Encoded length of this header, 16 or 22. */
size_t fcp_hdr_len(const fcp_hdr_t *h);

fcp_err_t fcp_hdr_encode(const fcp_hdr_t *h, uint8_t *out, size_t cap, size_t *written);
fcp_err_t fcp_hdr_decode(const uint8_t *in, size_t len, fcp_hdr_t *out);

/* ---- whole frames ------------------------------------------------------ */

/**
 * Encode header followed by an opaque payload. The codec deliberately knows
 * nothing about what is inside a REQUEST, MSG or ACK payload, because that is
 * the crypto layer's business.
 */
fcp_err_t fcp_frame_encode(const fcp_hdr_t *h, const uint8_t *payload, size_t payload_len,
                           uint8_t *out, size_t cap, size_t *written);

/**
 * Decode and validate a received frame. On success `*payload` points into
 * `in`, so it stays valid only as long as the caller's buffer does. Nothing is
 * copied and nothing is allocated.
 */
fcp_err_t fcp_frame_decode(const uint8_t *in, size_t len, fcp_hdr_t *hdr,
                           const uint8_t **payload, size_t *payload_len);

/* ---- signatures -------------------------------------------------------- */

/**
 * Serialise exactly the bytes a signature covers: version, type, src_id, then
 * the payload.
 *
 * hop_limit and hop_count are excluded on purpose. Every relay mutates both,
 * so a signature covering them would verify at the sender and fail at the
 * first hop, and the mesh would silently drop everything it forwarded.
 *
 * Returns FCP_OK and sets *written, or FCP_E_SPACE.
 */
fcp_err_t fcp_signed_region(const fcp_hdr_t *h, const uint8_t *payload, size_t payload_len,
                            uint8_t *out, size_t cap, size_t *written);

/* ---- beacon ------------------------------------------------------------ */

fcp_err_t fcp_beacon_encode(const fcp_beacon_t *b, uint8_t *out, size_t cap, size_t *written);
fcp_err_t fcp_beacon_decode(const uint8_t *in, size_t len, fcp_beacon_t *out);

/* ---- relay ------------------------------------------------------------- */

/**
 * Rewrite the hop fields of an already encoded frame in place, for forwarding.
 * Returns false when the frame has no hops left, in which case the buffer is
 * untouched and the caller must drop it.
 */
bool fcp_frame_step_hop(uint8_t *frame, size_t len);

/* ---- misc -------------------------------------------------------------- */

const char *fcp_strerror(fcp_err_t e);

/** True for a type this build understands. */
bool fcp_type_valid(uint8_t type);

/** True when the type must carry a destination. */
bool fcp_type_addressed(uint8_t type);

#ifdef __cplusplus
}
#endif
#endif /* FCP_H */
