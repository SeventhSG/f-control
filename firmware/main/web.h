/* Access point, web server, and the dashboard websocket.
 *
 * The access point is OPEN in this build, with no WPA2 passphrase. The spec
 * calls for WPA2 and it will get it. Adding a password here would be theatre:
 * this firmware has no encryption on the radio either, so anything a
 * passphrase protected on the wifi link travels in clear over ESP-NOW two
 * milliseconds later. Both go in together, or neither means anything.
 */
#ifndef WEB_H
#define WEB_H

#include <stddef.h>
#include <stdint.h>

#include "fcp.h"
#include "ident.h"

void web_start(ident_t *id);

/** Push the current roster to every connected dashboard. */
void web_send_roster(void);

/** A message arrived over the radio. */
void web_on_message(const uint8_t src_id[FCP_ID_LEN], const char *text, size_t len);

/** Our network key just changed because a confirmed contact shared theirs. */
void web_on_keyshare(void);

#endif /* WEB_H */
