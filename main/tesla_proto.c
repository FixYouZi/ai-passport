// main/tesla_proto.c
// Tesla VCSEC protobuf 编解码的纯 C 实现。
#include "tesla_proto.h"

#include <string.h>

#define WIRE_VARINT 0
#define WIRE_FIXED64 1
#define WIRE_BYTES 2
#define WIRE_FIXED32 5

// ---------------------------------------------------------------------------
// 写入
// ---------------------------------------------------------------------------

void tesla_pb_init(tesla_pb_t *pb, uint8_t *data, size_t cap)
{
    pb->data = data;
    pb->cap = cap;
    pb->len = 0;
    pb->ok = (data != NULL);
}

void tesla_pb_raw(tesla_pb_t *pb, const void *data, size_t len)
{
    if (!pb->ok || pb->len + len > pb->cap) {
        pb->ok = false;
        return;
    }
    if (len != 0) {
        memcpy(pb->data + pb->len, data, len);
    }
    pb->len += len;
}

void tesla_pb_varint(tesla_pb_t *pb, uint32_t value)
{
    uint8_t buffer[5];
    size_t count = 0;
    do {
        uint8_t byte = (uint8_t)(value & 0x7fU);
        value >>= 7;
        if (value != 0) {
            byte |= 0x80U;
        }
        buffer[count++] = byte;
    } while (value != 0);
    tesla_pb_raw(pb, buffer, count);
}

void tesla_pb_tag(tesla_pb_t *pb, uint32_t field, uint8_t wire_type)
{
    tesla_pb_varint(pb, (field << 3) | wire_type);
}

void tesla_pb_uint(tesla_pb_t *pb, uint32_t field, uint32_t value)
{
    tesla_pb_tag(pb, field, WIRE_VARINT);
    tesla_pb_varint(pb, value);
}

void tesla_pb_fixed32(tesla_pb_t *pb, uint32_t field, uint32_t value)
{
    uint8_t buffer[4] = {
        (uint8_t)(value & 0xffU),
        (uint8_t)((value >> 8) & 0xffU),
        (uint8_t)((value >> 16) & 0xffU),
        (uint8_t)((value >> 24) & 0xffU),
    };
    tesla_pb_tag(pb, field, WIRE_FIXED32);
    tesla_pb_raw(pb, buffer, sizeof(buffer));
}

void tesla_pb_bytes(tesla_pb_t *pb, uint32_t field, const void *data, size_t len)
{
    tesla_pb_tag(pb, field, WIRE_BYTES);
    tesla_pb_varint(pb, (uint32_t)len);
    tesla_pb_raw(pb, data, len);
}

void tesla_meta_init(tesla_meta_t *meta, uint8_t *data, size_t cap)
{
    meta->data = data;
    meta->cap = cap;
    meta->len = 0;
    meta->ok = (data != NULL);
}

void tesla_meta_add(tesla_meta_t *meta, uint8_t tag, const void *value, size_t len)
{
    if (len > 255 || !meta->ok || meta->len + 2 + len > meta->cap) {
        meta->ok = false;
        return;
    }
    meta->data[meta->len++] = tag;
    meta->data[meta->len++] = (uint8_t)len;
    if (len != 0) {
        memcpy(meta->data + meta->len, value, len);
        meta->len += len;
    }
}

void tesla_meta_add_u32(tesla_meta_t *meta, uint8_t tag, uint32_t value)
{
    uint8_t buffer[4] = {
        (uint8_t)((value >> 24) & 0xffU),
        (uint8_t)((value >> 16) & 0xffU),
        (uint8_t)((value >> 8) & 0xffU),
        (uint8_t)(value & 0xffU),
    };
    tesla_meta_add(meta, tag, buffer, sizeof(buffer));
}

// ---------------------------------------------------------------------------
// 读取
// ---------------------------------------------------------------------------

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} cursor_t;

static bool read_varint(cursor_t *cur, uint64_t *out)
{
    uint64_t value = 0;
    unsigned shift = 0;
    while (cur->p < cur->end && shift <= 63) {
        uint8_t byte = *cur->p++;
        value |= (uint64_t)(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) {
            *out = value;
            return true;
        }
        shift += 7;
    }
    return false;
}

