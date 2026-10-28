#pragma once

typedef enum {
    DIAL_MESSAGING_OFF,
    DIAL_MESSAGING_REQUESTING,
    DIAL_MESSAGING_WAITING,
    DIAL_MESSAGING_CONNECTING,
    DIAL_MESSAGING_READY,
    DIAL_MESSAGING_REJECTED,
    DIAL_MESSAGING_EXPIRED,
    DIAL_MESSAGING_ERROR,
} dial_messaging_state_t;

typedef struct {
    dial_messaging_state_t state;
    unsigned revision;
    char request_id[129];
} dial_messaging_snapshot_t;
