/* Host tests for the FCP codec. No hardware, no framework, no dependencies.
 *
 * Everything this codec parses arrived from a radio, so the interesting tests
 * are the hostile ones: truncation, garbage, contradictions, and a fuzz pass
 * whose only pass condition is that nothing crashes and nothing hands the
 * caller a pointer outside the buffer it was given.
 */

#include "fcp.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);       \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

#define CHECK_ERR(actual, expect)                                          \
    CHECK((actual) == (expect), "expected %s, got %s",                     \
          fcp_strerror(expect), fcp_strerror(actual))

static void section(const char *name) { printf("%s\n", name); }

/* ---- fixtures ---------------------------------------------------------- */

static fcp_hdr_t beacon_hdr(void) {
    fcp_hdr_t h;
    memset(&h, 0, sizeof h);
    h.version   = FCP_VERSION;
    h.type      = FCP_T_BEACON;
    h.flags     = 0;
    h.msg_id    = 0xDEADBEEFu;
    h.hop_limit = 1;
    h.hop_count = 0;
    memcpy(h.src_id, "\x01\x02\x03\x04\x05\x06", FCP_ID_LEN);
    return h;
}

static fcp_hdr_t msg_hdr(void) {
    fcp_hdr_t h = beacon_hdr();
    h.type  = FCP_T_MSG;
    h.flags = FCP_FLAG_ADDRESSED;
    h.hop_limit = 3;
    memcpy(h.dst_id, "\xAA\xBB\xCC\xDD\xEE\xFF", FCP_ID_LEN);
    return h;
}

static fcp_beacon_t sample_beacon(void) {
    fcp_beacon_t b;
    memset(&b, 0, sizeof b);
    for (int i = 0; i < FCP_PUB_LEN; i++) {
        b.ed25519_pub[i] = (uint8_t)(i);
        b.x25519_pub[i]  = (uint8_t)(0x40 + i);
    }
    b.name_len = 4;
    memcpy(b.name, "kaya", 4);
    b.boot_id = 0xB00Du;
    b.seq     = 12345u;
    for (int i = 0; i < FCP_SIG_LEN; i++) b.signature[i] = (uint8_t)(0x80 + i);
    return b;
}

/* ---- header ------------------------------------------------------------ */

static void test_header_roundtrip(void) {
    section("header round trip");

    uint8_t buf[FCP_MAX_FRAME];
    size_t n = 0;

    fcp_hdr_t in = beacon_hdr();
    CHECK_ERR(fcp_hdr_encode(&in, buf, sizeof buf, &n), FCP_OK);
    CHECK(n == FCP_HDR_LEN, "broadcast header should be %u bytes, was %zu", FCP_HDR_LEN, n);

    fcp_hdr_t out;
    CHECK_ERR(fcp_hdr_decode(buf, n, &out), FCP_OK);
    CHECK(out.type == in.type, "type survived");
    CHECK(out.msg_id == in.msg_id, "msg_id survived, got %u", out.msg_id);
    CHECK(out.hop_limit == in.hop_limit, "hop_limit survived");
    CHECK(memcmp(out.src_id, in.src_id, FCP_ID_LEN) == 0, "src_id survived");

    fcp_hdr_t ain = msg_hdr();
    CHECK_ERR(fcp_hdr_encode(&ain, buf, sizeof buf, &n), FCP_OK);
    CHECK(n == FCP_HDR_LEN_ADDRESSED, "addressed header should be %u bytes, was %zu",
          FCP_HDR_LEN_ADDRESSED, n);

    fcp_hdr_t aout;
    CHECK_ERR(fcp_hdr_decode(buf, n, &aout), FCP_OK);
    CHECK(memcmp(aout.dst_id, ain.dst_id, FCP_ID_LEN) == 0, "dst_id survived");
}

