#pragma once

#include <stdbool.h>

typedef enum {
    TESLA_PAIRING_IDLE = 0,
    TESLA_PAIRING_VIN_READY,
    TESLA_PAIRING_SCANNING,
    TESLA_PAIRING_VEHICLE_FOUND,
    TESLA_PAIRING_KEY_READY,
    TESLA_PAIRING_REQUEST_SENT,
    TESLA_PAIRING_WAITING_FOR_CARD,
    TESLA_PAIRING_AUTHORIZED,
    TESLA_PAIRING_ERROR,
} tesla_pairing_state_t;

typedef enum {
    TESLA_PAIRING_EVENT_VIN_ACCEPTED = 0,
    TESLA_PAIRING_EVENT_SCAN_STARTED,
    TESLA_PAIRING_EVENT_VEHICLE_MATCHED,
    TESLA_PAIRING_EVENT_KEY_CREATED,
    TESLA_PAIRING_EVENT_REQUEST_SENT,
    TESLA_PAIRING_EVENT_CARD_TAP_REQUIRED,
    TESLA_PAIRING_EVENT_VEHICLE_CONFIRMED,
    TESLA_PAIRING_EVENT_FAILED,
    TESLA_PAIRING_EVENT_RESET,
} tesla_pairing_event_t;

/*
 * Apply one observed workflow event. Invalid or out-of-order events are
 * rejected without changing state. AUTHORIZED is reachable only after the
 * vehicle reports the card-confirmation step complete.
 */
bool tesla_pairing_state_apply(tesla_pairing_state_t *state,
                               tesla_pairing_event_t event);
