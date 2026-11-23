#include "tesla_pairing_state.h"

#include <assert.h>
#include <stdio.h>

static void test_happy_path_requires_vehicle_confirmation(void)
{
    tesla_pairing_state_t state = TESLA_PAIRING_IDLE;

    assert(!tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_VEHICLE_CONFIRMED));
    assert(state == TESLA_PAIRING_IDLE);
    assert(tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_VIN_ACCEPTED));
    assert(tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_SCAN_STARTED));
    assert(tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_VEHICLE_MATCHED));
    assert(tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_KEY_CREATED));
    assert(tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_REQUEST_SENT));
    assert(tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_CARD_TAP_REQUIRED));
    assert(state == TESLA_PAIRING_WAITING_FOR_CARD);
    assert(tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_VEHICLE_CONFIRMED));
    assert(state == TESLA_PAIRING_AUTHORIZED);
    assert(!tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_REQUEST_SENT));
}

static void test_invalid_events_do_not_advance(void)
{
    tesla_pairing_state_t state = TESLA_PAIRING_VIN_READY;
    assert(!tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_KEY_CREATED));
    assert(state == TESLA_PAIRING_VIN_READY);
    assert(tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_FAILED));
    assert(state == TESLA_PAIRING_ERROR);
    assert(tesla_pairing_state_apply(&state, TESLA_PAIRING_EVENT_RESET));
    assert(state == TESLA_PAIRING_IDLE);
}

int main(void)
{
    test_happy_path_requires_vehicle_confirmation();
    test_invalid_events_do_not_advance();
    assert(!tesla_pairing_state_apply(NULL, TESLA_PAIRING_EVENT_RESET));
    puts("tesla_pairing_state: PASS");
    return 0;
}
