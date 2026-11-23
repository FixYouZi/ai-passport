// tests/test_tesla_proto.c
// Tesla VCSEC protobuf 编码/解析的字节级断言（纯主机测试）。
#include "tesla_proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_rke_encoding(void)
{
    uint8_t out[8];

    size_t len = tesla_proto_rke(TESLA_RKE_UNLOCK, out, sizeof(out));
    assert(len == 2);
    const uint8_t unlock[] = {0x10, 0x00};
    assert(memcmp(out, unlock, sizeof(unlock)) == 0);

    len = tesla_proto_rke(TESLA_RKE_LOCK, out, sizeof(out));
    assert(len == 2);
    const uint8_t lock[] = {0x10, 0x01};
    assert(memcmp(out, lock, sizeof(lock)) == 0);

    // 缓冲区不足时必须失败而不是截断。
    assert(tesla_proto_rke(TESLA_RKE_UNLOCK, out, 1) == 0);
}

static void test_closure_encoding(void)
{
    uint8_t out[8];

    size_t len = tesla_proto_open_frunk(out, sizeof(out));
    assert(len == 4);
    const uint8_t frunk[] = {0x22, 0x02, 0x30, 0x01};
    assert(memcmp(out, frunk, sizeof(frunk)) == 0);

    len = tesla_proto_open_trunk(out, sizeof(out));
    assert(len == 4);
    const uint8_t trunk[] = {0x22, 0x02, 0x28, 0x01};
    assert(memcmp(out, trunk, sizeof(trunk)) == 0);

    len = tesla_proto_close_trunk(out, sizeof(out));
    assert(len == 4);
    const uint8_t close[] = {0x22, 0x02, 0x28, 0x04};
    assert(memcmp(out, close, sizeof(close)) == 0);
}

static void test_status_encoding(void)
{
    uint8_t out[8];
    size_t len = tesla_proto_info_status(out, sizeof(out));
    assert(len == 4);
    const uint8_t status[] = {0x0A, 0x02, 0x08, 0x00};
    assert(memcmp(out, status, sizeof(status)) == 0);
}

static void test_whitelist_add_key(void)
{
    uint8_t pub[TESLA_PUBKEY_LEN];
    for (int i = 0; i < TESLA_PUBKEY_LEN; i++) {
        pub[i] = (uint8_t)i;
    }
    uint8_t out[160];
    size_t len = tesla_proto_whitelist_add_key(pub, TESLA_KEY_FORM_ANDROID, out, sizeof(out));
    assert(len == 82);

    // UnsignedMessage.WhitelistOperation = 16
    assert(out[0] == 0x82 && out[1] == 0x01);
    assert(out[2] == 0x4F);
    // WhitelistOperation.addKeyToWhitelistAndAddPermissions = 5
    assert(out[3] == 0x2A && out[4] == 0x49);
    // PermissionChange.key = 1 -> PublicKey.PublicKeyRaw = 1
    assert(out[5] == 0x0A && out[6] == 0x43);
    assert(out[7] == 0x0A && out[8] == 0x41);
    assert(memcmp(out + 9, pub, TESLA_PUBKEY_LEN) == 0);
    // secondsToBeActive = 0，keyRole = ROLE_OWNER(2)
    assert(out[74] == 0x18 && out[75] == 0x00);
    assert(out[76] == 0x20 && out[77] == 0x02);
    // KeyMetadata.keyFormFactor = KEY_FORM_FACTOR_ANDROID(7)
    assert(out[78] == 0x32 && out[79] == 0x02);
    assert(out[80] == 0x08 && out[81] == 0x07);
}

static void test_vcsec_envelope(void)
{
    uint8_t msg[8] = {0x10, 0x01};
    uint8_t out[64];
    size_t len = tesla_proto_vcsec_envelope(msg, 2, TESLA_VCSEC_SIG_PRESENT_KEY, out, sizeof(out));
    assert(len == 8);
    // ToVCSECMessage.signedMessage = 1
    assert(out[0] == 0x0A && out[1] == 0x06);
    // SignedMessage.protobufMessageAsBytes = 2
    assert(out[2] == 0x12 && out[3] == 0x02);
    assert(memcmp(out + 4, msg, 2) == 0);
    // SignedMessage.signatureType = 3, PRESENT_KEY = 2
    assert(out[6] == 0x18 && out[7] == 0x02);
}

