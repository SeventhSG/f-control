/* Lets the flasher hand this board a name, an access point name, a home
 * network, and a shared encryption key over the same USB serial connection it
 * was just flashed through, so the person never has to open the dashboard and
 * type any of it by hand.
 *
 * The board listens on the console UART for one line:
 *
 *   FCPROV {"name":"ozan","ap":"","ssid":"A1_98AA","pass":"...","netkey":"..."}
 *
 * Every field is optional. A field that is absent or empty is left alone. On
 * success the board prints "FCPROV-OK" and restarts, since every field here
 * is either applied at boot (the access point name, the wifi credentials) or
 * safest applied consistently from a clean boot (the network key, so nothing
 * sent between provisioning and the reboot was ever sealed under a stale one).
 */
#ifndef PROVISION_H
#define PROVISION_H

#include "ident.h"

void provision_start(ident_t *id);

#endif /* PROVISION_H */
