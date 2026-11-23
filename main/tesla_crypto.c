// main/tesla_crypto.c
#include "tesla_crypto.h"

#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#define KEY_NAMESPACE "tesla_key"
#define KEY_STORAGE "sk"
#define SESSION_TTL_SECONDS 30

static uint8_t s_private_key[32];
static bool s_private_loaded;

void tesla_crypto_random(void *out, size_t len)
{
    esp_fill_random(out, len);
}

// mbedTLS 3.x 在 mbedtls_ecp_mul/ecdh_compute_shared 中要求非空 RNG 回调，
// 否则直接返回 MBEDTLS_ERR_ECP_BAD_INPUT_DATA。
static int ecp_rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

static esp_err_t load_private_key(void)
{
    if (s_private_loaded) {
        return ESP_OK;
    }
    nvs_handle_t handle;
    bool dirty = false;
    size_t size = sizeof(s_private_key);
    esp_err_t err = nvs_open(KEY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(handle, KEY_STORAGE, s_private_key, &size);
    if (err != ESP_OK || size != sizeof(s_private_key)) {
        dirty = true;
    } else {
        mbedtls_ecp_group group;
        mbedtls_mpi scalar;
        mbedtls_ecp_group_init(&group);
        mbedtls_mpi_init(&scalar);
        mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
        if (mbedtls_mpi_read_binary(&scalar, s_private_key, sizeof(s_private_key)) != 0 ||
            mbedtls_ecp_check_privkey(&group, &scalar) != 0) {
            dirty = true;
        }
        mbedtls_mpi_free(&scalar);
        mbedtls_ecp_group_free(&group);
    }

    if (dirty) {
        mbedtls_ecp_group group;
        mbedtls_mpi scalar;
        mbedtls_ecp_group_init(&group);
        mbedtls_mpi_init(&scalar);
        mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
        do {
            tesla_crypto_random(s_private_key, sizeof(s_private_key));
            mbedtls_mpi_free(&scalar);
            mbedtls_mpi_init(&scalar);
            if (mbedtls_mpi_read_binary(&scalar, s_private_key, sizeof(s_private_key)) != 0) {
                continue;
            }
        } while (mbedtls_ecp_check_privkey(&group, &scalar) != 0);
        mbedtls_mpi_free(&scalar);
        mbedtls_ecp_group_free(&group);
        err = nvs_set_blob(handle, KEY_STORAGE, s_private_key, sizeof(s_private_key));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        if (err != ESP_OK) {
            nvs_close(handle);
            return err;
        }
    }
    nvs_close(handle);
    s_private_loaded = true;
    return ESP_OK;
}

static esp_err_t compute_public_key(const uint8_t private_key[32],
                                   uint8_t public_key[TESLA_PUBKEY_LEN])
{
    mbedtls_ecp_group group;
    mbedtls_mpi scalar;
    mbedtls_ecp_point point;
    esp_err_t result = ESP_FAIL;
    size_t written = 0;

    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&scalar);
    mbedtls_ecp_point_init(&point);
    if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
        mbedtls_mpi_read_binary(&scalar, private_key, 32) != 0 ||
        mbedtls_ecp_check_privkey(&group, &scalar) != 0 ||
        mbedtls_ecp_mul(&group, &point, &scalar, &group.G, ecp_rng, NULL) != 0) {
        goto cleanup;
    }
    if (mbedtls_ecp_point_write_binary(&group, &point, MBEDTLS_ECP_PF_UNCOMPRESSED, &written,
                                       public_key, TESLA_PUBKEY_LEN) != 0 ||
        written != TESLA_PUBKEY_LEN) {
        goto cleanup;
    }
    result = ESP_OK;

cleanup:
    mbedtls_ecp_point_free(&point);
    mbedtls_mpi_free(&scalar);
    mbedtls_ecp_group_free(&group);
    return result;
}

