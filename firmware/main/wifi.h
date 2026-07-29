/* Joining a network you already have.
 *
 * One radio, one channel. When this board joins your wifi, ESP-NOW moves to
 * that network's channel along with it, because it has no choice. That is not
 * automatically a problem: two boards on the SAME network land on the SAME
 * channel and the mesh works normally. It is only a problem when they are on
 * different networks, and the interface says so plainly rather than letting
 * somebody discover it as a silent failure.
 *
 * The duty cycled rendezvous described in the spec, where a station mode board
 * hops back to channel 1 periodically to stay reachable from boards on their
 * own access points, is NOT implemented here.
 */
#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 63
#define WIFI_SCAN_MAX 16

typedef struct {
    char    ssid[WIFI_SSID_MAX + 1];
    int8_t  rssi;
    bool    secure;
} wifi_found_t;

typedef struct {
    bool   joined;                     /* associated and holding an address */
    bool   configured;                 /* credentials stored, may still be trying */
    char   ssid[WIFI_SSID_MAX + 1];
    char   ip[16];
    uint8_t channel;                   /* the channel the radio is actually on */
} wifi_status_t;

/** Brings up the access point, then joins a stored network if there is one. */
void wifi_start(const char *ap_ssid, uint8_t ap_channel);

void wifi_status(wifi_status_t *out);

/** Blocking scan. Returns how many entries were written. */
size_t wifi_scan(wifi_found_t *out, size_t max);

/** Stores the credentials and tries them. Survives a reboot. */
bool wifi_join(const char *ssid, const char *password);

/** Forgets the stored network and returns to access point only. */
void wifi_leave(void);

#endif /* WIFI_H */
