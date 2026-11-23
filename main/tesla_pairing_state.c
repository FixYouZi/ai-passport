#include "tesla_pairing_state.h"

bool tesla_pairing_state_apply(tesla_pairing_state_t *state,
                               tesla_pairing_event_t event)
{
    if (!state) return false;

    if (event == TESLA_PAIRING_EVENT_RESET) {
        *state = TESLA_PAIRING_IDLE;
        return true;
    }
    if (event == TESLA_PAIRING_EVENT_FAILED) {
        if (*state == TESLA_PAIRING_IDLE) return false;
        *state = TESLA_PAIRING_ERROR;
        return true;
    }

    switch (*state) {
    case TESLA_PAIRING_IDLE:
    case TESLA_PAIRING_ERROR:
        if (event == TESLA_PAIRING_EVENT_VIN_ACCEPTED) {
            *state = TESLA_PAIRING_VIN_READY;
            return true;
        }
        break;
    case TESLA_PAIRING_VIN_READY:
        if (event == TESLA_PAIRING_EVENT_SCAN_STARTED) {
            *state = TESLA_PAIRING_SCANNING;
            return true;
        }
        break;
    case TESLA_PAIRING_SCANNING:
        if (event == TESLA_PAIRING_EVENT_VEHICLE_MATCHED) {
            *state = TESLA_PAIRING_VEHICLE_FOUND;
            return true;
        }
        break;
    case TESLA_PAIRING_VEHICLE_FOUND:
        if (event == TESLA_PAIRING_EVENT_KEY_CREATED) {
            *state = TESLA_PAIRING_KEY_READY;
            return true;
        }
        break;
    case TESLA_PAIRING_KEY_READY:
        if (event == TESLA_PAIRING_EVENT_REQUEST_SENT) {
            *state = TESLA_PAIRING_REQUEST_SENT;
            return true;
        }
        break;
    case TESLA_PAIRING_REQUEST_SENT:
        if (event == TESLA_PAIRING_EVENT_CARD_TAP_REQUIRED) {
            *state = TESLA_PAIRING_WAITING_FOR_CARD;
            return true;
        }
        break;
    case TESLA_PAIRING_WAITING_FOR_CARD:
        if (event == TESLA_PAIRING_EVENT_VEHICLE_CONFIRMED) {
            *state = TESLA_PAIRING_AUTHORIZED;
            return true;
        }
        break;
    case TESLA_PAIRING_AUTHORIZED:
        break;
    }
    return false;
}
