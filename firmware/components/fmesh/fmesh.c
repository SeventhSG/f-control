#include "fmesh.h"

#include <string.h>

static bool id_eq(const uint8_t a[FCP_ID_LEN], const uint8_t b[FCP_ID_LEN]) {
    return memcmp(a, b, FCP_ID_LEN) == 0;
}

/* Wrap safe comparison. now_ms comes from a millisecond counter that rolls
 * over roughly every 49 days, and a naive `now - then > ttl` goes badly wrong
 * across that boundary. */
static bool older_than(uint32_t then_ms, uint32_t now_ms, uint32_t ttl) {
    return (uint32_t)(now_ms - then_ms) > ttl;
}

void fmesh_init(fmesh_t *m, const uint8_t self_id[FCP_ID_LEN],
                uint32_t (*rand_fn)(void *), void *rand_ctx) {
    if (m == NULL) return;
    memset(m, 0, sizeof(*m));
    if (self_id != NULL) memcpy(m->self_id, self_id, FCP_ID_LEN);
    m->rand = rand_fn;
    m->rand_ctx = rand_ctx;
}

/* ---- dedup ring -------------------------------------------------------- */

static fmesh_seen_t *seen_find(fmesh_t *m, const uint8_t src[FCP_ID_LEN], uint32_t msg_id,
                               uint32_t now_ms) {
    for (size_t i = 0; i < FMESH_DEDUP_SLOTS; i++) {
        fmesh_seen_t *s = &m->seen[i];
        if (!s->used) continue;
        if (older_than(s->at_ms, now_ms, FMESH_DEDUP_TTL_MS)) { s->used = false; continue; }
        if (s->msg_id == msg_id && id_eq(s->src_id, src)) return s;
    }
    return NULL;
}

static void seen_add(fmesh_t *m, const uint8_t src[FCP_ID_LEN], uint32_t msg_id,
                     uint32_t now_ms) {
    fmesh_seen_t *victim = NULL;

    for (size_t i = 0; i < FMESH_DEDUP_SLOTS; i++) {
        fmesh_seen_t *s = &m->seen[i];
        if (!s->used || older_than(s->at_ms, now_ms, FMESH_DEDUP_TTL_MS)) { victim = s; break; }
        /* Otherwise remember the oldest, so a full ring evicts by age rather
         * than by whichever slot happened to come first. */
        if (victim == NULL || (uint32_t)(now_ms - s->at_ms) > (uint32_t)(now_ms - victim->at_ms)) {
            victim = s;
        }
    }
    if (victim == NULL) victim = &m->seen[0];

    memcpy(victim->src_id, src, FCP_ID_LEN);
    victim->msg_id = msg_id;
    victim->at_ms  = now_ms;
    victim->used   = true;
}

/* ---- peer table -------------------------------------------------------- */

static fmesh_peer_t *peer_slot(fmesh_t *m, const uint8_t id[FCP_ID_LEN], uint32_t now_ms) {
    fmesh_peer_t *free_slot = NULL;
    fmesh_peer_t *stale = NULL;

    for (size_t i = 0; i < FMESH_PEER_SLOTS; i++) {
        fmesh_peer_t *p = &m->peers[i];
        if (p->used && id_eq(p->id, id)) return p;
        if (!p->used && free_slot == NULL) free_slot = p;
        if (p->used && older_than(p->last_heard_ms, now_ms, FMESH_PEER_TTL_MS)) {
            if (stale == NULL || p->last_heard_ms < stale->last_heard_ms) stale = p;
        }
    }

    fmesh_peer_t *slot = free_slot ? free_slot : stale;
    if (slot == NULL) return NULL;   /* table full of live peers, keep them */

    memset(slot, 0, sizeof(*slot));
    memcpy(slot->id, id, FCP_ID_LEN);
    slot->used = true;
    return slot;
}

const fmesh_peer_t *fmesh_peer(const fmesh_t *m, const uint8_t id[FCP_ID_LEN]) {
    if (m == NULL || id == NULL) return NULL;
    for (size_t i = 0; i < FMESH_PEER_SLOTS; i++) {
        const fmesh_peer_t *p = &m->peers[i];
        if (p->used && id_eq(p->id, id)) return p;
    }
    return NULL;
}

size_t fmesh_peer_count(const fmesh_t *m, uint32_t now_ms) {
    if (m == NULL) return 0;
    size_t n = 0;
    for (size_t i = 0; i < FMESH_PEER_SLOTS; i++) {
        const fmesh_peer_t *p = &m->peers[i];
        if (p->used && !older_than(p->last_heard_ms, now_ms, FMESH_PEER_TTL_MS)) n++;
    }
    return n;
}

/* ---- relay queue ------------------------------------------------------- */

size_t fmesh_queue_depth(const fmesh_t *m) {
    if (m == NULL) return 0;
    size_t n = 0;
    for (size_t i = 0; i < FMESH_RELAY_SLOTS; i++) if (m->queue[i].used) n++;
    return n;
}

/* Somebody else repeated this packet, so we do not have to. This is the
 * mechanism that keeps a dense cell from turning every packet into N
 * transmissions. */
static bool cancel_queued(fmesh_t *m, const uint8_t src[FCP_ID_LEN], uint32_t msg_id) {
    for (size_t i = 0; i < FMESH_RELAY_SLOTS; i++) {
        fmesh_relay_t *r = &m->queue[i];
        if (r->used && r->msg_id == msg_id && id_eq(r->src_id, src)) {
            r->used = false;
            m->stats.suppressed++;
            return true;
        }
    }
    return false;
}

