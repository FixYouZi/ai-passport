// main/tesla_crypto.h
// Tesla BLE 会话密码学：P-256 静态密钥、ECDH、AES-128-GCM、HMAC-SHA256。
// 密钥持久化在 NVS（tesla_key/sk），首次启动自动生成。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "tesla_proto.h"

typedef struct {
    uint8_t session_key[16]; // SHA1(ECDH X)[:16]
    uint8_t epoch[TESLA_EPOCH_LEN];
    uint8_t verifier[18]; // VIN，最大 17 字符 + NUL
    uint8_t peer_public[TESLA_PUBKEY_LEN];
    uint8_t public_key[TESLA_PUBKEY_LEN];
    uint32_t counter;
    uint32_t clock_time;   // 握手时车辆时钟（秒）
    int64_t time_zero_us;  // 本地单调时钟对应的车辆 epoch 起点
    bool ready;
} tesla_session_t;

void tesla_crypto_random(void *out, size_t len);

// 加载或生成静态 P-256 私钥，返回未压缩公钥（0x04||X||Y）。
esp_err_t tesla_crypto_key_ensure(uint8_t public_key[TESLA_PUBKEY_LEN]);
esp_err_t tesla_crypto_key_erase(void);
// 公钥指纹：SHA1(public_key) 的前 8 个十六进制字符。
void tesla_crypto_fingerprint(const uint8_t public_key[TESLA_PUBKEY_LEN], char out[9]);

// 用车辆 SessionInfo 建立会话（ECDH + epoch/counter/时钟）。
esp_err_t tesla_crypto_session_apply(tesla_session_t *session, const uint8_t *vin,
                                     const uint8_t *session_info_bytes, size_t len);

// 校验车辆 SessionInfo 的 HMAC 标签。
bool tesla_crypto_verify_session_info(const tesla_session_t *session, const uint8_t challenge[TESLA_UUID_LEN],
                                      const uint8_t *session_info_bytes, size_t len,
                                      const uint8_t *tag, size_t tag_len);

// 加密命令：输出 nonce/tag，返回 ciphertext 长度；expires_at 相对车辆时钟。
esp_err_t tesla_crypto_encrypt_command(tesla_session_t *session, const uint8_t *plaintext, size_t len,
                                       uint8_t *ciphertext, uint8_t nonce[TESLA_NONCE_LEN],
                                       uint8_t tag[TESLA_KEY_TAG_LEN], uint32_t *counter,
                                       uint32_t *expires_at);

// 解密车辆响应（AAD 由响应元数据构造）。
esp_err_t tesla_crypto_decrypt_response(const tesla_session_t *session, const tesla_routable_t *message,
                                        const uint8_t request_id[1 + TESLA_KEY_TAG_LEN],
                                        uint8_t *plaintext, size_t cap, size_t *out_len);
