#include "esp_openclaw_setup_mailbox.h"
#include "esp_openclaw_setup_security2.h"

#include <stdlib.h>
#include <string.h>

static void wipe(void *memory, size_t size)
{ volatile uint8_t *bytes = memory; for (size_t i = 0; i < size; ++i) bytes[i] = 0; }

static void clear(esp_openclaw_setup_mailbox_t *mailbox)
{
    wipe(mailbox->request, sizeof(mailbox->request)); wipe(mailbox->reply, sizeof(mailbox->reply));
    mailbox->request_size = mailbox->reply_size = 0;
    mailbox->delivered = mailbox->reply_pending = false; mailbox->phase = ESP_OPENCLAW_MAILBOX_EMPTY;
}

/* Slot lock held. PROCESSING bytes belong to the bounded owner transfer call;
 * revocation takes effect now, erasure follows when that call releases them. */
static bool live(esp_openclaw_setup_mailbox_t *mailbox)
{
    if (mailbox->open && esp_openclaw_setup_time_is_current(mailbox->opened_ms,
            mailbox->now_ms(mailbox->context)) &&
            (!mailbox->peer || esp_openclaw_setup_security2_peer_is_current(mailbox->peer))) return true;
    mailbox->open = false;
    esp_openclaw_setup_security2_revoke_peer(mailbox->peer);
    if (mailbox->phase != ESP_OPENCLAW_MAILBOX_PROCESSING) clear(mailbox);
    return false;
}

bool esp_openclaw_setup_mailbox_init(esp_openclaw_setup_mailbox_t *mailbox,
    esp_openclaw_setup_transfer_t *transfer, esp_openclaw_setup_dispatcher_t *dispatcher,
    uint64_t (*now_ms)(void *context), void *context)
{
    if (!mailbox || !transfer || !dispatcher || !now_ms) return false;
    *mailbox = (esp_openclaw_setup_mailbox_t){.lock = portMUX_INITIALIZER_UNLOCKED,
        .transfer = transfer, .dispatcher = dispatcher, .now_ms = now_ms, .context = context};
    return true;
}

bool esp_openclaw_setup_mailbox_open(esp_openclaw_setup_mailbox_t *mailbox,
    uint64_t generation, uint64_t peer, uint64_t opened_ms)
{
    portENTER_CRITICAL(&mailbox->lock);
    bool fresh = !mailbox->open && generation > mailbox->generation;
    bool bind = mailbox->open && !mailbox->peer && peer && generation == mailbox->generation && opened_ms == mailbox->opened_ms;
    bool admitted = (fresh || bind) && mailbox->phase != ESP_OPENCLAW_MAILBOX_PROCESSING &&
        (!peer || esp_openclaw_setup_security2_peer_is_current(peer)) &&
        esp_openclaw_setup_time_is_current(opened_ms, mailbox->now_ms(mailbox->context));
    if (admitted) {
        clear(mailbox); mailbox->generation = generation; mailbox->peer = peer;
        mailbox->opened_ms = opened_ms; mailbox->open = true;
    }
    portEXIT_CRITICAL(&mailbox->lock);
    return admitted;
}

bool esp_openclaw_setup_mailbox_close(esp_openclaw_setup_mailbox_t *mailbox, uint64_t generation, uint64_t peer)
{
    portENTER_CRITICAL(&mailbox->lock);
    bool matching = generation == mailbox->generation && peer == mailbox->peer;
    if (matching) {
        mailbox->open = false;
        esp_openclaw_setup_security2_revoke_peer(peer);
        if (mailbox->phase != ESP_OPENCLAW_MAILBOX_PROCESSING) clear(mailbox);
    }
    bool drained = matching && mailbox->phase == ESP_OPENCLAW_MAILBOX_EMPTY;
    portEXIT_CRITICAL(&mailbox->lock);
    return drained;
}

static void error_reply(uint8_t *output, size_t *size, uint32_t id, esp_openclaw_setup_frame_error_t code)
{
    uint8_t detail[2] = {0, (uint8_t)code};
    esp_openclaw_setup_frame_t frame = {.kind = ESP_OPENCLAW_SETUP_FRAME_ERROR,
        .transfer_id = id, .payload_size = sizeof(detail), .payload = detail};
    esp_openclaw_setup_frame_encode(&frame, output, ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX, size);
}