static bool read_tag(cursor_t *cur, uint32_t *field, uint8_t *wire_type)
{
    uint64_t key = 0;
    if (!read_varint(cur, &key) || key == 0) {
        return false;
    }
    *field = (uint32_t)(key >> 3);
    *wire_type = (uint8_t)(key & 0x7U);
    return true;
}

static bool read_bytes(cursor_t *cur, const uint8_t **data, size_t *len)
{
    uint64_t size = 0;
    if (!read_varint(cur, &size) || size > (uint64_t)(cur->end - cur->p)) {
        return false;
    }
    *data = cur->p;
    *len = (size_t)size;
    cur->p += size;
    return true;
}

static bool skip_field(cursor_t *cur, uint8_t wire_type)
{
    uint64_t value = 0;
    const uint8_t *data = NULL;
    size_t len = 0;
    switch (wire_type) {
        case WIRE_VARINT:
            return read_varint(cur, &value);
        case WIRE_FIXED64:
            if ((size_t)(cur->end - cur->p) < 8) {
                return false;
            }
            cur->p += 8;
            return true;
        case WIRE_BYTES:
            return read_bytes(cur, &data, &len);
        case WIRE_FIXED32:
            if ((size_t)(cur->end - cur->p) < 4) {
                return false;
            }
            cur->p += 4;
            return true;
        default:
            return false;
    }
}

static bool read_fixed32(cursor_t *cur, uint32_t *out)
{
    if ((size_t)(cur->end - cur->p) < 4) {
        return false;
    }
    *out = (uint32_t)cur->p[0] | ((uint32_t)cur->p[1] << 8) | ((uint32_t)cur->p[2] << 16) |
           ((uint32_t)cur->p[3] << 24);
    cur->p += 4;
    return true;
}

// ---------------------------------------------------------------------------
// VCSEC 命令
// ---------------------------------------------------------------------------

// UnsignedMessage { oneof sub_message }，返回写入长度。
static size_t unsigned_message(uint32_t field, const uint8_t *nested, size_t nested_len, uint8_t *out,
                               size_t cap)
{
    tesla_pb_t pb;
    tesla_pb_init(&pb, out, cap);
    tesla_pb_bytes(&pb, field, nested, nested_len);
    return pb.ok ? pb.len : 0;
}

size_t tesla_proto_rke(uint8_t action, uint8_t *out, size_t cap)
{
    tesla_pb_t pb;
    tesla_pb_init(&pb, out, cap);
    tesla_pb_uint(&pb, 2, action); // UnsignedMessage.RKEAction = 2
    return pb.ok ? pb.len : 0;
}

static size_t closure_move_value(uint32_t field, uint8_t value, uint8_t *out, size_t cap)
{
    uint8_t closure[8];
    tesla_pb_t pb;
    tesla_pb_init(&pb, closure, sizeof(closure));
    tesla_pb_uint(&pb, field, value);
    if (!pb.ok) {
        return 0;
    }
    return unsigned_message(4, closure, pb.len, out, cap); // UnsignedMessage.closureMoveRequest = 4
}

static size_t closure_move(uint32_t field, uint8_t *out, size_t cap)
{
    return closure_move_value(field, TESLA_CLOSURE_MOVE, out, cap);
}

size_t tesla_proto_open_frunk(uint8_t *out, size_t cap)
{
    return closure_move(6, out, cap); // ClosureMoveRequest.frontTrunk = 6
}

size_t tesla_proto_open_trunk(uint8_t *out, size_t cap)
{
    return closure_move(5, out, cap); // ClosureMoveRequest.rearTrunk = 5
}

size_t tesla_proto_close_trunk(uint8_t *out, size_t cap)
{
    return closure_move_value(5, TESLA_CLOSURE_CLOSE, out, cap);
}

