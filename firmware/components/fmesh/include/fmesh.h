/* fmesh, the relay brain.
 *
 * Decides three things and nothing else: have I seen this packet before, is it
 * for me, and should I repeat it. It owns no radio, no timer and no crypto. A
 * clock and a random source are handed in, which is what lets a hundred
 * virtual nodes run a full flood on a development machine in milliseconds.
 *
 * The hard problem here is the broadcast storm. ESP-NOW is one shared channel,
 * so a naive flood where everyone repeats everything collapses as soon as a
 * handful of nodes are talking. The fix is counter based flooding with a
 * random assessment delay: wait a short random moment before repeating, and if
 * somebody else repeats it first, stay quiet.
 */
#ifndef FMESH_H
#define FMESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fcp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Addressed traffic only. Beacons are deduplicated by (boot_id, seq) in the
 * peer table, because at the 50 node design target they alone would churn a
 * ring this size every few seconds and push real traffic out of it. */
#define FMESH_DEDUP_SLOTS     128u
#define FMESH_DEDUP_TTL_MS    60000u

#define FMESH_PEER_SLOTS      64u
#define FMESH_PEER_TTL_MS     30000u

#define FMESH_RELAY_SLOTS     16u

/* The assessment delay is drawn from MIN to MIN+span, and the span grows with
 * how many nodes we can hear.
 *
 * A fixed span does not survive density. At millisecond resolution a 100 ms
 * window offers 101 distinct delays, so once there are more nodes than that
 * several of them draw the same smallest value and transmit at the same
 * instant, before anyone can hear anyone else and stand down. Widening the
 * window with the crowd keeps simultaneous draws rare. The cost is latency,
 * paid only in the cells busy enough to need it. */
#define FMESH_DELAY_MIN_MS      20u
#define FMESH_DELAY_SPAN_MS     100u   /* an empty cell */
#define FMESH_DELAY_PER_PEER_MS 10u    /* widening per audible node */
#define FMESH_DELAY_SPAN_MAX_MS 1000u  /* ceiling, so latency stays bounded */

/* Above this the mesh is past the density it was tested at. */
#define FMESH_DENSITY_WARN    50u

typedef enum {
    FMESH_DROP = 0,      /* seen before, malformed, or not ours to repeat */
    FMESH_DELIVER,       /* addressed to us, hand it up */
    FMESH_BEACON,        /* a fresh beacon, the peer table was updated */
    FMESH_RELAY_QUEUED,  /* not ours, will be repeated unless somebody beats us */
} fmesh_action_t;

typedef struct {
    uint8_t  id[FCP_ID_LEN];
    char     name[FCP_NAME_MAX + 1];   /* from the beacon, cosmetic, NUL terminated */
    uint16_t boot_id;
    uint32_t seq;
    int8_t   rssi;
    uint8_t  hops;
    uint32_t last_heard_ms;
    bool     used;
} fmesh_peer_t;

typedef struct {
    uint8_t  src_id[FCP_ID_LEN];
    uint32_t msg_id;
    uint32_t at_ms;
    bool     used;
} fmesh_seen_t;

typedef struct {
    uint8_t  frame[FCP_MAX_FRAME];
    size_t   len;
    uint8_t  src_id[FCP_ID_LEN];
    uint32_t msg_id;
    uint32_t due_ms;
    bool     used;
} fmesh_relay_t;

typedef struct {
    uint32_t relayed;        /* frames we actually repeated */
    uint32_t suppressed;     /* repeats cancelled because someone beat us */
    uint32_t duplicates;     /* frames dropped as already seen */
    uint32_t malformed;      /* frames the codec refused */
    uint32_t replays;        /* beacons dropped for a stale sequence */
    uint32_t delivered;      /* frames addressed to us */
} fmesh_stats_t;

typedef struct fmesh fmesh_t;

struct fmesh {
    uint8_t       self_id[FCP_ID_LEN];
    fmesh_seen_t  seen[FMESH_DEDUP_SLOTS];
    fmesh_peer_t  peers[FMESH_PEER_SLOTS];
    fmesh_relay_t queue[FMESH_RELAY_SLOTS];
    fmesh_stats_t stats;

    /* Injected so tests are deterministic and the flood can be replayed. */
    uint32_t (*rand)(void *ctx);
    void     *rand_ctx;
};

/** A frame that came due and should go on the air now. */
typedef struct {
    const uint8_t *frame;
    size_t         len;
} fmesh_out_t;

void fmesh_init(fmesh_t *m, const uint8_t self_id[FCP_ID_LEN],
                uint32_t (*rand_fn)(void *), void *rand_ctx);

/**
 * Feed in one received frame.
 *
 * Also cancels any queued repeat of the same packet, which is the whole point:
 * hearing somebody else repeat it is the signal that we do not need to.
 */
fmesh_action_t fmesh_rx(fmesh_t *m, const uint8_t *frame, size_t len,
                        int8_t rssi, uint32_t now_ms);

/**
 * Collect repeats whose delay has expired. Returns how many were written to
 * `out`, each of which the caller should transmit.
 */
size_t fmesh_due(fmesh_t *m, uint32_t now_ms, fmesh_out_t *out, size_t max);

/** Peers heard within FMESH_PEER_TTL_MS. */
size_t fmesh_peer_count(const fmesh_t *m, uint32_t now_ms);

const fmesh_peer_t *fmesh_peer(const fmesh_t *m, const uint8_t id[FCP_ID_LEN]);

/** Number of repeats currently waiting on their assessment delay. */
size_t fmesh_queue_depth(const fmesh_t *m);

#ifdef __cplusplus
}
#endif
#endif /* FMESH_H */