static void test_session_info_request(void)
{
    uint8_t pub[TESLA_PUBKEY_LEN];
    uint8_t uuid[TESLA_UUID_LEN];
    uint8_t routing[TESLA_UUID_LEN];
    memset(pub, 0xA5, sizeof(pub));
    memset(uuid, 0x11, sizeof(uuid));
    memset(routing, 0x22, sizeof(routing));

    uint8_t out[256];
    size_t len = tesla_proto_session_info_request(pub, uuid, routing, out, sizeof(out));
    assert(len == 112);

    // to_destination = 6 { domain = VCSEC(2) }
    assert(out[0] == 0x32 && out[1] == 0x02);
    assert(out[2] == 0x08 && out[3] == 0x02);
    // from_destination = 7 { routing_address = 2, 16 字节 }
    assert(out[4] == 0x3A && out[5] == 0x12);
    assert(out[6] == 0x12 && out[7] == 0x10);
    assert(memcmp(out + 8, routing, TESLA_UUID_LEN) == 0);
    // session_info_request = 14 { public_key = 1, 65 字节 }
    assert(out[24] == 0x72 && out[25] == 0x43);
    assert(out[26] == 0x0A && out[27] == 0x41);
    assert(memcmp(out + 28, pub, TESLA_PUBKEY_LEN) == 0);
    // uuid = 51
    assert(out[93] == 0x9A && out[94] == 0x03 && out[95] == 0x10);
    assert(memcmp(out + 96, uuid, TESLA_UUID_LEN) == 0);

    tesla_routable_t parsed;
    assert(tesla_proto_parse_routable(out, len, &parsed));
    // 请求里的 uuid 是字段 51，响应才会用 request_uuid(50) 回显。
    assert(!parsed.has_payload);
    assert(!parsed.has_request_uuid);
}

static void test_parse_session_info(void)
{
    uint8_t buf[160];
    size_t len = 0;
    buf[len++] = 0x08; // counter = 5
    buf[len++] = 0x05;
    buf[len++] = 0x12; // public_key
    buf[len++] = TESLA_PUBKEY_LEN;
    for (int i = 0; i < TESLA_PUBKEY_LEN; i++) {
        buf[len++] = (uint8_t)(0x30 + i);
    }
    buf[len++] = 0x1A; // epoch
    buf[len++] = TESLA_EPOCH_LEN;
    for (int i = 0; i < TESLA_EPOCH_LEN; i++) {
        buf[len++] = (uint8_t)(0x80 + i);
    }
    // clock_time = fixed32 0x12345678（小端）
    buf[len++] = 0x25;
    buf[len++] = 0x78;
    buf[len++] = 0x56;
    buf[len++] = 0x34;
    buf[len++] = 0x12;
    buf[len++] = 0x28; // status = OK(0)
    buf[len++] = 0x00;
    buf[len++] = 0x38; // flags = 1
    buf[len++] = 0x01;

    tesla_session_info_t info;
    assert(tesla_proto_parse_session_info(buf, len, &info));
    assert(info.counter == 5);
    assert(info.public_key_len == TESLA_PUBKEY_LEN);
    assert(info.public_key[0] == 0x30);
    assert(info.epoch_len == TESLA_EPOCH_LEN);
    assert(info.epoch[0] == 0x80);
    assert(info.clock_time == 0x12345678);
    assert(info.status == 0);
    assert(info.flags == 1);
    assert(tesla_proto_parse_session_info(NULL, 0, &info) == false);
    assert(tesla_proto_parse_session_info(buf, len, NULL) == false);
}

static void test_parse_routable_fault(void)
{
    // MessageStatus { fault = 6 }
    const uint8_t buf[] = {0x62, 0x02, 0x10, 0x06};
    tesla_routable_t parsed;
    assert(tesla_proto_parse_routable(buf, sizeof(buf), &parsed));
    assert(parsed.has_fault);
    assert(parsed.fault == 6);
}

