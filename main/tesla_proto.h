// main/tesla_proto.h
// Tesla VCSEC 协议的纯 C protobuf 编解码。不依赖 ESP-IDF，可在主机上单测。
//
// 字段编号来自 teslamotors/vehicle-command 的 universal_message.proto /
// signatures.proto / vcsec.proto / keys.proto。这里只实现钥匙所需的最小集合。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TESLA_UUID_LEN 16
#define TESLA_PUBKEY_LEN 65
#define TESLA_EPOCH_LEN 16
#define TESLA_KEY_TAG_LEN 16
#define TESLA_NONCE_LEN 12
#define TESLA_SHA256_LEN 32
#define TESLA_MAX_MESSAGE 1024

// UniversalMessage.Domain
#define TESLA_DOMAIN_BROADCAST 0
#define TESLA_DOMAIN_VCSEC 2
#define TESLA_DOMAIN_INFOTAINMENT 3

// UniversalMessage.Flags
#define TESLA_FLAG_ENCRYPT_RESPONSE 2

// Signatures.Tag
enum {
    TESLA_TAG_SIGNATURE_TYPE = 0,
    TESLA_TAG_DOMAIN = 1,
    TESLA_TAG_PERSONALIZATION = 2,
    TESLA_TAG_EPOCH = 3,
    TESLA_TAG_EXPIRES_AT = 4,
    TESLA_TAG_COUNTER = 5,
    TESLA_TAG_CHALLENGE = 6,
    TESLA_TAG_FLAGS = 7,
    TESLA_TAG_REQUEST_HASH = 8,
    TESLA_TAG_FAULT = 9,
    TESLA_TAG_COMMAND_PREFIX = 10,
    TESLA_TAG_END = 255,
};

// Signatures.SignatureType
enum {
    TESLA_SIG_TYPE_AES_GCM = 0,
    TESLA_SIG_TYPE_AES_GCM_PERSONALIZED = 5,
    TESLA_SIG_TYPE_HMAC = 6,
    TESLA_SIG_TYPE_HMAC_PERSONALIZED = 8,
    TESLA_SIG_TYPE_AES_GCM_RESPONSE = 9,
    TESLA_SIG_TYPE_AES_GCM_DETACHED = 10,
};

// VCSEC.SignatureType（车辆端 SignedMessage 内的签名类型）
enum {
    TESLA_VCSEC_SIG_NONE = 0,
    TESLA_VCSEC_SIG_PRESENT_KEY = 2,
};

// VCSEC.RKEAction_E
enum {
    TESLA_RKE_UNLOCK = 0,
    TESLA_RKE_LOCK = 1,
    TESLA_RKE_WAKE = 30,
};

// VCSEC.ClosureMoveType_E
enum {
    TESLA_CLOSURE_NONE = 0,
    TESLA_CLOSURE_MOVE = 1,
    TESLA_CLOSURE_OPEN = 3,
    TESLA_CLOSURE_CLOSE = 4,
};

// Keys.Role
enum {
    TESLA_ROLE_SERVICE = 1,
    TESLA_ROLE_OWNER = 2,
    TESLA_ROLE_DRIVER = 3,
};

// VCSEC.KeyFormFactor
enum {
    TESLA_KEY_FORM_UNKNOWN = 0,
    TESLA_KEY_FORM_NFC_CARD = 1,
    TESLA_KEY_FORM_IOS = 6,
    TESLA_KEY_FORM_ANDROID = 7,
};

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t len;
    bool ok;
} tesla_pb_t;

void tesla_pb_init(tesla_pb_t *pb, uint8_t *data, size_t cap);
void tesla_pb_raw(tesla_pb_t *pb, const void *data, size_t len);
void tesla_pb_varint(tesla_pb_t *pb, uint32_t value);
void tesla_pb_tag(tesla_pb_t *pb, uint32_t field, uint8_t wire_type);
void tesla_pb_uint(tesla_pb_t *pb, uint32_t field, uint32_t value);
void tesla_pb_fixed32(tesla_pb_t *pb, uint32_t field, uint32_t value);
void tesla_pb_bytes(tesla_pb_t *pb, uint32_t field, const void *data, size_t len);

// 元数据拼接（Signatures.Tag 顺序敏感）：只负责字节序列，便于独立测试。
typedef struct {
    uint8_t *data;
    size_t cap;
    size_t len;
    bool ok;
} tesla_meta_t;

void tesla_meta_init(tesla_meta_t *meta, uint8_t *data, size_t cap);
void tesla_meta_add(tesla_meta_t *meta, uint8_t tag, const void *value, size_t len);
void tesla_meta_add_u32(tesla_meta_t *meta, uint8_t tag, uint32_t value);