static void test_header_rejects(void) {
    section("header rejects malformed input");

    uint8_t buf[FCP_MAX_FRAME];
    size_t n = 0;
    fcp_hdr_t h = msg_hdr();
    fcp_hdr_t out;

    CHECK_ERR(fcp_hdr_encode(&h, buf, sizeof buf, &n), FCP_OK);

    /* Truncation at every possible length must be a clean error, never a read
     * past the end. */
    for (size_t len = 0; len < n; len++) {
        fcp_err_t e = fcp_hdr_decode(buf, len, &out);
        CHECK(e == FCP_E_SHORT, "len %zu should be SHORT, got %s", len, fcp_strerror(e));
    }

    uint8_t bad[FCP_MAX_FRAME];

    memcpy(bad, buf, n); bad[0] = 0x00;
    CHECK_ERR(fcp_hdr_decode(bad, n, &out), FCP_E_MAGIC);

    memcpy(bad, buf, n); bad[1] = 0x02;
    CHECK_ERR(fcp_hdr_decode(bad, n, &out), FCP_E_VERSION);

    memcpy(bad, buf, n); bad[2] = 0x7F;
    CHECK_ERR(fcp_hdr_decode(bad, n, &out), FCP_E_TYPE);

    /* A MSG with the addressed flag cleared has nowhere to go. */
    memcpy(bad, buf, n); bad[3] = 0x00;
    CHECK_ERR(fcp_hdr_decode(bad, n, &out), FCP_E_FLAGS);

    memcpy(bad, buf, n); bad[8] = 99;
    CHECK_ERR(fcp_hdr_decode(bad, n, &out), FCP_E_HOPS);

    /* A beacon claiming a destination is a contradiction. */
    fcp_hdr_t contradiction = beacon_hdr();
    contradiction.flags = FCP_FLAG_ADDRESSED;
    CHECK_ERR(fcp_hdr_encode(&contradiction, buf, sizeof buf, &n), FCP_E_FLAGS);

    fcp_hdr_t ok = beacon_hdr();
    CHECK_ERR(fcp_hdr_encode(&ok, buf, FCP_HDR_LEN - 1u, &n), FCP_E_SPACE);
}

/* ---- signatures -------------------------------------------------------- */

static void test_signed_region_ignores_hops(void) {
    section("signed region excludes the fields relays mutate");

    /* This is the defect that would have broken every forwarded packet: if the
     * signature covers hop_limit or hop_count, it verifies at the sender and
     * fails at the first relay. */
    uint8_t payload[FCP_BEACON_PAYLOAD_LEN];
    memset(payload, 0x5A, sizeof payload);

    fcp_hdr_t a = beacon_hdr();
    fcp_hdr_t b = beacon_hdr();
    b.hop_limit = 0;
    b.hop_count = 1;
    b.msg_id    = 0x11111111u;   /* also unstable, also excluded */

    uint8_t ra[256], rb[256];
    size_t na = 0, nb = 0;

    CHECK_ERR(fcp_signed_region(&a, payload, sizeof payload, ra, sizeof ra, &na), FCP_OK);
    CHECK_ERR(fcp_signed_region(&b, payload, sizeof payload, rb, sizeof rb, &nb), FCP_OK);

    CHECK(na == nb, "regions should be the same length, %zu vs %zu", na, nb);
    CHECK(memcmp(ra, rb, na) == 0,
          "a relayed packet must produce the identical signed region");

    /* But a different payload must produce a different region, or the
     * signature would be meaningless. */
    uint8_t other[FCP_BEACON_PAYLOAD_LEN];
    memset(other, 0x5A, sizeof other);
    other[7] ^= 0xFFu;
    uint8_t rc[256];
    size_t nc = 0;
    CHECK_ERR(fcp_signed_region(&a, other, sizeof other, rc, sizeof rc, &nc), FCP_OK);
    CHECK(memcmp(ra, rc, na) != 0, "a changed payload must change the signed region");

    /* And a different sender must too. */
    fcp_hdr_t d = beacon_hdr();
    d.src_id[0] ^= 0xFFu;
    uint8_t rd[256];
    size_t nd = 0;
    CHECK_ERR(fcp_signed_region(&d, payload, sizeof payload, rd, sizeof rd, &nd), FCP_OK);
    CHECK(memcmp(ra, rd, na) != 0, "a changed sender must change the signed region");

    CHECK_ERR(fcp_signed_region(&a, payload, sizeof payload, ra, 4u, &na), FCP_E_SPACE);
}