static bool queue_relay(fmesh_t *m, const uint8_t *frame, size_t len,
                        const uint8_t src[FCP_ID_LEN], uint32_t msg_id, uint32_t now_ms) {
    fmesh_relay_t *slot = NULL;
    for (size_t i = 0; i < FMESH_RELAY_SLOTS; i++) {
        if (!m->queue[i].used) { slot = &m->queue[i]; break; }
    }
    /* A full queue means the cell is busier than we can repeat for. Dropping
     * is correct: the packet still reaches everyone in direct range, and
     * adding to a backlog we cannot drain would only make the channel worse. */
    if (slot == NULL) return false;

    memcpy(slot->frame, frame, len);
    slot->len = len;
    memcpy(slot->src_id, src, FCP_ID_LEN);
    slot->msg_id = msg_id;

    /* Widen the contention window with the size of the crowd. See the note in
     * fmesh.h: a fixed window has too few distinct delays to keep a hundred
     * nodes from picking the same one. */
    uint32_t span = FMESH_DELAY_SPAN_MS
                  + (uint32_t)fmesh_peer_count(m, now_ms) * FMESH_DELAY_PER_PEER_MS;
    if (span > FMESH_DELAY_SPAN_MAX_MS) span = FMESH_DELAY_SPAN_MAX_MS;

    const uint32_t jitter = m->rand ? (m->rand(m->rand_ctx) % (span + 1u)) : (span / 2u);
    slot->due_ms = now_ms + FMESH_DELAY_MIN_MS + jitter;
    slot->used = true;
    return true;
}

/* ---- receive ----------------------------------------------------------- */

static fmesh_action_t handle_beacon(fmesh_t *m, const fcp_hdr_t *h, const uint8_t *payload,
                                    size_t payload_len, int8_t rssi, uint32_t now_ms) {
    fcp_beacon_t b;
    if (fcp_beacon_decode(payload, payload_len, &b) != FCP_OK) {
        m->stats.malformed++;
        return FMESH_DROP;
    }

    fmesh_peer_t *p = peer_slot(m, h->src_id, now_ms);
    if (p == NULL) return FMESH_DROP;

    /* Fresh boot resets the sequence. Without this a board that reboots is
     * rejected as a replayer by everyone who remembers its old sequence, and
     * never reappears in anyone's roster. */
    const bool fresh_boot = !p->last_heard_ms || p->boot_id != b.boot_id;

    if (!fresh_boot && b.seq <= p->seq) {
        m->stats.replays++;
        return FMESH_DROP;
    }

    p->boot_id       = b.boot_id;
    p->seq           = b.seq;
    p->rssi          = rssi;
    p->hops          = h->hop_count;
    p->last_heard_ms = now_ms ? now_ms : 1u;   /* 0 doubles as "never heard" */

    return FMESH_BEACON;
}

fmesh_action_t fmesh_rx(fmesh_t *m, const uint8_t *frame, size_t len,
                        int8_t rssi, uint32_t now_ms) {
    if (m == NULL || frame == NULL) return FMESH_DROP;

    fcp_hdr_t h;
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (fcp_frame_decode(frame, len, &h, &payload, &payload_len) != FCP_OK) {
        m->stats.malformed++;
        return FMESH_DROP;
    }

    /* Our own packet, echoed back by a relay. */
    if (id_eq(h.src_id, m->self_id)) {
        cancel_queued(m, h.src_id, h.msg_id);
        return FMESH_DROP;
    }

    if (h.type == FCP_T_BEACON) {
        /* Hearing a beacon repeated is also the cancel signal for our own
         * pending repeat of it. */
        cancel_queued(m, h.src_id, h.msg_id);

        const fmesh_action_t act = handle_beacon(m, &h, payload, payload_len, rssi, now_ms);
        if (act != FMESH_BEACON) return act;

        if (h.hop_limit > 0u && h.hop_count == 0u) {
            /* Beacons travel exactly one hop. Any further and the roster fills
             * with people there is no path to. */
            uint8_t copy[FCP_MAX_FRAME];
            memcpy(copy, frame, len);
            if (fcp_frame_step_hop(copy, len)) {
                queue_relay(m, copy, len, h.src_id, h.msg_id, now_ms);
            }
        }
        return FMESH_BEACON;
    }

    /* Addressed traffic. Hearing it at all cancels any repeat we had pending,
     * whether it was the original or someone else's repeat. */
    const bool was_pending = cancel_queued(m, h.src_id, h.msg_id);

    if (seen_find(m, h.src_id, h.msg_id, now_ms) != NULL) {
        m->stats.duplicates++;
        return FMESH_DROP;
    }
    seen_add(m, h.src_id, h.msg_id, now_ms);

    if (id_eq(h.dst_id, m->self_id)) {
        m->stats.delivered++;
        return FMESH_DELIVER;
    }

    if (was_pending) return FMESH_DROP;

    if (h.hop_limit == 0u) return FMESH_DROP;

    uint8_t copy[FCP_MAX_FRAME];
    memcpy(copy, frame, len);
    if (!fcp_frame_step_hop(copy, len)) return FMESH_DROP;

    return queue_relay(m, copy, len, h.src_id, h.msg_id, now_ms)
               ? FMESH_RELAY_QUEUED
               : FMESH_DROP;
}

size_t fmesh_due(fmesh_t *m, uint32_t now_ms, fmesh_out_t *out, size_t max) {
    if (m == NULL || out == NULL) return 0;

    size_t n = 0;
    for (size_t i = 0; i < FMESH_RELAY_SLOTS && n < max; i++) {
        fmesh_relay_t *r = &m->queue[i];
        if (!r->used) continue;
        if ((int32_t)(now_ms - r->due_ms) < 0) continue;   /* wrap safe */

        out[n].frame = r->frame;
        out[n].len   = r->len;
        n++;

        r->used = false;
        m->stats.relayed++;
    }
    return n;
}
