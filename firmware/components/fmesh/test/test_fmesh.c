/* Host tests for the relay brain, including the density simulation that is the
 * whole reason this component exists. A flood that works with three nodes and
 * collapses with fifty is not a mesh, it is a demo. */

#include "fmesh.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        checks++;                                         \
        if (!(cond)) {                                    \
            failures++;                                   \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static void section(const char *n) { printf("%s\n", n); }

/* Deterministic randomness, so a failure can be replayed exactly. */
typedef struct { uint32_t s; } rng_t;

static uint32_t rng_next(void *ctx) {
    rng_t *r = (rng_t *)ctx;
    uint32_t x = r->s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->s = x;
    return x;
}

static void mk_id(uint8_t out[FCP_ID_LEN], uint8_t n) {
    memset(out, 0, FCP_ID_LEN);
    out[0] = 0xA0u;
    out[5] = n;
}

/* Build an addressed frame from `from` to `to`. */
static size_t mk_msg(uint8_t *buf, size_t cap, uint8_t from, uint8_t to,
                     uint32_t msg_id, uint8_t hop_limit) {
    fcp_hdr_t h;
    memset(&h, 0, sizeof h);
    h.version = FCP_VERSION;
    h.type = FCP_T_MSG;
    h.flags = FCP_FLAG_ADDRESSED;
    h.msg_id = msg_id;
    h.hop_limit = hop_limit;
    h.hop_count = 0;
    mk_id(h.src_id, from);
    mk_id(h.dst_id, to);

    uint8_t payload[FCP_NONCE_LEN + FCP_TAG_LEN + 16u];
    memset(payload, 0x22, sizeof payload);

    size_t n = 0;
    if (fcp_frame_encode(&h, payload, sizeof payload, buf, cap, &n) != FCP_OK) return 0;
    return n;
}

static size_t mk_beacon(uint8_t *buf, size_t cap, uint8_t from, uint16_t boot_id,
                        uint32_t seq, uint32_t msg_id) {
    fcp_hdr_t h;
    memset(&h, 0, sizeof h);
    h.version = FCP_VERSION;
    h.type = FCP_T_BEACON;
    h.flags = 0;
    h.msg_id = msg_id;
    h.hop_limit = 1;
    h.hop_count = 0;
    mk_id(h.src_id, from);

    fcp_beacon_t b;
    memset(&b, 0, sizeof b);
    b.name_len = 4;
    memcpy(b.name, "kaya", 4);
    b.boot_id = boot_id;
    b.seq = seq;

    uint8_t payload[FCP_BEACON_PAYLOAD_LEN];
    size_t plen = 0;
    if (fcp_beacon_encode(&b, payload, sizeof payload, &plen) != FCP_OK) return 0;

    size_t n = 0;
    if (fcp_frame_encode(&h, payload, plen, buf, cap, &n) != FCP_OK) return 0;
    return n;
}

/* ---- basics ------------------------------------------------------------ */

static void test_deliver_and_dedup(void) {
    section("delivery and deduplication");

    rng_t r = { 1 };
    fmesh_t m;
    uint8_t me[FCP_ID_LEN];
    mk_id(me, 9);
    fmesh_init(&m, me, rng_next, &r);

    uint8_t frame[FCP_MAX_FRAME];
    const size_t n = mk_msg(frame, sizeof frame, 1, 9, 0x1000u, 3);
    CHECK(n > 0, "fixture frame should encode");

    CHECK(fmesh_rx(&m, frame, n, -60, 1000) == FMESH_DELIVER, "addressed to us, deliver it");
    CHECK(fmesh_rx(&m, frame, n, -60, 1010) == FMESH_DROP, "the same frame again is a duplicate");
    CHECK(m.stats.delivered == 1, "delivered once");
    CHECK(m.stats.duplicates == 1, "one duplicate counted");

    /* Garbage must be counted and dropped, never acted on. */
    uint8_t junk[40];
    memset(junk, 0xEE, sizeof junk);
    CHECK(fmesh_rx(&m, junk, sizeof junk, -60, 1020) == FMESH_DROP, "garbage is dropped");
    CHECK(m.stats.malformed == 1, "malformed counted");
}

static void test_relay_and_suppression(void) {
    section("relay, and staying quiet when somebody beats us");

    rng_t r = { 7 };
    fmesh_t m;
    uint8_t me[FCP_ID_LEN];
    mk_id(me, 5);
    fmesh_init(&m, me, rng_next, &r);

    uint8_t frame[FCP_MAX_FRAME];
    const size_t n = mk_msg(frame, sizeof frame, 1, 9, 0x2000u, 3);

    CHECK(fmesh_rx(&m, frame, n, -70, 5000) == FMESH_RELAY_QUEUED, "not ours, queue a repeat");
    CHECK(fmesh_queue_depth(&m) == 1, "one repeat pending");

    /* Nothing is due before the minimum delay. */
    fmesh_out_t out[4];
    CHECK(fmesh_due(&m, 5000 + FMESH_DELAY_MIN_MS - 1u, out, 4) == 0,
          "nothing is due before the minimum assessment delay");

    /* Everything is due after the maximum. */
    const size_t sent = fmesh_due(&m, 5000 + FMESH_DELAY_MIN_MS + FMESH_DELAY_SPAN_MAX_MS + 1u, out, 4);
    CHECK(sent == 1, "the repeat came due, got %zu", sent);
    CHECK(fmesh_queue_depth(&m) == 0, "queue drained");
    CHECK(m.stats.relayed == 1, "one relay counted");

    /* The relayed copy must carry one more hop than the original. */
    fcp_hdr_t h;
    const uint8_t *p = NULL;
    size_t plen = 0;
    CHECK(fcp_frame_decode(out[0].frame, out[0].len, &h, &p, &plen) == FCP_OK,
          "the relayed frame is still valid");
    CHECK(h.hop_count == 1, "hop_count incremented, got %u", h.hop_count);
    CHECK(h.hop_limit == 2, "hop_limit decremented, got %u", h.hop_limit);

    /* Now the suppression path: queue one, then hear somebody else send it. */
    fmesh_t m2;
    fmesh_init(&m2, me, rng_next, &r);
    const size_t n2 = mk_msg(frame, sizeof frame, 1, 9, 0x3000u, 3);
    CHECK(fmesh_rx(&m2, frame, n2, -70, 100) == FMESH_RELAY_QUEUED, "queued");

    uint8_t theirs[FCP_MAX_FRAME];
    memcpy(theirs, frame, n2);
    CHECK(fcp_frame_step_hop(theirs, n2), "somebody else forwards it");

    CHECK(fmesh_rx(&m2, theirs, n2, -65, 130) == FMESH_DROP, "their copy is a duplicate to us");
    CHECK(fmesh_queue_depth(&m2) == 0, "our repeat was cancelled");
    CHECK(m2.stats.suppressed == 1, "suppression counted");
    CHECK(m2.stats.relayed == 0, "we never transmitted");
}

static void test_hop_exhaustion(void) {
    section("hop exhaustion");

    rng_t r = { 3 };
    fmesh_t m;
    uint8_t me[FCP_ID_LEN];
    mk_id(me, 5);
    fmesh_init(&m, me, rng_next, &r);

    uint8_t frame[FCP_MAX_FRAME];
    const size_t n = mk_msg(frame, sizeof frame, 1, 9, 0x4000u, 0);
    CHECK(fmesh_rx(&m, frame, n, -70, 10) == FMESH_DROP, "a frame with no hops left dies here");
    CHECK(fmesh_queue_depth(&m) == 0, "and nothing is queued");
}

static void test_own_echo(void) {
    section("our own packet coming back");

    rng_t r = { 11 };
    fmesh_t m;
    uint8_t me[FCP_ID_LEN];
    mk_id(me, 1);
    fmesh_init(&m, me, rng_next, &r);

    uint8_t frame[FCP_MAX_FRAME];
    const size_t n = mk_msg(frame, sizeof frame, 1, 9, 0x5000u, 3);

    CHECK(fmesh_rx(&m, frame, n, -70, 10) == FMESH_DROP, "we do not relay our own traffic");
    CHECK(m.stats.relayed == 0, "nothing relayed");
    CHECK(fmesh_queue_depth(&m) == 0, "nothing queued");
}

/* ---- beacons ----------------------------------------------------------- */

static void test_beacons(void) {
    section("beacons, replay, and reboot");

    rng_t r = { 5 };
    fmesh_t m;
    uint8_t me[FCP_ID_LEN];
    mk_id(me, 5);
    fmesh_init(&m, me, rng_next, &r);

    uint8_t frame[FCP_MAX_FRAME];
    uint8_t them[FCP_ID_LEN];
    mk_id(them, 2);

    size_t n = mk_beacon(frame, sizeof frame, 2, 0xAAAAu, 1, 0x10u);
    CHECK(fmesh_rx(&m, frame, n, -55, 1000) == FMESH_BEACON, "a first beacon is accepted");

    const fmesh_peer_t *p = fmesh_peer(&m, them);
    CHECK(p != NULL, "the peer appeared");
    CHECK(p && p->rssi == -55, "signal recorded");
    CHECK(p && p->seq == 1, "sequence recorded");
    CHECK(fmesh_peer_count(&m, 1000) == 1, "one peer in range");

    /* A beacon relays exactly one hop. */
    CHECK(fmesh_queue_depth(&m) == 1, "the beacon is queued for one repeat");
    fmesh_out_t out[2];
    CHECK(fmesh_due(&m, 1000 + FMESH_DELAY_MIN_MS + FMESH_DELAY_SPAN_MAX_MS + 1u, out, 2) == 1, "the repeat came due");
    fcp_hdr_t rh;
    const uint8_t *rp = NULL;
    size_t rplen = 0;
    CHECK(fcp_frame_decode(out[0].frame, out[0].len, &rh, &rp, &rplen) == FCP_OK, "valid");
    CHECK(rh.hop_limit == 0, "a repeated beacon has no hops left, so it stops there");

    /* A replayed beacon is refused. */
    n = mk_beacon(frame, sizeof frame, 2, 0xAAAAu, 1, 0x11u);
    CHECK(fmesh_rx(&m, frame, n, -55, 1100) == FMESH_DROP, "same sequence is a replay");
    CHECK(m.stats.replays == 1, "replay counted");

    n = mk_beacon(frame, sizeof frame, 2, 0xAAAAu, 2, 0x12u);
    CHECK(fmesh_rx(&m, frame, n, -55, 1200) == FMESH_BEACON, "a higher sequence is fresh");

    /* The reboot case. A restarted board starts again at sequence 0, and must
     * not be locked out by everyone who remembers its old sequence. */
    n = mk_beacon(frame, sizeof frame, 2, 0xBBBBu, 0, 0x13u);
    CHECK(fmesh_rx(&m, frame, n, -55, 1300) == FMESH_BEACON,
          "a new boot id resets the sequence, so a rebooted board comes back");
    p = fmesh_peer(&m, them);
    CHECK(p && p->seq == 0, "sequence tracking restarted");

    /* Peers age out. */
    CHECK(fmesh_peer_count(&m, 1300 + FMESH_PEER_TTL_MS + 1u) == 0,
          "a peer not heard for a while drops off the roster");
}

/* ---- the millisecond counter rolls over -------------------------------- */

static void test_clock_wrap(void) {
    section("the clock rolling over after 49 days");

    rng_t r = { 13 };
    fmesh_t m;
    uint8_t me[FCP_ID_LEN];
    mk_id(me, 5);
    fmesh_init(&m, me, rng_next, &r);

    const uint32_t near_wrap = 0xFFFFFF00u;

    uint8_t frame[FCP_MAX_FRAME];
    const size_t n = mk_msg(frame, sizeof frame, 1, 9, 0x6000u, 3);
    CHECK(fmesh_rx(&m, frame, n, -70, near_wrap) == FMESH_RELAY_QUEUED, "queued before the wrap");

    /* Due time lands after the counter wraps to zero. A naive comparison would
     * either fire immediately or never fire at all. */
    fmesh_out_t out[2];
    CHECK(fmesh_due(&m, near_wrap + 5u, out, 2) == 0, "not due yet, across the wrap");
    CHECK(fmesh_due(&m, near_wrap + FMESH_DELAY_MIN_MS + FMESH_DELAY_SPAN_MAX_MS + 2u, out, 2) == 1,
          "due after the wrap");

    /* And the dedup entry stored before the wrap is still recognised after it. */
    CHECK(fmesh_rx(&m, frame, n, -70, near_wrap + 300u) == FMESH_DROP,
          "a duplicate is still a duplicate across the wrap");
}

/* ---- density ----------------------------------------------------------- */

/* One radio cell where every node hears every other node. This is the worst
 * case for a flood and the case that kills naive implementations.
 *
 * The run has two phases. First everyone beacons, which is how a node learns
 * how crowded the cell is and therefore how wide to make its contention
 * window. Only then is the message sent and counted. Skipping the beacon phase
 * would measure a mesh whose nodes all believe they are alone, which is not a
 * mesh anybody will ever run.
 */
#define SIM_MAX_NODES 128u
#define SIM_AIR_SLOTS 256u

typedef struct {
    fmesh_t  mesh[SIM_MAX_NODES];
    rng_t    rngs[SIM_MAX_NODES];
    uint8_t  air[SIM_AIR_SLOTS][FCP_MAX_FRAME];
    size_t   air_len[SIM_AIR_SLOTS];
    size_t   air_from[SIM_AIR_SLOTS];
    size_t   air_n;
    size_t   nodes;
    size_t   transmissions;
    bool     counting;
} sim_t;

static sim_t sim;

/* Advance one millisecond: deliver last tick's traffic, then collect whatever
 * came due. Transmissions are only tallied once counting is switched on. */
static void sim_tick(uint32_t t) {
    const size_t pending = sim.air_n;
    static uint8_t inflight[SIM_AIR_SLOTS][FCP_MAX_FRAME];
    static size_t inflight_len[SIM_AIR_SLOTS];
    static size_t inflight_from[SIM_AIR_SLOTS];

    for (size_t a = 0; a < pending; a++) {
        memcpy(inflight[a], sim.air[a], sim.air_len[a]);
        inflight_len[a] = sim.air_len[a];
        inflight_from[a] = sim.air_from[a];
    }
    sim.air_n = 0;

    for (size_t a = 0; a < pending; a++) {
        for (size_t i = 0; i < sim.nodes; i++) {
            if (i == inflight_from[a]) continue;
            fmesh_rx(&sim.mesh[i], inflight[a], inflight_len[a], -70, t);
        }
    }

    for (size_t i = 0; i < sim.nodes; i++) {
        fmesh_out_t out[FMESH_RELAY_SLOTS];
        const size_t k = fmesh_due(&sim.mesh[i], t, out, FMESH_RELAY_SLOTS);
        for (size_t j = 0; j < k && sim.air_n < SIM_AIR_SLOTS; j++) {
            memcpy(sim.air[sim.air_n], out[j].frame, out[j].len);
            sim.air_len[sim.air_n] = out[j].len;
            sim.air_from[sim.air_n] = i;
            sim.air_n++;
            if (sim.counting) sim.transmissions++;
        }
    }
}

static void run_density(size_t nodes, uint32_t seed,
                        size_t *transmissions_out, size_t *known_peers_out) {
    if (nodes > SIM_MAX_NODES) nodes = SIM_MAX_NODES;

    memset(&sim, 0, sizeof sim);
    sim.nodes = nodes;

    for (size_t i = 0; i < nodes; i++) {
        sim.rngs[i].s = seed + (uint32_t)i * 2654435761u;
        uint8_t id[FCP_ID_LEN];
        mk_id(id, (uint8_t)i);
        fmesh_init(&sim.mesh[i], id, rng_next, &sim.rngs[i]);
    }

    /* Phase one: everybody announces themselves, staggered so the beacons do
     * not all land on the same millisecond. */
    uint32_t t = 1;
    for (size_t i = 0; i < nodes; i++, t++) {
        uint8_t frame[FCP_MAX_FRAME];
        const size_t n = mk_beacon(frame, sizeof frame, (uint8_t)i, 0x1234u, 1, 0x100u + (uint32_t)i);
        for (size_t j = 0; j < nodes; j++) {
            if (j == i) continue;
            fmesh_rx(&sim.mesh[j], frame, n, -70, t);
        }
        sim_tick(t);
    }

    /* Let the beacon repeats drain before measuring anything. */
    for (uint32_t settle = 0; settle < 4000u; settle++, t++) sim_tick(t);

    *known_peers_out = fmesh_peer_count(&sim.mesh[1], t);

    /* Phase two: one message, counted. */
    sim.counting = true;
    sim.transmissions = 1;   /* node 0's original transmission */

    uint8_t frame[FCP_MAX_FRAME];
    const size_t flen = mk_msg(frame, sizeof frame, 0, (uint8_t)(nodes - 1), 0x7777u, 3);
    for (size_t i = 1; i < nodes; i++) fmesh_rx(&sim.mesh[i], frame, flen, -70, t);
    t++;

    for (uint32_t k = 0; k < 4000u; k++, t++) sim_tick(t);

    *transmissions_out = sim.transmissions;
}

static void test_density(void) {
    section("density, one packet through a single cell");

    const size_t sizes[] = { 10, 25, 50, 100 };

    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        size_t worst = 0;
        size_t peers = 0;

        /* Several seeds, because a single lucky draw proves nothing. */
        for (uint32_t seed = 1; seed <= 25u; seed++) {
            size_t tx = 0, kp = 0;
            run_density(sizes[s], seed, &tx, &kp);
            if (tx > worst) worst = tx;
            peers = kp;
        }

        printf("  %3zu nodes, %2zu known to each other -> worst case %zu transmissions\n",
               sizes[s], peers, worst);

        /* A naive flood is one transmission per node per hop, so a hundred
         * nodes would mean hundreds. Three is the ceiling the spec commits to,
         * and the contention window widens with density to hold it. */
        CHECK(worst <= 3, "%zu nodes should need at most 3 transmissions, worst was %zu",
              sizes[s], worst);
    }
}

/* ------------------------------------------------------------------------ */

int main(void) {
    printf("fmesh tests\n\n");

    test_deliver_and_dedup();
    test_relay_and_suppression();
    test_hop_exhaustion();
    test_own_echo();
    test_beacons();
    test_clock_wrap();
    test_density();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
