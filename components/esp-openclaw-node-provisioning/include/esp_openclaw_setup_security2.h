#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "protocomm_security.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One native BLE setup server per boot. NULL when native S3 Wi-Fi, BLE/NimBLE,
 * Security2 or dynamic log suppression is unavailable. Delegates all crypto to
 * IDF Security2 patch1. This does not start BLE, attach/confirm a setup session,
 * generate credentials, or create a comparison challenge. */
const protocomm_security_t *esp_openclaw_setup_security2(void);
/* Suppress sensitive SDK tags BEFORE SRP parameter generation or constructing
 * protocomm. Require true before either action; false includes SDK log-level
 * allocation failure. init() also requires this before delegating to the SDK.
 * No other caller may reenable these tags while setup exists. */
bool esp_openclaw_setup_security2_quiet_logs(void);
/* Cross-task authority queries. First successful application decrypt creates a
 * nonzero boot-unique peer, never a BLE address/connection ID. Cleanup/start
 * cannot reset its counter. Only canonical setup attach grants session access. */
uint64_t esp_openclaw_setup_security2_peer(void);
/* SDK endpoint: require the exact transport ID after SDK decryption. */
uint64_t esp_openclaw_setup_security2_peer_for_session(uint32_t session_id);
bool esp_openclaw_setup_security2_peer_is_current(uint64_t peer);
/* Stale mailbox/lifecycle work cannot revoke a replacement authenticated peer. */
bool esp_openclaw_setup_security2_revoke_peer(uint64_t peer);
/* Local server owner: synchronously revoke BEFORE awaiting transport stop.
 * Callbacks/lifecycle must serialize on the BLE host; cleanup requires those
 * callbacks to have drained. No SDK callback may run after cleanup returns. */
void esp_openclaw_setup_security2_revoke(void);

#ifdef __cplusplus
}
#endif
