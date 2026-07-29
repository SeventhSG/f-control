#include "fcp.h"

#include <string.h>

/* Little endian helpers. Written out rather than memcpy'd from a struct so the
 * wire format does not depend on the host's packing or byte order. */

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

bool fcp_type_valid(uint8_t type) {
    switch (type) {
        case FCP_T_BEACON:
        case FCP_T_REQUEST:
        case FCP_T_ACCEPT:
        case FCP_T_CONFIRM:
        case FCP_T_MSG:
        case FCP_T_ACK:
            return true;
        default:
            return false;
    }
}

bool fcp_type_addressed(uint8_t type) {
    /* A beacon is shouted at the whole cell. Everything else is meant for one
     * node, even when it travels through others to get there. */
    return type != FCP_T_BEACON;
}

size_t fcp_hdr_len(const fcp_hdr_t *h) {
    if (h == NULL) return 0;
    return (h->flags & FCP_FLAG_ADDRESSED) ? FCP_HDR_LEN_ADDRESSED : FCP_HDR_LEN;
}

/* ------------------------------------------------------------------------ */
/* header                                                                    */
/* ------------------------------------------------------------------------ */

fcp_err_t fcp_hdr_encode(const fcp_hdr_t *h, uint8_t *out, size_t cap, size_t *written) {
    if (h == NULL || out == NULL) return FCP_E_NULL;
    if (!fcp_type_valid(h->type)) return FCP_E_TYPE;
    if (h->hop_limit > FCP_HOP_LIMIT_MAX || h->hop_count > FCP_HOP_LIMIT_MAX) return FCP_E_HOPS;

    const bool addressed = (h->flags & FCP_FLAG_ADDRESSED) != 0u;

    /* The flag and the type must agree. A MSG with no destination has nowhere
     * to go, and a BEACON with one is a contradiction. */
    if (addressed != fcp_type_addressed(h->type)) return FCP_E_FLAGS;

    const size_t need = addressed ? FCP_HDR_LEN_ADDRESSED : FCP_HDR_LEN;
    if (cap < need) return FCP_E_SPACE;

    out[0] = FCP_MAGIC;
    out[1] = FCP_VERSION;
    out[2] = h->type;
    out[3] = h->flags;
    put_u32(&out[4], h->msg_id);
    out[8] = h->hop_limit;
    out[9] = h->hop_count;
    memcpy(&out[10], h->src_id, FCP_ID_LEN);
    if (addressed) memcpy(&out[16], h->dst_id, FCP_ID_LEN);

    if (written != NULL) *written = need;
    return FCP_OK;
}

fcp_err_t fcp_hdr_decode(const uint8_t *in, size_t len, fcp_hdr_t *out) {
    if (in == NULL || out == NULL) return FCP_E_NULL;
    if (len < FCP_HDR_LEN) return FCP_E_SHORT;
    if (in[0] != FCP_MAGIC) return FCP_E_MAGIC;
    if (in[1] != FCP_VERSION) return FCP_E_VERSION;
    if (!fcp_type_valid(in[2])) return FCP_E_TYPE;

    const bool addressed = (in[3] & FCP_FLAG_ADDRESSED) != 0u;
    if (addressed != fcp_type_addressed(in[2])) return FCP_E_FLAGS;
    if (addressed && len < FCP_HDR_LEN_ADDRESSED) return FCP_E_SHORT;

    if (in[8] > FCP_HOP_LIMIT_MAX || in[9] > FCP_HOP_LIMIT_MAX) return FCP_E_HOPS;

    memset(out, 0, sizeof(*out));
    out->version   = in[1];
    out->type      = in[2];
    out->flags     = in[3];
    out->msg_id    = get_u32(&in[4]);
    out->hop_limit = in[8];
    out->hop_count = in[9];
    memcpy(out->src_id, &in[10], FCP_ID_LEN);
    if (addressed) memcpy(out->dst_id, &in[16], FCP_ID_LEN);

    return FCP_OK;
}

/* ------------------------------------------------------------------------ */
/* frames                                                                    */
/* ------------------------------------------------------------------------ */