esp_err_t tesla_crypto_key_ensure(uint8_t public_key[TESLA_PUBKEY_LEN])
{
    esp_err_t err = load_private_key();
    if (err != ESP_OK) {
        return err;
    }
    return compute_public_key(s_private_key, public_key);
}

esp_err_t tesla_crypto_key_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(KEY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, KEY_STORAGE);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(handle);
        err = ESP_OK;
    }
    nvs_close(handle);
    s_private_loaded = false;
    return err;
}

void tesla_crypto_fingerprint(const uint8_t public_key[TESLA_PUBKEY_LEN], char out[9])
{
    uint8_t digest[20];
    mbedtls_sha1(public_key, TESLA_PUBKEY_LEN, digest);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[8] = '\0';
}

static esp_err_t derive_session_key(const uint8_t private_key[32],
                                    const uint8_t peer_public[TESLA_PUBKEY_LEN],
                                    uint8_t session_key[16])
{
    mbedtls_ecp_group group;
    mbedtls_mpi scalar;
    mbedtls_mpi shared;
    mbedtls_ecp_point peer;
    uint8_t secret[32];
    uint8_t digest[20];
    esp_err_t result = ESP_FAIL;

    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&scalar);
    mbedtls_mpi_init(&shared);
    mbedtls_ecp_point_init(&peer);

    if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
        mbedtls_mpi_read_binary(&scalar, private_key, 32) != 0 ||
        mbedtls_ecp_point_read_binary(&group, &peer, peer_public, TESLA_PUBKEY_LEN) != 0 ||
        mbedtls_ecp_check_pubkey(&group, &peer) != 0 ||
        mbedtls_ecdh_compute_shared(&group, &shared, &peer, &scalar, ecp_rng, NULL) != 0) {
        goto cleanup;
    }
    if (mbedtls_mpi_write_binary(&shared, secret, sizeof(secret)) != 0) {
        goto cleanup;
    }
    // 与 vehicle-command 一致：SHA1(共享 X 坐标) 的前 16 字节作为 AES-128 密钥。
    mbedtls_sha1(secret, sizeof(secret), digest);
    memcpy(session_key, digest, 16);
    result = ESP_OK;

cleanup:
    memset(secret, 0, sizeof(secret));
    mbedtls_ecp_point_free(&peer);
    mbedtls_mpi_free(&shared);
    mbedtls_mpi_free(&scalar);
    mbedtls_ecp_group_free(&group);
    return result;
}

static esp_err_t hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                             uint8_t out[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (mbedtls_md_hmac(info, key, key_len, data, len, out) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// 子密钥：HMAC-SHA256(session_key, label)
static esp_err_t subkey(const uint8_t session_key[16], const char *label, uint8_t out[32])
{
    return hmac_sha256(session_key, 16, (const uint8_t *)label, strlen(label), out);
}

static esp_err_t sha256_meta(const uint8_t *meta, size_t meta_len, uint8_t out[32])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }
    uint8_t end = TESLA_TAG_END;
    if (mbedtls_sha256_update(&ctx, meta, meta_len) != 0 ||
        mbedtls_sha256_update(&ctx, &end, 1) != 0 ||
        mbedtls_sha256_finish(&ctx, out) != 0) {
        mbedtls_sha256_free(&ctx);
        return ESP_FAIL;
    }
    mbedtls_sha256_free(&ctx);
    return ESP_OK;
}

