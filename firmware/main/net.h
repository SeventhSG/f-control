/* The radio side: ESP-NOW in, ESP-NOW out, beacons, and the relay pump.
 *
 * Everything above this file deals in peers and text. Everything below it
 * deals in frames. The decision logic lives in fmesh, which knows nothing
 * about ESP-NOW, so swapping in LoRa later means writing a sibling to this
 * file and nothing else.
 */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fmesh.h"
#include "ident.h"

#define NET_CHANNEL       1u      /* the mesh rendezvous channel */
#define NET_BEACON_MS     5000u
#define NET_BEACON_JITTER 1000u
#define NET_TEXT_MAX      FCP_PLAINTEXT_MAX

/** A message that arrived for us. `text` is not NUL terminated. */
typedef void (*net_msg_cb_t)(const uint8_t src_id[FCP_ID_LEN],
                             const char *text, size_t len);

/** Somebody's beacon changed the roster. */
typedef void (*net_roster_cb_t)(void);

/** Our own network key just changed because a confirmed contact shared theirs
 *  with us. See net_share_key for exactly what this does and does not mean. */
typedef void (*net_keyshare_cb_t)(void);

void net_start(ident_t *id, net_msg_cb_t on_msg, net_roster_cb_t on_roster,
              net_keyshare_cb_t on_keyshare);

/** Picks up whatever key is currently stored in NVS. Call after
 *  netkey_set_passphrase() or netkey_set_random() so a live key change takes
 *  effect on the next message immediately, with no reboot. */
void net_reload_key(void);

/**
 * Sends our current network key, in the clear, to one specific peer. Called
 * when a person presses Confirm, so that two people who just verified each
 * other in person end up able to read each other without either of them
 * typing a passphrase by hand. See the long comment beside the
 * implementation in net.c for exactly what this protects and does not.
 */
void net_share_key(const uint8_t dst_id[FCP_ID_LEN]);

/** Copy the live roster. Returns how many were written. */
size_t net_roster(fmesh_peer_t *out, size_t max);

/**
 * Queue a message for one peer. Returns the message id, or 0 if the text does
 * not fit. Delivery is not confirmed here: an ACK arrives later, or does not.
 */
uint32_t net_send_text(const uint8_t dst_id[FCP_ID_LEN], const char *text, size_t len);

/** Counters, for the diagnostics line in the dashboard. */
void net_stats(fmesh_stats_t *out);

#endif /* NET_H */