/* ---- beacon ------------------------------------------------------------ */

static void test_beacon(void) {
    section("beacon payload");

    uint8_t buf[FCP_BEACON_PAYLOAD_LEN];
    size_t n = 0;
    fcp_beacon_t in = sample_beacon();

    CHECK_ERR(fcp_beacon_encode(&in, buf, sizeof buf, &n), FCP_OK);
    CHECK(n == FCP_BEACON_PAYLOAD_LEN, "beacon should be %u bytes, was %zu",
          FCP_BEACON_PAYLOAD_LEN, n);

    fcp_beacon_t out;
    CHECK_ERR(fcp_beacon_decode(buf, n, &out), FCP_OK);
    CHECK(memcmp(out.ed25519_pub, in.ed25519_pub, FCP_PUB_LEN) == 0, "identity key survived");
    CHECK(memcmp(out.x25519_pub, in.x25519_pub, FCP_PUB_LEN) == 0, "static key survived");
    CHECK(out.name_len == in.name_len, "name_len survived");
    CHECK(memcmp(out.name, in.name, in.name_len) == 0, "name survived");
    CHECK(out.boot_id == in.boot_id, "boot_id survived, got %u", out.boot_id);
    CHECK(out.seq == in.seq, "seq survived, got %u", out.seq);
    CHECK(memcmp(out.signature, in.signature, FCP_SIG_LEN) == 0, "signature survived");

    /* The name field is fixed width, so two beacons with different name
     * lengths must still be byte identical in size. Anything else leaks the
     * length of a name to a passive listener. */
    fcp_beacon_t shortname = sample_beacon();
    shortname.name_len = 1;
    uint8_t buf2[FCP_BEACON_PAYLOAD_LEN];
    size_t n2 = 0;
    CHECK_ERR(fcp_beacon_encode(&shortname, buf2, sizeof buf2, &n2), FCP_OK);
    CHECK(n2 == n, "beacon size must not depend on name length");

    /* Padding must actually be zeroed, not left as whatever was in the struct. */
    fcp_beacon_t dirty = sample_beacon();
    memset(dirty.name, 'X', FCP_NAME_MAX);
    dirty.name_len = 2;
    CHECK_ERR(fcp_beacon_encode(&dirty, buf2, sizeof buf2, &n2), FCP_OK);
    CHECK(buf2[FCP_PUB_LEN * 2 + 1 + 2] == 0, "bytes past name_len must be zero padded");

    fcp_beacon_t bad = sample_beacon();
    bad.name_len = FCP_NAME_MAX + 1u;
    CHECK_ERR(fcp_beacon_encode(&bad, buf2, sizeof buf2, &n2), FCP_E_NAME);

    CHECK_ERR(fcp_beacon_decode(buf, FCP_BEACON_PAYLOAD_LEN - 1u, &out), FCP_E_LEN);
    CHECK_ERR(fcp_beacon_encode(&in, buf, 4u, &n), FCP_E_SPACE);

    uint8_t liar[FCP_BEACON_PAYLOAD_LEN];
    memcpy(liar, buf, sizeof liar);
    liar[FCP_PUB_LEN * 2] = FCP_NAME_MAX + 5u;
    CHECK_ERR(fcp_beacon_decode(liar, sizeof liar, &out), FCP_E_NAME);
}

/* ---- frames ------------------------------------------------------------ */