esp_err_t esp_openclaw_setup_mailbox_endpoint(uint32_t session_id, const uint8_t *input,
    ssize_t size, uint8_t **output, ssize_t *output_size, void *context)
{
    esp_openclaw_setup_mailbox_t *mailbox = context;
    esp_openclaw_setup_frame_t frame;
    if (!mailbox || !output || !output_size || size <= 0 || size > ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX ||
        !esp_openclaw_setup_frame_decode(input, (size_t)size, &frame) ||
        (frame.kind != ESP_OPENCLAW_SETUP_FRAME_REQUEST && frame.kind != ESP_OPENCLAW_SETUP_FRAME_PULL)) return ESP_ERR_INVALID_ARG;
    /* Allocate before admission: failure cannot leave a newly accepted write. */
    uint8_t *bytes = malloc(ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX);
    if (!bytes) return ESP_ERR_NO_MEM;
    size_t length = 0;
    portENTER_CRITICAL(&mailbox->lock);
    uint64_t peer = esp_openclaw_setup_security2_peer_for_session(session_id);
    bool authorized = live(mailbox) && peer && (!mailbox->peer || peer == mailbox->peer);
    if (authorized) {
        bool same = mailbox->request_size == (size_t)size && memcmp(mailbox->request, input, (size_t)size) == 0;
        if (!mailbox->peer) {
            /* First authenticated frame precedes owner attach/bind. Retain
             * nothing and admit nothing; the phone retries it after binding. */
            error_reply(bytes, &length, frame.transfer_id, ESP_OPENCLAW_SETUP_FRAME_PENDING);
        } else if (same && mailbox->phase == ESP_OPENCLAW_MAILBOX_READY && !mailbox->reply_pending) {
            memcpy(bytes, mailbox->reply, mailbox->reply_size); length = mailbox->reply_size;
            mailbox->delivered = true;
        } else if (mailbox->phase == ESP_OPENCLAW_MAILBOX_EMPTY ||
                (mailbox->phase == ESP_OPENCLAW_MAILBOX_READY && mailbox->delivered)) {
            clear(mailbox); memcpy(mailbox->request, input, (size_t)size); mailbox->request_size = (size_t)size;
            mailbox->phase = ESP_OPENCLAW_MAILBOX_QUEUED;
            error_reply(bytes, &length, frame.transfer_id, ESP_OPENCLAW_SETUP_FRAME_PENDING);
        } else {
            error_reply(bytes, &length, frame.transfer_id, same ? ESP_OPENCLAW_SETUP_FRAME_PENDING : ESP_OPENCLAW_SETUP_FRAME_BUSY);
        }
        authorized = live(mailbox) && esp_openclaw_setup_security2_peer_for_session(session_id) == peer;
    }
    portEXIT_CRITICAL(&mailbox->lock);
    if (!authorized) { wipe(bytes, ESP_OPENCLAW_SETUP_FRAME_SIZE_MAX); free(bytes); return ESP_ERR_INVALID_STATE; }
    *output = bytes; *output_size = (ssize_t)length;
    return ESP_OK; /* SDK encryption rechecks authority after this callback. */
}

void esp_openclaw_setup_mailbox_poll(esp_openclaw_setup_mailbox_t *mailbox)
{
    portENTER_CRITICAL(&mailbox->lock);
    bool queued = live(mailbox) && mailbox->phase == ESP_OPENCLAW_MAILBOX_QUEUED;
    uint64_t generation = mailbox->generation, peer = mailbox->peer;
    if (queued) mailbox->phase = ESP_OPENCLAW_MAILBOX_PROCESSING;
    portEXIT_CRITICAL(&mailbox->lock);
    if (!queued) return;

    esp_openclaw_setup_transfer_request_t handoff = {0};
    size_t size = 0;
    bool received = esp_openclaw_setup_transfer_receive(mailbox->transfer, generation, peer,
        mailbox->request, mailbox->request_size, mailbox->reply, sizeof(mailbox->reply), &size, &handoff);
    esp_openclaw_setup_frame_t reply;
    bool pending = received && esp_openclaw_setup_frame_decode(mailbox->reply, size, &reply) &&
        reply.kind == ESP_OPENCLAW_SETUP_FRAME_ERROR && reply.payload[1] == ESP_OPENCLAW_SETUP_FRAME_PENDING;
    portENTER_CRITICAL(&mailbox->lock);
    bool publish = live(mailbox) && received;
    if (publish) {
        mailbox->reply_size = size; mailbox->phase = ESP_OPENCLAW_MAILBOX_READY;
        /* Initial enqueue already returned PENDING. Reevaluate it on the next
         * retry, or allow a missing fragment to replace a premature pull. */
        mailbox->delivered = mailbox->reply_pending = pending;
    } else {
        mailbox->open = false; esp_openclaw_setup_security2_revoke_peer(peer); clear(mailbox);
    }
    portEXIT_CRITICAL(&mailbox->lock);
    /* ACK is now retrievable while dispatch waits. Handoff borrows canonical
     * transfer storage, never mailbox input that the BLE callback may replace. */
    if (publish && handoff.size && !esp_openclaw_setup_dispatcher_handle(mailbox->dispatcher, &handoff))
        esp_openclaw_setup_mailbox_close(mailbox, generation, peer);
}