size_t tesla_proto_info_status(uint8_t *out, size_t cap)
{
    uint8_t request[8];
    tesla_pb_t pb;
    tesla_pb_init(&pb, request, sizeof(request));
    tesla_pb_uint(&pb, 1, 0); // InformationRequest.informationRequestType = GET_STATUS
    if (!pb.ok) {
        return 0;
    }
    return unsigned_message(1, request, pb.len, out, cap); // UnsignedMessage.InformationRequest = 1
}

size_t tesla_proto_whitelist_add_key(const uint8_t pub[TESLA_PUBKEY_LEN], uint8_t form_factor,
                                     uint8_t *out, size_t cap)
{
    uint8_t public_key[80];
    uint8_t permission[96];
    uint8_t metadata[8];
    uint8_t whitelist[160];
    tesla_pb_t pb;
    size_t public_key_len;
    size_t permission_len;
    size_t metadata_len;

    tesla_pb_init(&pb, public_key, sizeof(public_key));
    tesla_pb_bytes(&pb, 1, pub, TESLA_PUBKEY_LEN); // PublicKey.PublicKeyRaw = 1
    if (!pb.ok) {
        return 0;
    }
    public_key_len = pb.len;

    tesla_pb_init(&pb, permission, sizeof(permission));
    tesla_pb_bytes(&pb, 1, public_key, public_key_len); // PermissionChange.key = 1
    tesla_pb_uint(&pb, 3, 0);                           // secondsToBeActive = 3
    tesla_pb_uint(&pb, 4, TESLA_ROLE_OWNER);            // keyRole = 4
    if (!pb.ok) {
        return 0;
    }
    permission_len = pb.len;

    tesla_pb_init(&pb, metadata, sizeof(metadata));
    tesla_pb_uint(&pb, 1, form_factor); // KeyMetadata.keyFormFactor = 1
    if (!pb.ok) {
        return 0;
    }
    metadata_len = pb.len;

    tesla_pb_init(&pb, whitelist, sizeof(whitelist));
    tesla_pb_bytes(&pb, 5, permission, permission_len); // addKeyToWhitelistAndAddPermissions = 5
    tesla_pb_bytes(&pb, 6, metadata, metadata_len);     // metadataForKey = 6
    if (!pb.ok) {
        return 0;
    }

    return unsigned_message(16, whitelist, pb.len, out, cap); // UnsignedMessage.WhitelistOperation = 16
}

size_t tesla_proto_vcsec_envelope(const uint8_t *unsigned_msg, size_t len, uint8_t sig_type,
                                  uint8_t *out, size_t cap)
{
    uint8_t signed_message[600];
    tesla_pb_t pb;
    size_t signed_len;

    tesla_pb_init(&pb, signed_message, sizeof(signed_message));
    tesla_pb_bytes(&pb, 2, unsigned_msg, len); // SignedMessage.protobufMessageAsBytes = 2
    tesla_pb_uint(&pb, 3, sig_type);            // SignedMessage.signatureType = 3
    if (!pb.ok) {
        return 0;
    }
    signed_len = pb.len;

    tesla_pb_init(&pb, out, cap);
    tesla_pb_bytes(&pb, 1, signed_message, signed_len); // ToVCSECMessage.signedMessage = 1
    return pb.ok ? pb.len : 0;
}

// ---------------------------------------------------------------------------
// RoutableMessage
// ---------------------------------------------------------------------------

static size_t destination_domain(uint32_t domain, uint8_t *out, size_t cap)
{
    tesla_pb_t pb;
    tesla_pb_init(&pb, out, cap);
    tesla_pb_uint(&pb, 1, domain);
    return pb.ok ? pb.len : 0;
}

static size_t destination_routing(const uint8_t routing[TESLA_UUID_LEN], uint8_t *out, size_t cap)
{
    tesla_pb_t pb;
    tesla_pb_init(&pb, out, cap);
    tesla_pb_bytes(&pb, 2, routing, TESLA_UUID_LEN);
    return pb.ok ? pb.len : 0;
}