esp_err_t tesla_crypto_session_apply(tesla_session_t *session, const uint8_t *vin,
                                     const uint8_t *session_info_bytes, size_t len)
{
    tesla_session_info_t info;
    if (session == NULL || vin == NULL ||
        !tesla_proto_parse_session_info(session_info_bytes, len, &info)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (info.public_key_len != TESLA_PUBKEY_LEN || info.epoch_len != TESLA_EPOCH_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = load_private_key();
    if (err != ESP_OK) {
        return err;
    }
    err = derive_session_key(s_private_key, info.public_key, session->session_key);
    if (err != ESP_OK) {
        return err;
    }
    err = compute_public_key(s_private_key, session->public_key);
    if (err != ESP_OK) {
        return err;
    }

    memcpy(session->peer_public, info.public_key, TESLA_PUBKEY_LEN);
    memcpy(session->epoch, info.epoch, TESLA_EPOCH_LEN);
    memset(session->verifier, 0, sizeof(session->verifier));
    strncpy((char *)session->verifier, (const char *)vin, sizeof(session->verifier) - 1);
    session->counter = info.counter;
    session->clock_time = info.clock_time;
    session->time_zero_us = esp_timer_get_time() - (int64_t)info.clock_time * 1000000LL;
    session->ready = (info.status == 0);
    return ESP_OK;
}

bool tesla_crypto_verify_session_info(const tesla_session_t *session,
                                      const uint8_t challenge[TESLA_UUID_LEN],
                                      const uint8_t *session_info_bytes, size_t len, const uint8_t *tag,
                                      size_t tag_len)
{
    if (session == NULL || challenge == NULL || session_info_bytes == NULL || tag == NULL ||
        tag_len != 32) {
        return false;
    }
    uint8_t key[32];
    uint8_t meta_bytes[128];
    uint8_t mac[32];
    tesla_meta_t meta;
    uint8_t signature_type = TESLA_SIG_TYPE_HMAC;

    if (subkey(session->session_key, "session info", key) != ESP_OK) {
        return false;
    }
    tesla_meta_init(&meta, meta_bytes, sizeof(meta_bytes));
    tesla_meta_add(&meta, TESLA_TAG_SIGNATURE_TYPE, &signature_type, 1);
    tesla_meta_add(&meta, TESLA_TAG_PERSONALIZATION, session->verifier, strlen((char *)session->verifier));
    tesla_meta_add(&meta, TESLA_TAG_CHALLENGE, challenge, TESLA_UUID_LEN);
    if (!meta.ok) {
        return false;
    }

    // HMAC 覆盖 metadata + TAG_END + encodedInfo。
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL) {
        return false;
    }
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    uint8_t end = TESLA_TAG_END;
    bool ok = mbedtls_md_setup(&ctx, md, 1) == 0 && mbedtls_md_hmac_starts(&ctx, key, 32) == 0 &&
              mbedtls_md_hmac_update(&ctx, meta_bytes, meta.len) == 0 &&
              mbedtls_md_hmac_update(&ctx, &end, 1) == 0 &&
              mbedtls_md_hmac_update(&ctx, session_info_bytes, len) == 0 &&
              mbedtls_md_hmac_finish(&ctx, mac) == 0;
    mbedtls_md_free(&ctx);
    if (!ok) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < 32; i++) {
        diff |= (uint8_t)(mac[i] ^ tag[i]);
    }
    return diff == 0;
}