static void test_frames(void) {
    section("whole frames");

    uint8_t payload[FCP_BEACON_PAYLOAD_LEN];
    memset(payload, 0x33, sizeof payload);

    uint8_t frame[FCP_MAX_FRAME];
    size_t n = 0;
    fcp_hdr_t h = beacon_hdr();

    CHECK_ERR(fcp_frame_encode(&h, payload, sizeof payload, frame, sizeof frame, &n), FCP_OK);
    CHECK(n == FCP_HDR_LEN + FCP_BEACON_PAYLOAD_LEN,
          "beacon frame should be %u bytes, was %zu",
          FCP_HDR_LEN + FCP_BEACON_PAYLOAD_LEN, n);
    CHECK(n <= FCP_MAX_FRAME, "beacon frame must fit an ESP-NOW frame");

    fcp_hdr_t dh;
    const uint8_t *dp = NULL;
    size_t dn = 0;
    CHECK_ERR(fcp_frame_decode(frame, n, &dh, &dp, &dn), FCP_OK);
    CHECK(dn == sizeof payload, "payload length survived");
    CHECK(dp == frame + FCP_HDR_LEN, "payload should point into the caller's buffer");
    CHECK(memcmp(dp, payload, dn) == 0, "payload bytes survived");

    /* A beacon is a fixed size. Anything else claiming to be one is malformed. */
    CHECK_ERR(fcp_frame_encode(&h, payload, 10u, frame, sizeof frame, &n), FCP_E_LEN);

    /* MSG bounds: nonce plus tag plus at least one byte, and no more than the
     * plaintext cap. */
    fcp_hdr_t mh = msg_hdr();
    uint8_t big[FCP_MAX_FRAME];
    memset(big, 7, sizeof big);

    const size_t just_right = FCP_NONCE_LEN + FCP_TAG_LEN + FCP_PLAINTEXT_MAX;
    CHECK_ERR(fcp_frame_encode(&mh, big, just_right, frame, sizeof frame, &n), FCP_OK);
    CHECK(n == FCP_HDR_LEN_ADDRESSED + just_right, "largest message frame is %zu bytes", n);
    CHECK(n <= FCP_MAX_FRAME, "largest message must still fit an ESP-NOW frame, was %zu", n);

    CHECK_ERR(fcp_frame_encode(&mh, big, just_right + 1u, frame, sizeof frame, &n), FCP_E_LEN);
    CHECK_ERR(fcp_frame_encode(&mh, big, FCP_NONCE_LEN + FCP_TAG_LEN, frame, sizeof frame, &n),
              FCP_E_LEN);

    /* ACK is exactly one message id. */
    fcp_hdr_t ah = msg_hdr();
    ah.type = FCP_T_ACK;
    CHECK_ERR(fcp_frame_encode(&ah, big, 4u, frame, sizeof frame, &n), FCP_OK);
    CHECK_ERR(fcp_frame_encode(&ah, big, 5u, frame, sizeof frame, &n), FCP_E_LEN);

    /* Nothing larger than a radio frame is ever accepted. */
    uint8_t oversize[FCP_MAX_FRAME + 1];
    memset(oversize, 0, sizeof oversize);
    oversize[0] = FCP_MAGIC;
    oversize[1] = FCP_VERSION;
    oversize[2] = FCP_T_MSG;
    oversize[3] = FCP_FLAG_ADDRESSED;
    CHECK_ERR(fcp_frame_decode(oversize, sizeof oversize, &dh, &dp, &dn), FCP_E_LEN);
}