size_t tesla_proto_session_info_request(const uint8_t pub[TESLA_PUBKEY_LEN],
                                        const uint8_t uuid[TESLA_UUID_LEN],
                                        const uint8_t routing[TESLA_UUID_LEN], uint8_t *out,
                                        size_t cap)
{
    uint8_t request[80];
    uint8_t to_dest[8];
    uint8_t from_dest[24];
    tesla_pb_t pb;
    size_t request_len;

    tesla_pb_init(&pb, request, sizeof(request));
    tesla_pb_bytes(&pb, 1, pub, TESLA_PUBKEY_LEN); // SessionInfoRequest.public_key = 1
    if (!pb.ok) {
        return 0;
    }
    request_len = pb.len;

    size_t to_len = destination_domain(TESLA_DOMAIN_VCSEC, to_dest, sizeof(to_dest));
    size_t from_len = destination_routing(routing, from_dest, sizeof(from_dest));
    if (to_len == 0 || from_len == 0) {
        return 0;
    }

    tesla_pb_init(&pb, out, cap);
    tesla_pb_bytes(&pb, 6, to_dest, to_len);       // to_destination = 6
    tesla_pb_bytes(&pb, 7, from_dest, from_len);   // from_destination = 7
    tesla_pb_bytes(&pb, 14, request, request_len); // session_info_request = 14
    tesla_pb_bytes(&pb, 51, uuid, TESLA_UUID_LEN); // uuid = 51
    return pb.ok ? pb.len : 0;
}

size_t tesla_proto_encrypted_envelope(const uint8_t pub[TESLA_PUBKEY_LEN], const tesla_gcm_sig_t *sig,
                                      const uint8_t *ciphertext, size_t ciphertext_len,
                                      const uint8_t routing[TESLA_UUID_LEN],
                                      const uint8_t uuid[TESLA_UUID_LEN], uint32_t flags, uint8_t *out,
                                      size_t cap)
{
    uint8_t gcm[96];
    uint8_t identity[80];
    uint8_t signature[200];
    uint8_t to_dest[8];
    uint8_t from_dest[24];
    tesla_pb_t pb;
    size_t gcm_len;
    size_t identity_len;
    size_t signature_len;

    tesla_pb_init(&pb, gcm, sizeof(gcm));
    tesla_pb_bytes(&pb, 1, sig->epoch, sig->epoch_len); // epoch = 1
    tesla_pb_bytes(&pb, 2, sig->nonce, sig->nonce_len); // nonce = 2
    tesla_pb_uint(&pb, 3, sig->counter);                // counter = 3
    tesla_pb_fixed32(&pb, 4, sig->expires_at);          // expires_at = 4
    tesla_pb_bytes(&pb, 5, sig->tag, sig->tag_len);     // tag = 5
    if (!pb.ok) {
        return 0;
    }
    gcm_len = pb.len;

    tesla_pb_init(&pb, identity, sizeof(identity));
    tesla_pb_bytes(&pb, 1, pub, TESLA_PUBKEY_LEN); // KeyIdentity.public_key = 1
    if (!pb.ok) {
        return 0;
    }
    identity_len = pb.len;

    tesla_pb_init(&pb, signature, sizeof(signature));
    tesla_pb_bytes(&pb, 1, identity, identity_len); // SignatureData.signer_identity = 1
    tesla_pb_bytes(&pb, 5, gcm, gcm_len);            // AES_GCM_Personalized_data = 5
    if (!pb.ok) {
        return 0;
    }
    signature_len = pb.len;

    size_t to_len = destination_domain(TESLA_DOMAIN_VCSEC, to_dest, sizeof(to_dest));
    size_t from_len = destination_routing(routing, from_dest, sizeof(from_dest));
    if (to_len == 0 || from_len == 0) {
        return 0;
    }

    tesla_pb_init(&pb, out, cap);
    tesla_pb_bytes(&pb, 6, to_dest, to_len);               // to_destination = 6
    tesla_pb_bytes(&pb, 7, from_dest, from_len);           // from_destination = 7
    tesla_pb_bytes(&pb, 10, ciphertext, ciphertext_len);   // payload = 10
    tesla_pb_bytes(&pb, 13, signature, signature_len);     // signature_data = 13
    tesla_pb_bytes(&pb, 51, uuid, TESLA_UUID_LEN);         // uuid = 51
    tesla_pb_uint(&pb, 52, flags);                         // flags = 52
    return pb.ok ? pb.len : 0;
}

