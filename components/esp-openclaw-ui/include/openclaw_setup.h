#pragma once

#include "lvgl.h"
#include "esp_openclaw_setup_runtime.h"

typedef struct openclaw_setup openclaw_setup_t;
typedef struct { uint64_t request, generation, peer; } openclaw_setup_key_t;
typedef enum { OPENCLAW_SETUP_OPEN, OPENCLAW_SETUP_CANCEL, OPENCLAW_SETUP_RETRY,
    OPENCLAW_SETUP_NEUTRAL, OPENCLAW_SETUP_CONFIRM, OPENCLAW_SETUP_WAKE } openclaw_setup_action_t;
typedef esp_err_t (*openclaw_setup_action_cb_t)(void *context,
    openclaw_setup_action_t action, openclaw_setup_key_t *key);

/* One local setup view per device. All calls require the display lock.
 * Input drivers must exist before show and outlive this view. Their callbacks
 * and user data remain owned by the board; wrappers only observe fresh reads.
 * action must be bounded/nonblocking, never reenter this view, and route to the one setup runtime. OPEN
 * writes its admitted request ID to key before returning ESP_OK, so immediate
 * Close can revoke even before the next snapshot. Other actions only read key.
 * Never expose show/confirmation through remote display or Canvas commands. */
openclaw_setup_t *openclaw_setup_create(lv_obj_t *parent, bool has_boot_button,
    openclaw_setup_action_cb_t action, void *context);
bool openclaw_setup_show(openclaw_setup_t *view);
bool openclaw_setup_is_visible(const openclaw_setup_t *view);
void openclaw_setup_update(openclaw_setup_t *view,
    const esp_openclaw_setup_runtime_snapshot_t *snapshot, bool awake);
/* Leaving/replacing/covering the view must call close before showing another
 * overlay. Revocation is synchronous; cleanup belongs to the runtime task. */
void openclaw_setup_close(openclaw_setup_t *view);
/* Destroy before deleting the parent or any input device. */
void openclaw_setup_destroy(openclaw_setup_t *view);
/* NEW physical BOOT samples, under the display lock. true means consumed;
 * suppress the board's normal Canvas toggle. BOOT cancels, never confirms. */
bool openclaw_setup_boot_sample(openclaw_setup_t *view, bool pressed);
