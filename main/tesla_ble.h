// main/tesla_ble.h
// Tesla VCSEC BLE 中央设备（GATT client）：扫描、连接、握手、命令、授权。
//
// 只做 BLE 传输与协议编排；界面通过事件回调获知进度，命令在调用者任务中
// 同步阻塞执行（App 用单独 worker 任务调用）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define TESLA_BLE_ACTION_COUNT 6

typedef enum {
    TESLA_CMD_UNLOCK = 0,
    TESLA_CMD_LOCK,
    TESLA_CMD_FRUNK,
    TESLA_CMD_TRUNK,
    TESLA_CMD_TRUNK_CLOSE,
    TESLA_CMD_STATUS,
} tesla_cmd_t;

typedef enum {
    TESLA_BLE_IDLE = 0,
    TESLA_BLE_SCANNING,
    TESLA_BLE_CONNECTING,
    TESLA_BLE_HANDSHAKE,
    TESLA_BLE_READY,
    TESLA_BLE_PAIRING,
} tesla_ble_state_t;

typedef enum {
    TESLA_EVT_STATE = 0,
    TESLA_EVT_RESULT,
    TESLA_EVT_PAIR_PROGRESS,
} tesla_ble_evt_type_t;

// 错误码（非 0 即失败）。界面据此给出中文提示。
enum {
    TESLA_OK = 0,
    TESLA_ERR_NO_VIN = 1,
    TESLA_ERR_INIT = 2,
    TESLA_ERR_SCAN_TIMEOUT = 3,
    TESLA_ERR_CONNECT = 4,
    TESLA_ERR_SETUP = 5,
    TESLA_ERR_TX = 6,
    TESLA_ERR_TIMEOUT = 7,
    TESLA_ERR_PROTO = 8,
    TESLA_ERR_CRYPTO = 9,
    TESLA_ERR_NOT_AUTHORIZED = 10,
    TESLA_ERR_VEHICLE = 11,
    TESLA_ERR_BUSY = 12,
    TESLA_ERR_SESSION_STALE = 13,
};

typedef struct {
    uint8_t type;      // tesla_ble_evt_type_t
    uint8_t state;     // tesla_ble_state_t
    bool connected;
    bool authorized;
    int8_t rssi;
    int error;
    uint16_t detail;   // 车辆故障码 / 白名单结果码，用于界面给出具体原因
    uint8_t progress;  // 0..100，仅授权流程使用
} tesla_ble_event_t;

typedef void (*tesla_ble_event_cb_t)(const tesla_ble_event_t *event);

typedef struct {
    bool initialized;
    bool connected;
    bool authorized;
    uint8_t state;     // tesla_ble_state_t
    int8_t rssi;
    int last_error;
    uint16_t detail;
    char vehicle_name[19];
} tesla_ble_status_t;

// 最近一次 VCSEC 状态应答（锁车、车门、前后备箱、休眠）。valid 为 false 表示还没读到。
typedef struct {
    bool valid;
    uint8_t lock_state;      // 0 解锁 1 锁车 2 内锁 3 部分解锁
    bool doors_open;
    bool rear_trunk_open;
    bool front_trunk_open;
    bool charge_port_open;
    uint8_t sleep_status;    // 1 唤醒 2 休眠
    uint8_t user_presence;   // 1 不在 2 在
} tesla_vehicle_status_t;

#define TESLA_BLE_DISCOVERY_MAX 6
#define TESLA_BLE_PEER_NAME_LEN 32

// 授权页扫描到的一条周边蓝牙广播。
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    bool target;   // 广播名与本机 VIN 推导出的名称一致
    char name[TESLA_BLE_PEER_NAME_LEN];
} tesla_ble_peer_t;

// 初始化 NimBLE 与静态密钥（NVS）。成功后 BLE 栈常驻。
esp_err_t tesla_ble_init(tesla_ble_event_cb_t cb);
// 车辆 VIN 变更后调用，下一次命令会用新的广播名扫描。
void tesla_ble_set_vin(const char *vin);
void tesla_ble_get_status(tesla_ble_status_t *out);
void tesla_ble_get_vehicle_status(tesla_vehicle_status_t *out);
// 公钥指纹（8 个十六进制字符），用于核对车辆端添加的钥匙。
void tesla_ble_fingerprint(char out[9]);

// 同步执行一条命令，返回 TESLA_OK 或错误码。
int tesla_ble_execute(tesla_cmd_t cmd, uint32_t *lock_state);
// 添加钥匙：发送 PRESENT_KEY 请求并轮询握手，直到车辆确认授权。
int tesla_ble_pair(void);
// 与指定广播所属车辆对配（跳过名称扫描）。
int tesla_ble_pair_peer(const uint8_t addr[6], uint8_t addr_type);
// 授权页持续扫描周边广播；开始前会断开现有连接。
esp_err_t tesla_ble_discover_start(void);
void tesla_ble_discover_stop(void);
// 复制当前发现的广播列表（按发现顺序），返回条数。
int tesla_ble_discover_list(tesla_ble_peer_t *out, int max);
// 丢弃本机静态密钥并断开连接；下次使用会生成新密钥，需要重新授权。
esp_err_t tesla_ble_reset_key(void);
void tesla_ble_disconnect(void);