/* Payload length rules, kept in one place so encode and decode cannot drift. */
static fcp_err_t payload_len_ok(uint8_t type, size_t n) {
    switch (type) {
        case FCP_T_BEACON:
            return (n == FCP_BEACON_PAYLOAD_LEN) ? FCP_OK : FCP_E_LEN;
        case FCP_T_ACK:
            return (n == 4u) ? FCP_OK : FCP_E_LEN;
        case FCP_T_MSG:
            /* nonce, at least one byte of ciphertext, tag */
            return (n > FCP_NONCE_LEN + FCP_TAG_LEN
                    && n <= FCP_NONCE_LEN + FCP_TAG_LEN + FCP_PLAINTEXT_MAX)
                       ? FCP_OK : FCP_E_LEN;
        default:
            /* Handshake payloads are sized by the Noise pattern, which this
             * layer does not model. Anything that fits the frame is allowed
             * through for the crypto layer to accept or reject. */
            return (n > 0u) ? FCP_OK : FCP_E_LEN;
    }
}

fcp_err_t fcp_frame_encode(const fcp_hdr_t *h, const uint8_t *payload, size_t payload_len,
                           uint8_t *out, size_t cap, size_t *written) {
    if (h == NULL || out == NULL) return FCP_E_NULL;
    if (payload == NULL && payload_len > 0u) return FCP_E_NULL;

    const fcp_err_t lerr = payload_len_ok(h->type, payload_len);
    if (lerr != FCP_OK) return lerr;

    size_t hlen = 0;
    const fcp_err_t herr = fcp_hdr_encode(h, out, cap, &hlen);
    if (herr != FCP_OK) return herr;

    if (hlen + payload_len > FCP_MAX_FRAME) return FCP_E_LEN;
    if (cap < hlen + payload_len) return FCP_E_SPACE;

    if (payload_len > 0u) memcpy(out + hlen, payload, payload_len);
    if (written != NULL) *written = hlen + payload_len;
    return FCP_OK;
}

fcp_err_t fcp_frame_decode(const uint8_t *in, size_t len, fcp_hdr_t *hdr,
                           const uint8_t **payload, size_t *payload_len) {
    if (in == NULL || hdr == NULL || payload == NULL || payload_len == NULL) return FCP_E_NULL;
    if (len > FCP_MAX_FRAME) return FCP_E_LEN;

    const fcp_err_t herr = fcp_hdr_decode(in, len, hdr);
    if (herr != FCP_OK) return herr;

    const size_t hlen = fcp_hdr_len(hdr);
    if (len < hlen) return FCP_E_SHORT;

    const size_t plen = len - hlen;
    const fcp_err_t lerr = payload_len_ok(hdr->type, plen);
    if (lerr != FCP_OK) return lerr;

    *payload = in + hlen;
    *payload_len = plen;
    return FCP_OK;
}

/* ------------------------------------------------------------------------ */
/* signatures                                                                */
/* ------------------------------------------------------------------------ */

fcp_err_t fcp_signed_region(const fcp_hdr_t *h, const uint8_t *payload, size_t payload_len,
                            uint8_t *out, size_t cap, size_t *written) {
    if (h == NULL || out == NULL) return FCP_E_NULL;
    if (payload == NULL && payload_len > 0u) return FCP_E_NULL;

    const size_t need = 2u + FCP_ID_LEN + payload_len;
    if (cap < need) return FCP_E_SPACE;

    /* version, type, src_id, payload. Deliberately not hop_limit, not
     * hop_count, and not msg_id, none of which are stable end to end. */
    out[0] = h->version;
    out[1] = h->type;
    memcpy(&out[2], h->src_id, FCP_ID_LEN);
    if (payload_len > 0u) memcpy(&out[2 + FCP_ID_LEN], payload, payload_len);

    if (written != NULL) *written = need;
    return FCP_OK;
}

/* ------------------------------------------------------------------------ */
/* beacon                                                                    */
/* ------------------------------------------------------------------------ */