// ---------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------

static void parse_destination(cursor_t *cur, bool *has_domain, uint32_t *domain, bool *has_routing,
                              uint8_t routing[TESLA_UUID_LEN])
{
    while (cur->p < cur->end) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!read_tag(cur, &field, &wire)) {
            return;
        }
        if (field == 1 && wire == WIRE_VARINT) {
            uint64_t value = 0;
            if (!read_varint(cur, &value)) {
                return;
            }
            *domain = (uint32_t)value;
            *has_domain = true;
        } else if (field == 2 && wire == WIRE_BYTES) {
            const uint8_t *data = NULL;
            size_t len = 0;
            if (!read_bytes(cur, &data, &len)) {
                return;
            }
            if (len == TESLA_UUID_LEN && routing != NULL) {
                memcpy(routing, data, TESLA_UUID_LEN);
                *has_routing = true;
            }
        } else if (!skip_field(cur, wire)) {
            return;
        }
    }
}

static void parse_message_status(cursor_t *cur, tesla_routable_t *out)
{
    while (cur->p < cur->end) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!read_tag(cur, &field, &wire)) {
            return;
        }
        if (wire == WIRE_VARINT) {
            uint64_t value = 0;
            if (!read_varint(cur, &value)) {
                return;
            }
            if (field == 2) {
                out->fault = (uint32_t)value;
                out->has_fault = true;
            }
        } else if (!skip_field(cur, wire)) {
            return;
        }
    }
}

static void parse_gcm_response(cursor_t *cur, tesla_routable_t *out)
{
    out->has_response = true;
    while (cur->p < cur->end) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!read_tag(cur, &field, &wire)) {
            return;
        }
        if (field == 1 && wire == WIRE_BYTES) {
            if (!read_bytes(cur, &out->resp_nonce, &out->resp_nonce_len)) {
                return;
            }
        } else if (field == 2 && wire == WIRE_VARINT) {
            uint64_t value = 0;
            if (!read_varint(cur, &value)) {
                return;
            }
            out->resp_counter = (uint32_t)value;
        } else if (field == 3 && wire == WIRE_BYTES) {
            if (!read_bytes(cur, &out->resp_tag, &out->resp_tag_len)) {
                return;
            }
        } else if (!skip_field(cur, wire)) {
            return;
        }
    }
}

static void parse_signature(cursor_t *cur, tesla_routable_t *out)
{
    while (cur->p < cur->end) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!read_tag(cur, &field, &wire)) {
            return;
        }
        if (wire != WIRE_BYTES) {
            if (!skip_field(cur, wire)) {
                return;
            }
            continue;
        }
        const uint8_t *data = NULL;
        size_t len = 0;
        if (!read_bytes(cur, &data, &len)) {
            return;
        }
        cursor_t nested = {data, data + len};
        if (field == 6) {
            // HMAC_Signature_Data { bytes tag = 1; }
            while (nested.p < nested.end) {
                uint32_t nf = 0;
                uint8_t nw = 0;
                if (!read_tag(&nested, &nf, &nw)) {
                    return;
                }
                if (nf == 1 && nw == WIRE_BYTES) {
                    if (!read_bytes(&nested, &out->session_info_tag, &out->session_info_tag_len)) {
                        return;
                    }
                } else if (!skip_field(&nested, nw)) {
                    return;
                }
            }
        } else if (field == 9) {
            parse_gcm_response(&nested, out);
        }
    }
}