static void test_relay(void) {
    section("relay hop accounting");

    uint8_t payload[FCP_NONCE_LEN + FCP_TAG_LEN + 8u];
    memset(payload, 1, sizeof payload);

    uint8_t frame[FCP_MAX_FRAME];
    size_t n = 0;
    fcp_hdr_t h = msg_hdr();          /* hop_limit 3, hop_count 0 */
    CHECK_ERR(fcp_frame_encode(&h, payload, sizeof payload, frame, sizeof frame, &n), FCP_OK);

    for (int expect = 1; expect <= 3; expect++) {
        CHECK(fcp_frame_step_hop(frame, n), "hop %d should be allowed", expect);
        fcp_hdr_t out;
        CHECK_ERR(fcp_hdr_decode(frame, n, &out), FCP_OK);
        CHECK(out.hop_count == (uint8_t)expect, "hop_count should be %d, was %u",
              expect, out.hop_count);
        CHECK(out.hop_limit == (uint8_t)(3 - expect), "hop_limit should be %d, was %u",
              3 - expect, out.hop_limit);
    }

    CHECK(!fcp_frame_step_hop(frame, n), "a frame with no hops left must not be forwarded");

    fcp_hdr_t after;
    CHECK_ERR(fcp_hdr_decode(frame, n, &after), FCP_OK);
    CHECK(after.hop_limit == 0 && after.hop_count == 3,
          "a refused hop must leave the frame untouched");

    uint8_t stub[4] = {0};
    CHECK(!fcp_frame_step_hop(stub, sizeof stub), "a runt frame cannot be forwarded");
    CHECK(!fcp_frame_step_hop(NULL, 0), "null is not a frame");
}

/* ---- fuzz -------------------------------------------------------------- */

static uint32_t rng_state = 0x12345678u;

static uint32_t rng(void) {
    /* xorshift32, fixed seed, so a failure is reproducible. */
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void test_fuzz(void) {
    section("fuzz, 200000 hostile frames");

    const int rounds = 200000;
    int accepted = 0;

    for (int i = 0; i < rounds; i++) {
        uint8_t buf[300];
        const size_t len = (size_t)(rng() % (sizeof buf + 1u));

        for (size_t j = 0; j < len; j++) buf[j] = (uint8_t)(rng() & 0xFFu);

        /* Bias a quarter of the rounds towards well formed headers, otherwise
         * almost every case dies at the magic byte and the deeper paths never
         * get exercised. */
        if (len >= FCP_HDR_LEN && (i % 4) == 0) {
            buf[0] = FCP_MAGIC;
            buf[1] = FCP_VERSION;
            buf[2] = (uint8_t)(1u + (rng() % 6u));
            buf[3] = fcp_type_addressed(buf[2]) ? FCP_FLAG_ADDRESSED : 0u;
            buf[8] = (uint8_t)(rng() % (FCP_HOP_LIMIT_MAX + 1u));
            buf[9] = (uint8_t)(rng() % (FCP_HOP_LIMIT_MAX + 1u));
        }

        fcp_hdr_t h;
        const uint8_t *p = NULL;
        size_t plen = 0;

        const fcp_err_t e = fcp_frame_decode(buf, len, &h, &p, &plen);
        if (e != FCP_OK) continue;

        accepted++;

        /* The only promises a successful decode makes. If any of these break,
         * something upstream is about to read memory it does not own. */
        CHECK(p >= buf, "payload must not point before the buffer");
        CHECK(p + plen <= buf + len, "payload must not run past the buffer");
        CHECK(plen == len - fcp_hdr_len(&h), "payload length must match the header");
        CHECK(fcp_type_valid(h.type), "an accepted frame must have a known type");

        /* A beacon that decodes must also decode as a beacon payload. */
        if (h.type == FCP_T_BEACON) {
            fcp_beacon_t b;
            const fcp_err_t be = fcp_beacon_decode(p, plen, &b);
            CHECK(be == FCP_OK || be == FCP_E_NAME,
                  "a beacon payload should decode or be rejected for its name, got %s",
                  fcp_strerror(be));
        }
    }

    printf("  %d of %d accepted\n", accepted, rounds);
    CHECK(accepted > 0, "the fuzzer never produced a valid frame, so it tested nothing");
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("fcp codec tests\n\n");

    test_header_roundtrip();
    test_header_rejects();
    test_signed_region_ignores_hops();
    test_beacon();
    test_frames();
    test_relay();
    test_fuzz();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