fcp_err_t fcp_beacon_encode(const fcp_beacon_t *b, uint8_t *out, size_t cap, size_t *written) {
    if (b == NULL || out == NULL) return FCP_E_NULL;
    if (b->name_len > FCP_NAME_MAX) return FCP_E_NAME;
    if (cap < FCP_BEACON_PAYLOAD_LEN) return FCP_E_SPACE;

    size_t o = 0;
    memcpy(&out[o], b->ed25519_pub, FCP_PUB_LEN); o += FCP_PUB_LEN;
    memcpy(&out[o], b->x25519_pub,  FCP_PUB_LEN); o += FCP_PUB_LEN;

    out[o++] = b->name_len;

    /* The name field is fixed width and zero padded. A variable width field
     * would leak the length of every name to anyone counting bytes on the air,
     * and would make beacons distinguishable from one another by size. */
    memset(&out[o], 0, FCP_NAME_MAX);
    if (b->name_len > 0u) memcpy(&out[o], b->name, b->name_len);
    o += FCP_NAME_MAX;

    put_u16(&out[o], b->boot_id); o += 2u;
    put_u32(&out[o], b->seq);     o += 4u;

    memcpy(&out[o], b->signature, FCP_SIG_LEN); o += FCP_SIG_LEN;

    if (written != NULL) *written = o;
    return (o == FCP_BEACON_PAYLOAD_LEN) ? FCP_OK : FCP_E_LEN;
}

fcp_err_t fcp_beacon_decode(const uint8_t *in, size_t len, fcp_beacon_t *out) {
    if (in == NULL || out == NULL) return FCP_E_NULL;
    if (len != FCP_BEACON_PAYLOAD_LEN) return FCP_E_LEN;

    const uint8_t name_len = in[FCP_PUB_LEN * 2u];
    if (name_len > FCP_NAME_MAX) return FCP_E_NAME;

    memset(out, 0, sizeof(*out));

    size_t o = 0;
    memcpy(out->ed25519_pub, &in[o], FCP_PUB_LEN); o += FCP_PUB_LEN;
    memcpy(out->x25519_pub,  &in[o], FCP_PUB_LEN); o += FCP_PUB_LEN;

    out->name_len = in[o++];
    if (out->name_len > 0u) memcpy(out->name, &in[o], out->name_len);
    o += FCP_NAME_MAX;

    out->boot_id = get_u16(&in[o]); o += 2u;
    out->seq     = get_u32(&in[o]); o += 4u;

    memcpy(out->signature, &in[o], FCP_SIG_LEN);

    return FCP_OK;
}

/* ------------------------------------------------------------------------ */
/* relay                                                                     */
/* ------------------------------------------------------------------------ */

bool fcp_frame_step_hop(uint8_t *frame, size_t len) {
    if (frame == NULL || len < FCP_HDR_LEN) return false;
    if (frame[0] != FCP_MAGIC || frame[1] != FCP_VERSION) return false;

    if (frame[8] == 0u) return false;             /* no hops left */
    if (frame[9] >= FCP_HOP_LIMIT_MAX) return false;

    frame[8]--;   /* hop_limit  */
    frame[9]++;   /* hop_count  */
    return true;
}

/* ------------------------------------------------------------------------ */

const char *fcp_strerror(fcp_err_t e) {
    switch (e) {
        case FCP_OK:        return "ok";
        case FCP_E_SHORT:   return "frame ended early";
        case FCP_E_MAGIC:   return "not an f-control frame";
        case FCP_E_VERSION: return "unsupported protocol version";
        case FCP_E_TYPE:    return "unknown packet type";
        case FCP_E_FLAGS:   return "flags contradict the packet type";
        case FCP_E_LEN:     return "payload length wrong for the type";
        case FCP_E_NAME:    return "name longer than the field allows";
        case FCP_E_HOPS:    return "hop fields out of range";
        case FCP_E_SPACE:   return "output buffer too small";
        case FCP_E_NULL:    return "required pointer was null";
        default:            return "unknown error";
    }
}