static void test_parse_routable_session_info_tag(void)
{
    uint8_t buf[128];
    size_t len = 0;
    // session_info = 15，2 字节占位
    buf[len++] = 0x7A;
    buf[len++] = 0x02;
    buf[len++] = 0x08;
    buf[len++] = 0x00;
    // signature_data = 13 { HMAC_Signature_Data = 6 { tag = 1, 32 字节 } }
    buf[len++] = 0x6A;
    buf[len++] = 0x24;
    buf[len++] = 0x32;
    buf[len++] = 0x22;
    buf[len++] = 0x0A;
    buf[len++] = 0x20;
    for (int i = 0; i < 32; i++) {
        buf[len++] = (uint8_t)i;
    }
    // request_uuid = 50
    buf[len++] = 0x92;
    buf[len++] = 0x03;
    buf[len++] = TESLA_UUID_LEN;
    for (int i = 0; i < TESLA_UUID_LEN; i++) {
        buf[len++] = (uint8_t)(0x40 + i);
    }

    tesla_routable_t parsed;
    assert(tesla_proto_parse_routable(buf, len, &parsed));
    assert(parsed.has_session_info);
    assert(parsed.session_info_len == 2);
    assert(parsed.session_info_tag_len == 32);
    assert(parsed.session_info_tag[0] == 0x00);
    assert(parsed.session_info_tag[31] == 0x1F);
    assert(parsed.has_request_uuid);
    assert(parsed.request_uuid_len == TESLA_UUID_LEN);

    assert(tesla_proto_parse_routable(NULL, 0, &parsed) == false);
    assert(tesla_proto_parse_routable(buf, len, NULL) == false);
}

static void test_parse_vcsec_status(void)
{
    // VCSEC.VehicleStatus = 1 { lockState = 1 (locked) }
    const uint8_t vehicle[] = {0x0A, 0x02, 0x10, 0x01};
    tesla_vcsec_status_t status;
    assert(tesla_proto_parse_vcsec(vehicle, sizeof(vehicle), &status));
    assert(status.has_vehicle_status);
    assert(status.has_lock_state);
    assert(status.lock_state == 1);

    // 完整 VehicleStatus：ClosureStatuses（左前门开、尾门半开、充电口未知）、
    // lockState = 1、sleepStatus = 2、userPresence = 2。
    const uint8_t full_status[] = {
        0x0A, 0x16,                                            // VehicleStatus, 22 字节
        0x0A, 0x0E,                                            // ClosureStatuses, 14 字节
        0x08, 0x00, 0x10, 0x01, 0x18, 0x00, 0x20, 0x00,        // 四门：关、开、关、关
        0x28, 0x02, 0x30, 0x00, 0x38, 0x03,                    // 尾门半开、前备箱关、充电口未知
        0x10, 0x01,                                            // lockState = 1
        0x18, 0x02,                                            // sleepStatus = 2
        0x20, 0x02,                                            // userPresence = 2
    };
    assert(tesla_proto_parse_vcsec(full_status, sizeof(full_status), &status));
    assert(status.has_vehicle_status);
    assert(status.has_doors && status.doors_open);
    assert(status.has_rear_trunk && status.rear_trunk_open);
    assert(status.has_front_trunk && !status.front_trunk_open);
    assert(status.has_charge_port && !status.charge_port_open);
    assert(status.has_lock_state && status.lock_state == 1);
    assert(status.has_sleep_status && status.sleep_status == 2);
    assert(status.has_user_presence && status.user_presence == 2);

    // VCSEC.CommandStatus = 4 { operationStatus = 2,
    //   whitelistOperationStatus = 3 { information = 13, operationStatus = 2 } }
    const uint8_t whitelist[] = {0x22, 0x08, 0x08, 0x02, 0x1A, 0x04, 0x08, 0x0D, 0x18, 0x02};
    assert(tesla_proto_parse_vcsec(whitelist, sizeof(whitelist), &status));
    assert(status.has_operation_status);
    assert(status.operation_status == 2);
    assert(status.has_whitelist_status);
    assert(status.whitelist_information == 13);
    assert(status.whitelist_status == 2);

    // VCSEC.CommandStatus = 4 { signedMessageStatus = 2 { counter = 1, information = 1 } }
    const uint8_t fault[] = {0x22, 0x06, 0x12, 0x04, 0x08, 0x01, 0x10, 0x01};
    assert(tesla_proto_parse_vcsec(fault, sizeof(fault), &status));
    assert(status.has_command_fault);
    assert(status.command_fault == 1);
}

int main(void)
{
    test_rke_encoding();
    test_closure_encoding();
    test_status_encoding();
    test_whitelist_add_key();
    test_vcsec_envelope();
    test_session_info_request();
    test_parse_session_info();
    test_parse_routable_fault();
    test_parse_routable_session_info_tag();
    test_parse_vcsec_status();
    puts("tesla_proto: PASS");
    return 0;
}