esp_err_t tesla_crypto_encrypt_command(tesla_session_t *session, const uint8_t *plaintext, size_t len,
                                       uint8_t *ciphertext, uint8_t nonce[TESLA_NONCE_LEN],
                                       uint8_t tag[TESLA_KEY_TAG_LEN], uint32_t *counter,
                                       uint32_t *expires_at)
{
    if (session == NULL || !session->ready || plaintext == NULL || len > 512) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t meta_bytes[160];
    uint8_t aad[32];
    tesla_meta_t meta;
    uint8_t signature_type = TESLA_SIG_TYPE_AES_GCM_PERSONALIZED;
    uint8_t domain = TESLA_DOMAIN_VCSEC;
    uint32_t flags = TESLA_FLAG_ENCRYPT_RESPONSE;

    int64_t elapsed_us = esp_timer_get_time() - session->time_zero_us;
    int64_t vehicle_seconds = elapsed_us / 1000000LL;
    uint32_t expire = (uint32_t)(vehicle_seconds + SESSION_TTL_SECONDS);

    session->counter++;
    if (session->counter == 0) {
        session->counter = 1;
    }

    tesla_meta_init(&meta, meta_bytes, sizeof(meta_bytes));
    tesla_meta_add(&meta, TESLA_TAG_SIGNATURE_TYPE, &signature_type, 1);
    tesla_meta_add(&meta, TESLA_TAG_DOMAIN, &domain, 1);
    tesla_meta_add(&meta, TESLA_TAG_PERSONALIZATION, session->verifier,
                   strlen((char *)session->verifier));
    tesla_meta_add(&meta, TESLA_TAG_EPOCH, session->epoch, TESLA_EPOCH_LEN);
    tesla_meta_add_u32(&meta, TESLA_TAG_EXPIRES_AT, expire);
    tesla_meta_add_u32(&meta, TESLA_TAG_COUNTER, session->counter);
    tesla_meta_add_u32(&meta, TESLA_TAG_FLAGS, flags);
    if (!meta.ok || sha256_meta(meta_bytes, meta.len, aad) != ESP_OK) {
        return ESP_FAIL;
    }

    tesla_crypto_random(nonce, TESLA_NONCE_LEN);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, session->session_key, 128);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, nonce, TESLA_NONCE_LEN, aad,
                                       sizeof(aad), plaintext, ciphertext, TESLA_KEY_TAG_LEN, tag);
    }
    mbedtls_gcm_free(&gcm);
    if (rc != 0) {
        return ESP_FAIL;
    }
    if (counter != NULL) {
        *counter = session->counter;
    }
    if (expires_at != NULL) {
        *expires_at = expire;
    }
    return ESP_OK;
}

esp_err_t tesla_crypto_decrypt_response(const tesla_session_t *session, const tesla_routable_t *message,
                                        const uint8_t request_id[1 + TESLA_KEY_TAG_LEN],
                                        uint8_t *plaintext, size_t cap, size_t *out_len)
{
    if (session == NULL || message == NULL || !message->has_response || !message->has_payload ||
        message->resp_nonce_len != TESLA_NONCE_LEN || message->resp_tag_len != TESLA_KEY_TAG_LEN ||
        message->payload_len > cap) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t meta_bytes[192];
    uint8_t aad[32];
    tesla_meta_t meta;
    uint8_t signature_type = TESLA_SIG_TYPE_AES_GCM_RESPONSE;
    uint8_t domain = (uint8_t)message->from_domain;
    uint32_t fault = message->has_fault ? message->fault : 0;

    tesla_meta_init(&meta, meta_bytes, sizeof(meta_bytes));
    tesla_meta_add(&meta, TESLA_TAG_SIGNATURE_TYPE, &signature_type, 1);
    tesla_meta_add(&meta, TESLA_TAG_DOMAIN, &domain, 1);
    tesla_meta_add(&meta, TESLA_TAG_PERSONALIZATION, session->verifier,
                   strlen((char *)session->verifier));
    tesla_meta_add_u32(&meta, TESLA_TAG_COUNTER, message->resp_counter);
    tesla_meta_add_u32(&meta, TESLA_TAG_FLAGS, message->flags);
    tesla_meta_add(&meta, TESLA_TAG_REQUEST_HASH, request_id, 1 + TESLA_KEY_TAG_LEN);
    tesla_meta_add_u32(&meta, TESLA_TAG_FAULT, fault);
    if (!meta.ok || sha256_meta(meta_bytes, meta.len, aad) != ESP_OK) {
        return ESP_FAIL;
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, session->session_key, 128);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&gcm, message->payload_len, message->resp_nonce, TESLA_NONCE_LEN,
                                      aad, sizeof(aad), message->resp_tag, TESLA_KEY_TAG_LEN,
                                      message->payload, plaintext);
    }
    mbedtls_gcm_free(&gcm);
    if (rc != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    if (out_len != NULL) {
        *out_len = message->payload_len;
    }
    return ESP_OK;
}