// ---- 车辆安全域（VCSEC）命令负载 ----
size_t tesla_proto_rke(uint8_t action, uint8_t *out, size_t cap);
size_t tesla_proto_open_frunk(uint8_t *out, size_t cap);
size_t tesla_proto_open_trunk(uint8_t *out, size_t cap);
size_t tesla_proto_close_trunk(uint8_t *out, size_t cap);
size_t tesla_proto_info_status(uint8_t *out, size_t cap);
size_t tesla_proto_whitelist_add_key(const uint8_t pub[TESLA_PUBKEY_LEN], uint8_t form_factor,
                                     uint8_t *out, size_t cap);
// 车辆端 SignedMessage 外层（ToVCSECMessage），用于未建立会话的添加钥匙请求。
size_t tesla_proto_vcsec_envelope(const uint8_t *unsigned_msg, size_t len, uint8_t sig_type,
                                  uint8_t *out, size_t cap);

// ---- RoutableMessage 信封 ----
size_t tesla_proto_session_info_request(uint8_t domain, const uint8_t pub[TESLA_PUBKEY_LEN],
                                        const uint8_t uuid[TESLA_UUID_LEN],
                                        const uint8_t routing[TESLA_UUID_LEN], uint8_t *out,
                                        size_t cap);

typedef struct {
    const uint8_t *epoch;
    size_t epoch_len;
    const uint8_t *nonce;
    size_t nonce_len;
    uint32_t counter;
    uint32_t expires_at;
    const uint8_t *tag;
    size_t tag_len;
} tesla_gcm_sig_t;

size_t tesla_proto_encrypted_envelope(uint8_t domain, const uint8_t pub[TESLA_PUBKEY_LEN],
                                      const tesla_gcm_sig_t *sig, const uint8_t *ciphertext,
                                      size_t ciphertext_len,
                                      const uint8_t routing[TESLA_UUID_LEN],
                                      const uint8_t uuid[TESLA_UUID_LEN], uint32_t flags, uint8_t *out,
                                      size_t cap);

// ---- 信息娱乐域（车机电量）----
// CarServer.Action { vehicleAction { getVehicleData { getChargeState } } }
size_t tesla_proto_charge_request(uint8_t *out, size_t cap);
// CarServer.Action { vehicleAction { vehicleControlFlashLightsAction } }
size_t tesla_proto_flash_lights(uint8_t *out, size_t cap);
// CarServer.Action { vehicleAction { vehicleControlHonkHornAction } }
size_t tesla_proto_honk_horn(uint8_t *out, size_t cap);
// 解析 CarServer.Response.vehicleData.chargeState，输出剩余电量百分比（0..100）。
bool tesla_proto_parse_charge_state(const uint8_t *data, size_t len, int *percent);

// ---- 解析 ----
typedef struct {
    bool has_from_domain;
    uint32_t from_domain;
    bool has_to_routing;
    uint8_t to_routing[TESLA_UUID_LEN];

    bool has_session_info;
    const uint8_t *session_info;
    size_t session_info_len;

    // AES_GCM_RESPONSE
    bool has_response;
    const uint8_t *resp_nonce;
    size_t resp_nonce_len;
    uint32_t resp_counter;
    const uint8_t *resp_tag;
    size_t resp_tag_len;

    // HMAC session-info tag
    const uint8_t *session_info_tag;
    size_t session_info_tag_len;

    bool has_payload;
    const uint8_t *payload;
    size_t payload_len;

    bool has_flags;
    uint32_t flags;
    bool has_fault;
    uint32_t fault;

    bool has_request_uuid;
    const uint8_t *request_uuid;
    size_t request_uuid_len;
} tesla_routable_t;

bool tesla_proto_parse_routable(const uint8_t *buf, size_t len, tesla_routable_t *out);

typedef struct {
    uint32_t counter;
    uint8_t public_key[TESLA_PUBKEY_LEN];
    size_t public_key_len;
    uint8_t epoch[TESLA_EPOCH_LEN];
    size_t epoch_len;
    uint32_t clock_time;
    uint32_t status;
    uint32_t flags;
} tesla_session_info_t;

bool tesla_proto_parse_session_info(const uint8_t *buf, size_t len, tesla_session_info_t *out);

typedef struct {
    bool has_lock_state;
    uint32_t lock_state;
    bool has_vehicle_status;
    bool has_doors;
    bool doors_open;
    bool has_rear_trunk;
    bool rear_trunk_open;
    bool has_front_trunk;
    bool front_trunk_open;
    bool has_charge_port;
    bool charge_port_open;
    bool has_sleep_status;
    uint32_t sleep_status;
    bool has_user_presence;
    uint32_t user_presence;
    bool has_operation_status;
    uint32_t operation_status;
    bool has_command_fault;
    uint32_t command_fault;
    bool has_whitelist_status;
    uint32_t whitelist_status;
    uint32_t whitelist_information;
} tesla_vcsec_status_t;

bool tesla_proto_parse_vcsec(const uint8_t *buf, size_t len, tesla_vcsec_status_t *out);