bool tesla_proto_parse_routable(const uint8_t *buf, size_t len, tesla_routable_t *out)
{
    if (buf == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    cursor_t cur = {buf, buf + len};
    while (cur.p < cur.end) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!read_tag(&cur, &field, &wire)) {
            return false;
        }
        if (wire == WIRE_BYTES) {
            const uint8_t *data = NULL;
            size_t size = 0;
            if (!read_bytes(&cur, &data, &size)) {
                return false;
            }
            cursor_t nested = {data, data + size};
            switch (field) {
                case 6: {
                    uint32_t ignored = 0;
                    bool has_ignored = false;
                    parse_destination(&nested, &has_ignored, &ignored, &out->has_to_routing,
                                      out->to_routing);
                    break;
                }
                case 7:
                    parse_destination(&nested, &out->has_from_domain, &out->from_domain, NULL, NULL);
                    break;
                case 10:
                    out->payload = data;
                    out->payload_len = size;
                    out->has_payload = true;
                    break;
                case 12:
                    parse_message_status(&nested, out);
                    break;
                case 13:
                    parse_signature(&nested, out);
                    break;
                case 15:
                    out->session_info = data;
                    out->session_info_len = size;
                    out->has_session_info = true;
                    break;
                case 50:
                    out->request_uuid = data;
                    out->request_uuid_len = size;
                    out->has_request_uuid = true;
                    break;
                default:
                    break;
            }
        } else if (wire == WIRE_VARINT) {
            uint64_t value = 0;
            if (!read_varint(&cur, &value)) {
                return false;
            }
            if (field == 52) {
                out->flags = (uint32_t)value;
                out->has_flags = true;
            }
        } else if (!skip_field(&cur, wire)) {
            return false;
        }
    }
    return true;
}

bool tesla_proto_parse_session_info(const uint8_t *buf, size_t len, tesla_session_info_t *out)
{
    if (buf == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    cursor_t cur = {buf, buf + len};
    while (cur.p < cur.end) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!read_tag(&cur, &field, &wire)) {
            return false;
        }
        if (wire == WIRE_VARINT) {
            uint64_t value = 0;
            if (!read_varint(&cur, &value)) {
                return false;
            }
            switch (field) {
                case 1:
                    out->counter = (uint32_t)value;
                    break;
                case 5:
                    out->status = (uint32_t)value;
                    break;
                case 7:
                    out->flags = (uint32_t)value;
                    break;
                default:
                    break;
            }
        } else if (wire == WIRE_BYTES) {
            const uint8_t *data = NULL;
            size_t size = 0;
            if (!read_bytes(&cur, &data, &size)) {
                return false;
            }
            if (field == 2) {
                if (size > TESLA_PUBKEY_LEN) {
                    return false;
                }
                memcpy(out->public_key, data, size);
                out->public_key_len = size;
            } else if (field == 3) {
                if (size > TESLA_EPOCH_LEN) {
                    return false;
                }
                memcpy(out->epoch, data, size);
                out->epoch_len = size;
            }
        } else if (wire == WIRE_FIXED32) {
            uint32_t value = 0;
            if (!read_fixed32(&cur, &value)) {
                return false;
            }
            if (field == 4) {
                out->clock_time = value;
            }
        } else if (!skip_field(&cur, wire)) {
            return false;
        }
    }
    return true;
}

// ClosureState: 0 Closed, 1 Open, 2 Ajar, 3 Unknown, 4 FailedUnlatch,
// 5 Opening, 6 Closing。只有明确处于打开/半开/运动中才算“未关闭”。
static bool closure_is_open(uint64_t state)
{
    return state == 1 || state == 2 || state == 5 || state == 6;
}

static void parse_closure_statuses(cursor_t *cur, tesla_vcsec_status_t *out)
{
    while (cur->p < cur->end) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!read_tag(cur, &field, &wire)) {
            return;
        }
        if (wire != WIRE_VARINT) {
            if (!skip_field(cur, wire)) {
                return;
            }
            continue;
        }
        uint64_t value = 0;
        if (!read_varint(cur, &value)) {
            return;
        }
        switch (field) {
        case 1:
        case 2:
        case 3:
        case 4:
            out->has_doors = true;
            if (closure_is_open(value)) {
                out->doors_open = true;
            }
            break;
        case 5:
            out->has_rear_trunk = true;
            out->rear_trunk_open = closure_is_open(value);
            break;
        case 6:
            out->has_front_trunk = true;
            out->front_trunk_open = closure_is_open(value);
            break;
        case 7:
            out->has_charge_port = true;
            out->charge_port_open = closure_is_open(value);
            break;
        default:
            break;
        }
    }
}

static void parse_vehicle_status(cursor_t *cur, tesla_vcsec_status_t *out)
{
    out->has_vehicle_status = true;
    while (cur->p < cur->end) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!read_tag(cur, &field, &wire)) {
            return;
        }
        if (field == 1 && wire == WIRE_BYTES) {
            const uint8_t *data = NULL;
            size_t size = 0;
            if (!read_bytes(cur, &data, &size)) {
                return;
            }
            cursor_t nested = {data, data + size};
            parse_closure_statuses(&nested, out);
        } else if (wire == WIRE_VARINT) {
            uint64_t value = 0;
            if (!read_varint(cur, &value)) {
                return;
            }
            if (field == 2) {
                out->lock_state = (uint32_t)value;
                out->has_lock_state = true;
            } else if (field == 3) {
                out->sleep_status = (uint32_t)value;
                out->has_sleep_status = true;
            } else if (field == 4) {
                out->user_presence = (uint32_t)value;
                out->has_user_presence = true;
            }
        } else if (!skip_field(cur, wire)) {
            return;
        }
    }
}

static void parse_command_status(cursor_t *cur, tesla_vcsec_status_t *out)
{
    while (cur->p < cur->end) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!read_tag(cur, &field, &wire)) {
            return;
        }
        if (field == 1 && wire == WIRE_VARINT) {
            uint64_t value = 0;
            if (!read_varint(cur, &value)) {
                return;
            }
            out->operation_status = (uint32_t)value;
            out->has_operation_status = true;
        } else if (wire == WIRE_BYTES) {
            const uint8_t *data = NULL;
            size_t size = 0;
            if (!read_bytes(cur, &data, &size)) {
                return;
            }
            cursor_t nested = {data, data + size};
            if (field == 2) {
                // SignedMessage_status { counter = 1; information = 2; }
                while (nested.p < nested.end) {
                    uint32_t nf = 0;
                    uint8_t nw = 0;
                    if (!read_tag(&nested, &nf, &nw)) {
                        return;
                    }
                    if (nf == 2 && nw == WIRE_VARINT) {
                        uint64_t value = 0;
                        if (!read_varint(&nested, &value)) {
                            return;
                        }
                        out->command_fault = (uint32_t)value;
                        out->has_command_fault = true;
                    } else if (!skip_field(&nested, nw)) {
                        return;
                    }
                }
            } else if (field == 3) {
                // WhitelistOperation_status { information = 1; operationStatus = 3; }
                out->has_whitelist_status = true;
                while (nested.p < nested.end) {
                    uint32_t nf = 0;
                    uint8_t nw = 0;
                    if (!read_tag(&nested, &nf, &nw)) {
                        return;
                    }
                    if (nw == WIRE_VARINT) {
                        uint64_t value = 0;
                        if (!read_varint(&nested, &value)) {
                            return;
                        }
                        if (nf == 1) {
                            out->whitelist_information = (uint32_t)value;
                        } else if (nf == 3) {
                            out->whitelist_status = (uint32_t)value;
                        }
                    } else if (!skip_field(&nested, nw)) {
                        return;
                    }
                }
            }
        } else if (!skip_field(cur, wire)) {
            return;
        }
    }
}

bool tesla_proto_parse_vcsec(const uint8_t *buf, size_t len, tesla_vcsec_status_t *out)
{
    if (buf == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    cursor_t cur = {buf, buf + len};
    while (cur.p < cur.end) {
        uint32_t field = 0;
        uint8_t wire = 0;
        if (!read_tag(&cur, &field, &wire)) {
            return false;
        }
        if (wire != WIRE_BYTES) {
            if (!skip_field(&cur, wire)) {
                return false;
            }
            continue;
        }
        const uint8_t *data = NULL;
        size_t size = 0;
        if (!read_bytes(&cur, &data, &size)) {
            return false;
        }
        cursor_t nested = {data, data + size};
        if (field == 1) {
            parse_vehicle_status(&nested, out);
        } else if (field == 4) {
            parse_command_status(&nested, out);
        }
    }
    return true;
}
