// main/tesla_ble.c
// Tesla VCSEC BLE 中央设备实现（NimBLE GATT client）。
//
// 帧格式与 teslamotors/vehicle-command 的 BLE 连接器一致：
//   每个 GATT 写入 = 2 字节大端总长度 + 负载分片，分片大小 = MTU - 3。
// 命令负载是 UnsignedMessage；添加钥匙使用明文 ToVCSECMessage（PRESENT_KEY）。
#include "tesla_ble.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"

#include "tesla_config.h"
#include "tesla_crypto.h"
#include "tesla_identity_esp.h"
#include "tesla_proto.h"

static const char *TAG = "tesla_ble";

#define BLE_SVC_UUID 0x0211
#define BLE_WRITE_UUID 0x0212
#define BLE_NOTIFY_UUID 0x0213

#define SCAN_TIMEOUT_MS 15000
#define CONNECT_TIMEOUT_MS 12000
#define SETUP_TIMEOUT_MS 8000
#define RX_TIMEOUT_MS 6000
// 配对：车辆回 WAIT（等待车机弹窗确认 + 刷卡）后进入人工确认窗口，
// 窗口内不重发加钥匙请求，给车辆和用户留出操作时间。
#define PAIR_WAIT_WINDOW_MS 120000
#define PAIR_RX_MS 1000
#define PAIR_PROBE_MS 4000
#define PAIR_RESEND_MS 30000
#define PAIR_MAX_SENDS 3
// 连接与 GATT 初始化最多尝试次数：一次性失败多来自车辆连接数已满等瞬时原因。
#define CONNECT_ATTEMPTS_PEER 3
#define CONNECT_ATTEMPTS_SCAN 2
#define CONNECT_RETRY_DELAY_MS 800

#define TESLA_NAME_LEN 18

static ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b, 0xf0, 0x43, 0xd1, 0xb2, 0x11,
                     0x02, 0x00, 0x00);
static ble_uuid128_t s_write_uuid =
    BLE_UUID128_INIT(0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b, 0xf0, 0x43, 0xd1, 0xb2, 0x12,
                     0x02, 0x00, 0x00);
static ble_uuid128_t s_notify_uuid =
    BLE_UUID128_INIT(0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b, 0xf0, 0x43, 0xd1, 0xb2, 0x13,
                     0x02, 0x00, 0x00);

static tesla_ble_event_cb_t s_event_cb;
static volatile bool s_initialized;
static volatile bool s_scanning;
static volatile bool s_conn_active;
static volatile bool s_ready;
static volatile bool s_authorized;
static volatile int s_last_error;
static volatile uint16_t s_detail;
static volatile int8_t s_rssi;
static volatile tesla_ble_state_t s_state;

static uint8_t s_own_addr_type;
static uint16_t s_conn;
static uint8_t s_vin[TESLA_VIN_BUF];
static char s_target_name[TESLA_BLE_NAME_BUFFER_SIZE];
static ble_addr_t s_peer_addr;
static bool s_peer_valid;

// 授权页选定的车辆：置位后 ensure_connected() 直接连接该地址。
static ble_addr_t s_pair_peer;
static bool s_pair_peer_valid;

// 授权页持续扫描结果。仅在 s_discovering 为真时由 GAP 回调更新。
typedef struct {
    bool used;
    bool connectable;
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
    bool target;
    char name[TESLA_BLE_PEER_NAME_LEN];
} disc_entry_t;

static disc_entry_t s_disc[TESLA_BLE_DISCOVERY_MAX];
static SemaphoreHandle_t s_disc_mutex;
static volatile bool s_discovering;

static const struct ble_gap_disc_params s_disc_params = {
    .itvl = 0x50,
    .window = 0x30,
    .passive = 0,
    .filter_duplicates = 0,
};

static uint16_t s_mtu = 23;
static uint16_t s_block_len = 20;
static uint16_t s_svc_start;
static uint16_t s_svc_end;
static uint16_t s_write_handle;
static uint16_t s_notify_handle;
static uint16_t s_cccd_handle;
static volatile int s_setup_err;
// GATT 初始化失败阶段：1=未找到服务 2=未找到特征 3=订阅通知失败 4=连接被断开。
static volatile uint8_t s_setup_stage;
static volatile uint8_t s_disc_reason;
static volatile int s_gatt_err;

static uint8_t s_from_routing[TESLA_UUID_LEN];
static tesla_session_t s_session;
static int64_t s_handshake_us;
static tesla_vehicle_status_t s_vehicle;

// 信息娱乐域会话独立于 VCSEC，用于读取车机电量。
static tesla_session_t s_inf_session;
static int64_t s_inf_handshake_us;
static volatile int s_charge_percent = -1;

static SemaphoreHandle_t s_sync_sem;
static SemaphoreHandle_t s_link_sem;
static SemaphoreHandle_t s_mtu_sem;
static SemaphoreHandle_t s_setup_sem;
static SemaphoreHandle_t s_rx_sem;
static SemaphoreHandle_t s_msg_mutex;

static uint8_t s_rx_buf[2 + TESLA_MAX_MESSAGE];
static size_t s_rx_len;
static uint8_t s_msg[TESLA_MAX_MESSAGE];
static size_t s_msg_len;
static uint8_t s_frame[TESLA_MAX_MESSAGE];
static size_t s_frame_len;
static uint8_t s_chunk[600];

static void emit(tesla_ble_evt_type_t type, int error, uint8_t progress)
{
    if (s_event_cb == NULL) {
        return;
    }
    tesla_ble_event_t event = {
        .type = (uint8_t)type,
        .state = (uint8_t)s_state,
        .connected = s_conn_active,
        .authorized = s_authorized,
        .rssi = s_rssi,
        .error = error,
        .detail = s_detail,
        .progress = progress,
    };
    s_event_cb(&event);
}

static void set_state(tesla_ble_state_t state, int error)
{
    s_state = state;
    if (error != TESLA_OK) {
        s_last_error = error;
    }
    emit(TESLA_EVT_STATE, error, 0);
}

// --------------------------------------------------------------------------
// RX 重组
// --------------------------------------------------------------------------
static void rx_reset(void)
{
    s_rx_len = 0;
    while (xSemaphoreTake(s_rx_sem, 0) == pdTRUE) {
    }
}

static void rx_append(const uint8_t *data, size_t len)
{
    if (s_rx_len + len > sizeof(s_rx_buf)) {
        s_rx_len = 0;
        return;
    }
    memcpy(s_rx_buf + s_rx_len, data, len);
    s_rx_len += len;
    if (s_rx_len < 2) {
        return;
    }
    size_t need = ((size_t)s_rx_buf[0] << 8) | s_rx_buf[1];
    if (need == 0 || need > TESLA_MAX_MESSAGE) {
        s_rx_len = 0;
        return;
    }
    if (s_rx_len < 2 + need) {
        return;
    }
    size_t rest = s_rx_len - 2 - need;
    if (xSemaphoreTake(s_msg_mutex, portMAX_DELAY) == pdTRUE) {
        memcpy(s_msg, s_rx_buf + 2, need);
        s_msg_len = need;
        xSemaphoreGive(s_msg_mutex);
    }
    memmove(s_rx_buf, s_rx_buf + 2 + need, rest);
    s_rx_len = rest;
    (void)xSemaphoreGive(s_rx_sem);
}

static bool wait_message(uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    if (xSemaphoreTake(s_msg_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    s_frame_len = s_msg_len;
    if (s_frame_len > sizeof(s_frame)) {
        s_frame_len = 0;
    }
    memcpy(s_frame, s_msg, s_frame_len);
    xSemaphoreGive(s_msg_mutex);
    return s_frame_len > 0;
}

// --------------------------------------------------------------------------
// GATT 发现
// --------------------------------------------------------------------------
static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg);

static int cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    if (error != NULL && error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "订阅通知失败：status=%d", error->status);
        s_gatt_err = error->status;
        s_setup_err = TESLA_ERR_SETUP;
        s_setup_stage = 3;
    }
    (void)xSemaphoreGive(s_setup_sem);
    return 0;
}

static int chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        if (s_write_handle == 0 || s_notify_handle == 0) {
            ESP_LOGE(TAG, "特征缺失：write=%u notify=%u", s_write_handle, s_notify_handle);
            s_setup_err = TESLA_ERR_SETUP;
            s_setup_stage = 2;
            (void)xSemaphoreGive(s_setup_sem);
            return 0;
        }
        ESP_LOGI(TAG, "特征就绪：write=%u notify=%u", s_write_handle, s_notify_handle);
        s_setup_stage = 3;
        int rc = ble_gattc_disc_all_dscs(conn_handle, s_notify_handle, s_svc_end, dsc_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "描述符发现启动失败：rc=%d", rc);
            s_setup_err = TESLA_ERR_SETUP;
            s_setup_stage = 3;
            (void)xSemaphoreGive(s_setup_sem);
        }
        return 0;
    }
    if (error != NULL && error->status != 0) {
        ESP_LOGE(TAG, "特征发现失败：status=%d", error->status);
        s_gatt_err = error->status;
        s_setup_err = TESLA_ERR_SETUP;
        s_setup_stage = 2;
        (void)xSemaphoreGive(s_setup_sem);
        return 0;
    }
    if (chr != NULL) {
        if (ble_uuid_cmp(&chr->uuid.u, &s_write_uuid.u) == 0) {
            s_write_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &s_notify_uuid.u) == 0) {
            s_notify_handle = chr->val_handle;
        }
    }
    return 0;
}

static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn_handle;
    (void)chr_val_handle;
    (void)arg;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        if (s_cccd_handle == 0) {
            // 少数实现把 CCCD 紧跟在值句柄之后。
            s_cccd_handle = (uint16_t)(s_notify_handle + 1);
        }
        ESP_LOGI(TAG, "订阅通知：cccd=%u", s_cccd_handle);
        uint8_t cfg[2] = {0x01, 0x00};
        int rc = ble_gattc_write_flat(conn_handle, s_cccd_handle, cfg, sizeof(cfg), cccd_write_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "订阅写入启动失败：rc=%d", rc);
            s_setup_err = TESLA_ERR_SETUP;
            s_setup_stage = 3;
            (void)xSemaphoreGive(s_setup_sem);
        }
        return 0;
    }
    if (error != NULL && error->status != 0) {
        ESP_LOGE(TAG, "描述符发现失败：status=%d", error->status);
        s_gatt_err = error->status;
        s_setup_err = TESLA_ERR_SETUP;
        s_setup_stage = 3;
        (void)xSemaphoreGive(s_setup_sem);
        return 0;
    }
    if (dsc != NULL && dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == 0x2902) {
        s_cccd_handle = dsc->handle;
    }
    return 0;
}

static int svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (error != NULL && error->status == BLE_HS_EDONE) {
        if (s_svc_start == 0 || s_svc_end == 0) {
            ESP_LOGE(TAG, "未找到特斯拉服务");
            s_setup_err = TESLA_ERR_SETUP;
            s_setup_stage = 1;
            (void)xSemaphoreGive(s_setup_sem);
            return 0;
        }
        ESP_LOGI(TAG, "服务就绪：start=%u end=%u", s_svc_start, s_svc_end);
        s_setup_stage = 2;
        int rc = ble_gattc_disc_all_chrs(conn_handle, s_svc_start, s_svc_end, chr_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "特征发现启动失败：rc=%d", rc);
            s_setup_err = TESLA_ERR_SETUP;
            s_setup_stage = 2;
            (void)xSemaphoreGive(s_setup_sem);
        }
        return 0;
    }
    if (error != NULL && error->status != 0) {
        ESP_LOGE(TAG, "服务发现失败：status=%d", error->status);
        s_gatt_err = error->status;
        s_setup_err = TESLA_ERR_SETUP;
        s_setup_stage = 1;
        (void)xSemaphoreGive(s_setup_sem);
        return 0;
    }
    if (svc != NULL && svc->start_handle != 0xFFFF &&
        ble_uuid_cmp(&svc->uuid.u, &s_svc_uuid.u) == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
    }
    return 0;
}

static int mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error == NULL || error->status == 0 || error->status == BLE_HS_EDONE) {
        if (mtu > 23) {
            s_mtu = mtu;
        }
    }
    (void)xSemaphoreGive(s_mtu_sem);
    return 0;
}

// --------------------------------------------------------------------------
// GAP
// --------------------------------------------------------------------------
static bool name_matches_target(const uint8_t *name, uint8_t len)
{
    if (s_target_name[0] == '\0' || name == NULL || len != TESLA_NAME_LEN) {
        return false;
    }
    // VIN 推导名形如 S<16 hex>C，最后一位是信标位置（C/R/D/P），均视为本车。
    if (memcmp(name, s_target_name, TESLA_NAME_LEN - 1) != 0) {
        return false;
    }
    char suffix = (char)name[TESLA_NAME_LEN - 1];
    return suffix == 'C' || suffix == 'R' || suffix == 'D' || suffix == 'P' || suffix == 'c' ||
           suffix == 'r' || suffix == 'd' || suffix == 'p';
}

// 记录一条广播：同地址刷新名称与信号强度；表满时保留目标车辆并替换最弱的一条。
static void discovery_record(const struct ble_gap_event *event,
                             const struct ble_hs_adv_fields *fields)
{
    if (s_disc_mutex == NULL || xSemaphoreTake(s_disc_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    const uint8_t *addr = event->disc.addr.val;
    bool target = name_matches_target(fields->name, fields->name_len);
    int match = -1;
    int free_slot = -1;
    int weakest = -1;
    for (int i = 0; i < TESLA_BLE_DISCOVERY_MAX; i++) {
        if (!s_disc[i].used) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (s_disc[i].addr_type == event->disc.addr.type &&
            memcmp(s_disc[i].addr, addr, sizeof(s_disc[i].addr)) == 0) {
            match = i;
        } else if (!s_disc[i].target &&
                   (weakest < 0 || s_disc[i].rssi < s_disc[weakest].rssi)) {
            weakest = i;
        }
    }
    if (match < 0) {
        match = free_slot;
        if (match < 0) {
            if (target || (weakest >= 0 && event->disc.rssi > s_disc[weakest].rssi)) {
                match = weakest;
            }
        }
        if (match < 0) {
            xSemaphoreGive(s_disc_mutex);
            return;
        }
        memset(&s_disc[match], 0, sizeof(s_disc[match]));
        s_disc[match].used = true;
        s_disc[match].addr_type = event->disc.addr.type;
        memcpy(s_disc[match].addr, addr, sizeof(s_disc[match].addr));
    }
    s_disc[match].rssi = (int8_t)event->disc.rssi;
    switch (event->disc.event_type) {
    case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
    case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:
        s_disc[match].connectable = true;
        break;
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
    case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:
        s_disc[match].connectable = false;
        break;
    default:
        // 扫描响应不改变可连接性。
        break;
    }
    if (fields->name != NULL && fields->name_len > 0) {
        size_t len = fields->name_len < sizeof(s_disc[match].name) - 1
                         ? fields->name_len
                         : sizeof(s_disc[match].name) - 1;
        memcpy(s_disc[match].name, fields->name, len);
        s_disc[match].name[len] = '\0';
        s_disc[match].target = target;
    }
    xSemaphoreGive(s_disc_mutex);
}

static bool adv_matches(const struct ble_hs_adv_fields *fields)
{
    if (name_matches_target(fields->name, fields->name_len)) {
        return true;
    }
    for (uint8_t i = 0; i < fields->num_uuids128; i++) {
        if (ble_uuid_cmp(&fields->uuids128[i].u, &s_svc_uuid.u) == 0) {
            return true;
        }
    }
    return false;
}

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_scan(void)
{
    s_scanning = true;
    set_state(TESLA_BLE_SCANNING, TESLA_OK);
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &s_disc_params, gap_event, NULL);
    if (rc != 0) {
        s_scanning = false;
        s_last_error = TESLA_ERR_SCAN_TIMEOUT;
        (void)xSemaphoreGive(s_link_sem);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
            return 0;
        }
        if (s_discovering) {
            // 授权页：只记录广播，等待用户确认后再连接。
            discovery_record(event, &fields);
            return 0;
        }
        s_rssi = (int8_t)event->disc.rssi;
        if (s_pair_peer_valid) {
            // 已选定车辆：只连接用户选择的地址，避免被其他广播抢占。
            return 0;
        }
        if (!adv_matches(&fields)) {
            return 0;
        }
        s_peer_addr = event->disc.addr;
        s_peer_valid = true;
        (void)ble_gap_disc_cancel();
        s_scanning = false;
        set_state(TESLA_BLE_CONNECTING, TESLA_OK);
        int rc = ble_gap_connect(s_own_addr_type, &s_peer_addr, CONNECT_TIMEOUT_MS, NULL,
                                 gap_event, NULL);
        if (rc != 0) {
            s_last_error = TESLA_ERR_CONNECT;
            (void)xSemaphoreGive(s_link_sem);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        s_discovering = false;
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_conn_active = true;
            s_last_error = TESLA_OK;
            ESP_LOGI(TAG, "连接成功：handle=%u", s_conn);
        } else {
            ESP_LOGW(TAG, "连接失败：status=%d", event->connect.status);
            s_conn_active = false;
            s_last_error = TESLA_ERR_CONNECT;
        }
        (void)xSemaphoreGive(s_link_sem);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_disc_reason = (uint8_t)event->disconnect.reason;
        if (s_setup_stage != 0) {
            s_setup_stage = 4;
        }
        ESP_LOGW(TAG, "连接断开：reason=0x%02x 阶段=%u", s_disc_reason, s_setup_stage);
        s_conn_active = false;
        s_ready = false;
        s_scanning = false;
        s_rx_len = 0;
        memset(&s_inf_session, 0, sizeof(s_inf_session));
        s_inf_handshake_us = 0;
        set_state(TESLA_BLE_IDLE, TESLA_OK);
        (void)xSemaphoreGive(s_setup_sem);
        (void)xSemaphoreGive(s_rx_sem);
        return 0;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU 协商完成：%u", s_mtu);
        (void)xSemaphoreGive(s_mtu_sem);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.attr_handle != s_notify_handle) {
            return 0;
        }
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (len > sizeof(s_chunk)) {
            return 0;
        }
        if (os_mbuf_copydata(event->notify_rx.om, 0, len, s_chunk) != 0) {
            return 0;
        }
        rx_append(s_chunk, len);
        return 0;
    }
    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    s_conn_active = false;
    s_ready = false;
    s_scanning = false;
    s_discovering = false;
    s_last_error = TESLA_ERR_INIT;
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) == 0) {
        (void)ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    (void)xSemaphoreGive(s_sync_sem);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    vTaskDelete(NULL);
}

// --------------------------------------------------------------------------
// 发送与连接
// --------------------------------------------------------------------------
static int send_frame(const uint8_t *data, size_t len)
{
    if (!s_conn_active || s_write_handle == 0 || len > TESLA_MAX_MESSAGE) {
        return -1;
    }
    uint8_t header[2] = {(uint8_t)(len >> 8), (uint8_t)(len & 0xff)};
    int rc = ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, header, sizeof(header));
    if (rc != 0) {
        return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(4));
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = s_block_len;
        if (chunk > len - offset) {
            chunk = len - offset;
        }
        rc = ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, data + offset, (uint16_t)chunk);
        if (rc != 0) {
            return -1;
        }
        offset += chunk;
        vTaskDelay(pdMS_TO_TICKS(6));
    }
    return 0;
}

static void disconnect_locked(void)
{
    if (s_conn_active) {
        (void)ble_gap_terminate(s_conn, BLE_ERR_CONN_TERM_LOCAL);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    s_conn_active = false;
    s_ready = false;
    s_handshake_us = 0;
}

void tesla_ble_disconnect(void)
{
    disconnect_locked();
    set_state(TESLA_BLE_IDLE, TESLA_OK);
}

esp_err_t tesla_ble_reset_key(void)
{
    disconnect_locked();
    esp_err_t err = tesla_crypto_key_erase();
    s_authorized = false;
    s_ready = false;
    s_detail = 0;
    memset(&s_vehicle, 0, sizeof(s_vehicle));
    set_state(TESLA_BLE_IDLE, TESLA_OK);
    return err;
}

esp_err_t tesla_ble_discover_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_discovering) {
        return ESP_OK;
    }
    disconnect_locked();
    if (s_scanning) {
        (void)ble_gap_disc_cancel();
        s_scanning = false;
    }
    if (s_disc_mutex != NULL && xSemaphoreTake(s_disc_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(s_disc, 0, sizeof(s_disc));
        xSemaphoreGive(s_disc_mutex);
    }
    s_pair_peer_valid = false;
    s_discovering = true;
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &s_disc_params, gap_event, NULL);
    if (rc != 0) {
        s_discovering = false;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void tesla_ble_discover_stop(void)
{
    if (!s_discovering) {
        return;
    }
    s_discovering = false;
    (void)ble_gap_disc_cancel();
}

int tesla_ble_discover_list(tesla_ble_peer_t *out, int max)
{
    if (out == NULL || max <= 0 || s_disc_mutex == NULL) {
        return 0;
    }
    if (xSemaphoreTake(s_disc_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < TESLA_BLE_DISCOVERY_MAX && count < max; i++) {
        if (!s_disc[i].used) {
            continue;
        }
        memcpy(out[count].addr, s_disc[i].addr, sizeof(out[count].addr));
        out[count].addr_type = s_disc[i].addr_type;
        out[count].rssi = s_disc[i].rssi;
        out[count].target = s_disc[i].target;
        memcpy(out[count].name, s_disc[i].name, sizeof(out[count].name));
        count++;
    }
    xSemaphoreGive(s_disc_mutex);
    return count;
}

// 单次连接与 GATT 初始化。fresh_scan 为真时忽略选定地址，重新按广播名扫描，
// 用于车辆旋转随机地址或上次连接残留导致失败的场景。
static int connect_once(bool fresh_scan)
{
    s_ready = false;
    s_peer_valid = false;
    s_last_error = TESLA_OK;
    s_setup_stage = 0;
    s_gatt_err = 0;
    while (xSemaphoreTake(s_link_sem, 0) == pdTRUE) {
    }
    if (s_pair_peer_valid && !fresh_scan) {
        // 授权页选定的车辆：直接连接该广播地址。
        s_peer_addr = s_pair_peer;
        s_peer_valid = true;
        set_state(TESLA_BLE_CONNECTING, TESLA_OK);
        ESP_LOGI(TAG, "连接选定车辆：%02x:%02x:%02x:%02x:%02x:%02x (type=%u)",
                 s_peer_addr.val[5], s_peer_addr.val[4], s_peer_addr.val[3], s_peer_addr.val[2],
                 s_peer_addr.val[1], s_peer_addr.val[0], s_peer_addr.type);
        int rc = ble_gap_connect(s_own_addr_type, &s_peer_addr, CONNECT_TIMEOUT_MS, NULL,
                                 gap_event, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "连接发起失败：rc=%d", rc);
            s_last_error = TESLA_ERR_CONNECT;
            set_state(TESLA_BLE_IDLE, TESLA_ERR_CONNECT);
            return TESLA_ERR_CONNECT;
        }
    } else {
        start_scan();
    }
    if (xSemaphoreTake(s_link_sem, pdMS_TO_TICKS(SCAN_TIMEOUT_MS)) != pdTRUE) {
        if (s_scanning) {
            (void)ble_gap_disc_cancel();
            s_scanning = false;
        }
        int err = s_pair_peer_valid ? TESLA_ERR_CONNECT : TESLA_ERR_SCAN_TIMEOUT;
        ESP_LOGW(TAG, "等待连接超时：err=%d", err);
        set_state(TESLA_BLE_IDLE, err);
        return err;
    }
    if (!s_conn_active) {
        int err = s_last_error ? s_last_error : TESLA_ERR_CONNECT;
        set_state(TESLA_BLE_IDLE, err);
        return err;
    }

    // 与官方 vehicle-command 一致：先发现服务/特征并订阅通知，最后再协商 MTU。
    s_setup_err = 0;
    s_setup_stage = 1;
    s_svc_start = 0;
    s_svc_end = 0;
    s_write_handle = 0;
    s_notify_handle = 0;
    s_cccd_handle = 0;
    while (xSemaphoreTake(s_setup_sem, 0) == pdTRUE) {
    }
    if (ble_gattc_disc_svc_by_uuid(s_conn, &s_svc_uuid.u, svc_cb, NULL) != 0) {
        ESP_LOGE(TAG, "服务发现启动失败");
        goto setup_failed;
    }
    if (xSemaphoreTake(s_setup_sem, pdMS_TO_TICKS(SETUP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "GATT 初始化超时，阶段=%u", s_setup_stage);
        goto setup_failed;
    }
    if (s_setup_err != 0) {
        goto setup_failed;
    }
    if (!s_conn_active) {
        s_setup_stage = 4;
        goto setup_failed;
    }

    // 订阅完成后再协商 MTU；失败只降级分片大小，不阻断流程。
    s_mtu = 23;
    while (xSemaphoreTake(s_mtu_sem, 0) == pdTRUE) {
    }
    if (ble_gattc_exchange_mtu(s_conn, mtu_cb, NULL) == 0) {
        if (xSemaphoreTake(s_mtu_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "MTU 协商无应答，沿用 %u", s_mtu);
        }
    } else {
        ESP_LOGW(TAG, "MTU 协商启动失败");
    }
    s_block_len = (s_mtu > 23) ? (uint16_t)(s_mtu - 3) : 20;
    if (s_block_len > TESLA_MAX_MESSAGE) {
        s_block_len = TESLA_MAX_MESSAGE;
    }

    s_setup_stage = 0;
    tesla_crypto_random(s_from_routing, sizeof(s_from_routing));
    s_ready = true;
    set_state(TESLA_BLE_READY, TESLA_OK);
    return TESLA_OK;

setup_failed:
    if (s_setup_stage == 0) {
        s_setup_stage = 1;
    }
    if (s_setup_stage == 4 && s_disc_reason != 0) {
        s_gatt_err = s_disc_reason;
    }
    // 高 8 位带错误码、低 4 位带阶段，界面据此显示可读诊断。
    s_detail = (uint16_t)(((uint16_t)(s_gatt_err & 0xff) << 4) | (s_setup_stage & 0x0f));
    ESP_LOGE(TAG, "GATT 初始化失败：阶段=%u 错误码=%d", s_setup_stage, s_gatt_err);
    disconnect_locked();
    set_state(TESLA_BLE_IDLE, TESLA_ERR_SETUP);
    return TESLA_ERR_SETUP;
}

static int ensure_connected(void)
{
    if (s_vin[0] == '\0') {
        return TESLA_ERR_NO_VIN;
    }
    if (s_conn_active && s_ready) {
        return TESLA_OK;
    }
    if (s_conn_active) {
        disconnect_locked();
    }
    if (tesla_ble_name_from_vin((const char *)s_vin, s_target_name) != ESP_OK) {
        return TESLA_ERR_NO_VIN;
    }
    int attempts = s_pair_peer_valid ? CONNECT_ATTEMPTS_PEER : CONNECT_ATTEMPTS_SCAN;
    int last = TESLA_ERR_CONNECT;
    bool fresh_scan = false;
    for (int attempt = 1; attempt <= attempts; attempt++) {
        int rc = connect_once(fresh_scan);
        if (rc == TESLA_OK) {
            return TESLA_OK;
        }
        last = rc;
        // GATT 阶段失败时重新扫描目标广播再连，避免沿用已失效的随机地址。
        if (rc == TESLA_ERR_SETUP) {
            fresh_scan = true;
            s_pair_peer_valid = false;
        }
        disconnect_locked();
        if (attempt < attempts) {
            ESP_LOGW(TAG, "连接重试 %d/%d（上次错误 %d）", attempt, attempts, rc);
            vTaskDelay(pdMS_TO_TICKS(CONNECT_RETRY_DELAY_MS));
        }
    }
    set_state(TESLA_BLE_IDLE, last);
    return last;
}

// --------------------------------------------------------------------------
// 协议流程
// --------------------------------------------------------------------------
// 解析车辆对“添加钥匙”请求的应答（未加密的 VCSEC commandStatus）。
// 返回 TESLA_OK 表示已加入白名单，TESLA_ERR_BUSY 表示车辆等待人工确认，
// TESLA_ERR_VEHICLE 表示明确拒绝（s_detail 为白名单原因码）。
static int parse_pair_reply(const uint8_t *buf, size_t len)
{
    tesla_routable_t message;
    if (!tesla_proto_parse_routable(buf, len, &message)) {
        return TESLA_ERR_PROTO;
    }
    if (message.has_fault && message.fault != 0) {
        s_detail = (uint16_t)message.fault;
        return TESLA_ERR_VEHICLE;
    }
    if (!message.has_payload || message.payload_len == 0) {
        return TESLA_ERR_PROTO;
    }
    tesla_vcsec_status_t status;
    if (!tesla_proto_parse_vcsec(message.payload, message.payload_len, &status)) {
        return TESLA_ERR_PROTO;
    }
    if (status.has_whitelist_status) {
        // 13 = 本机公钥已在白名单，可视为配对完成。
        if (status.whitelist_information == 13) {
            return TESLA_OK;
        }
        if (status.whitelist_information != 0 || status.whitelist_status == 2) {
            s_detail = (uint16_t)status.whitelist_information;
            return TESLA_ERR_VEHICLE;
        }
        if (status.whitelist_status == 1) {
            return TESLA_ERR_BUSY;
        }
    }
    if (status.has_operation_status) {
        if (status.operation_status == 0) {
            return TESLA_OK;
        }
        if (status.operation_status == 1) {
            return TESLA_ERR_BUSY;
        }
        return TESLA_ERR_VEHICLE;
    }
    return TESLA_ERR_PROTO;
}

static int handshake_domain(uint8_t domain, tesla_session_t *session, int64_t *stamp, bool quiet)
{
    uint8_t public_key[TESLA_PUBKEY_LEN];
    uint8_t uuid[TESLA_UUID_LEN];
    uint8_t routing[TESLA_UUID_LEN];
    uint8_t request[512];

    if (tesla_crypto_key_ensure(public_key) != ESP_OK) {
        return TESLA_ERR_CRYPTO;
    }
    tesla_crypto_random(uuid, sizeof(uuid));
    tesla_crypto_random(routing, sizeof(routing));
    size_t len =
        tesla_proto_session_info_request(domain, public_key, uuid, routing, request, sizeof(request));
    if (len == 0) {
        return TESLA_ERR_PROTO;
    }

    if (!quiet) {
        set_state(TESLA_BLE_HANDSHAKE, TESLA_OK);
    }
    rx_reset();
    if (send_frame(request, len) != 0) {
        return TESLA_ERR_TX;
    }
    if (!wait_message(RX_TIMEOUT_MS)) {
        return TESLA_ERR_TIMEOUT;
    }

    tesla_routable_t message;
    if (!tesla_proto_parse_routable(s_frame, s_frame_len, &message)) {
        return TESLA_ERR_PROTO;
    }
    if (message.has_fault && message.fault != 0) {
        s_detail = (uint16_t)message.fault;
        return TESLA_ERR_VEHICLE;
    }
    if (!message.has_session_info || message.session_info_tag == NULL ||
        message.session_info_tag_len != 32) {
        // 配对期间车辆可能直接回未加密的 commandStatus（含拒绝原因），
        // 把它透出给调用方，避免把明确拒绝当成协议错误而空等。
        if (parse_pair_reply(s_frame, s_frame_len) == TESLA_ERR_VEHICLE) {
            return TESLA_ERR_VEHICLE;
        }
        return TESLA_ERR_PROTO;
    }
    if (message.has_request_uuid &&
        (message.request_uuid_len != sizeof(uuid) ||
         memcmp(message.request_uuid, uuid, sizeof(uuid)) != 0)) {
        return TESLA_ERR_PROTO;
    }

    memset(session, 0, sizeof(*session));
    if (tesla_crypto_session_apply(session, s_vin, message.session_info,
                                   message.session_info_len) != ESP_OK) {
        return TESLA_ERR_CRYPTO;
    }
    if (!tesla_crypto_verify_session_info(session, uuid, message.session_info,
                                          message.session_info_len, message.session_info_tag,
                                          message.session_info_tag_len)) {
        memset(session, 0, sizeof(*session));
        return TESLA_ERR_CRYPTO;
    }
    if (!session->ready) {
        return TESLA_ERR_NOT_AUTHORIZED;
    }
    *stamp = esp_timer_get_time();
    if (domain == TESLA_DOMAIN_VCSEC) {
        s_authorized = true;
        s_ready = true;
        s_detail = 0;
        set_state(TESLA_BLE_READY, TESLA_OK);
    }
    return TESLA_OK;
}

static int do_handshake_impl(bool quiet)
{
    return handshake_domain(TESLA_DOMAIN_VCSEC, &s_session, &s_handshake_us, quiet);
}

static int do_handshake(void)
{
    return do_handshake_impl(false);
}

static int send_command(const uint8_t *payload, size_t payload_len, uint32_t *lock_state)
{
    uint8_t ciphertext[512];
    uint8_t nonce[TESLA_NONCE_LEN];
    uint8_t tag[TESLA_KEY_TAG_LEN];
    uint8_t uuid[TESLA_UUID_LEN];
    uint8_t request_id[1 + TESLA_KEY_TAG_LEN];
    uint8_t packet[768];
    uint32_t counter = 0;
    uint32_t expires_at = 0;

    if (payload_len > sizeof(ciphertext)) {
        return TESLA_ERR_PROTO;
    }
    if (tesla_crypto_encrypt_command(&s_session, payload, payload_len, ciphertext, nonce, tag,
                                     &counter, &expires_at) != ESP_OK) {
        return TESLA_ERR_CRYPTO;
    }
    tesla_gcm_sig_t signature = {
        .epoch = s_session.epoch,
        .epoch_len = TESLA_EPOCH_LEN,
        .nonce = nonce,
        .nonce_len = TESLA_NONCE_LEN,
        .counter = counter,
        .expires_at = expires_at,
        .tag = tag,
        .tag_len = TESLA_KEY_TAG_LEN,
    };
    tesla_crypto_random(uuid, sizeof(uuid));
    size_t packet_len = tesla_proto_encrypted_envelope(
        TESLA_DOMAIN_VCSEC, s_session.public_key, &signature, ciphertext, payload_len,
        s_from_routing, uuid, TESLA_FLAG_ENCRYPT_RESPONSE, packet, sizeof(packet));
    if (packet_len == 0) {
        return TESLA_ERR_PROTO;
    }
    request_id[0] = TESLA_SIG_TYPE_AES_GCM_PERSONALIZED;
    memcpy(request_id + 1, tag, TESLA_KEY_TAG_LEN);

    for (int attempt = 0; attempt < 3; attempt++) {
        rx_reset();
        if (send_frame(packet, packet_len) != 0) {
            return TESLA_ERR_TX;
        }
        if (!wait_message(RX_TIMEOUT_MS)) {
            return TESLA_ERR_TIMEOUT;
        }
        tesla_routable_t message;
        if (!tesla_proto_parse_routable(s_frame, s_frame_len, &message)) {
            return TESLA_ERR_PROTO;
        }
        if (message.has_fault && message.fault != 0) {
            s_detail = (uint16_t)message.fault;
            // 5=签名失效 6=计数器失效 15=epoch 不符：会话已过期，重连后重新握手。
            if (message.fault == 5 || message.fault == 6 || message.fault == 15) {
                return TESLA_ERR_SESSION_STALE;
            }
            if (message.fault == 1) {
                return TESLA_ERR_BUSY;
            }
            return TESLA_ERR_VEHICLE;
        }
        if (!message.has_response || !message.has_payload) {
            continue;
        }
        uint8_t plaintext[512];
        size_t plaintext_len = 0;
        if (tesla_crypto_decrypt_response(&s_session, &message, request_id, plaintext,
                                          sizeof(plaintext), &plaintext_len) != ESP_OK) {
            return TESLA_ERR_CRYPTO;
        }
        tesla_vcsec_status_t status;
        if (!tesla_proto_parse_vcsec(plaintext, plaintext_len, &status)) {
            return TESLA_ERR_PROTO;
        }
        if (status.has_vehicle_status) {
            if (status.has_lock_state) {
                s_vehicle.lock_state = (uint8_t)status.lock_state;
            }
            if (status.has_doors) {
                s_vehicle.doors_open = status.doors_open;
            }
            if (status.has_rear_trunk) {
                s_vehicle.rear_trunk_open = status.rear_trunk_open;
            }
            if (status.has_front_trunk) {
                s_vehicle.front_trunk_open = status.front_trunk_open;
            }
            if (status.has_charge_port) {
                s_vehicle.charge_port_open = status.charge_port_open;
            }
            if (status.has_sleep_status) {
                s_vehicle.sleep_status = (uint8_t)status.sleep_status;
            }
            if (status.has_user_presence) {
                s_vehicle.user_presence = (uint8_t)status.user_presence;
            }
            s_vehicle.valid = true;
        }
        if (lock_state != NULL && status.has_lock_state) {
            *lock_state = status.lock_state;
        }
        if (status.has_operation_status) {
            if (status.operation_status == 2) {
                if (status.has_whitelist_status) {
                    s_detail = (uint16_t)status.whitelist_information;
                }
                return TESLA_ERR_VEHICLE;
            }
            if (status.operation_status == 1) {
                return TESLA_ERR_BUSY;
            }
        }
        if (status.has_whitelist_status && status.whitelist_information != 0) {
            s_detail = (uint16_t)status.whitelist_information;
            return TESLA_ERR_VEHICLE;
        }
        return TESLA_OK;
    }
    return TESLA_ERR_TIMEOUT;
}

// --------------------------------------------------------------------------
// 信息娱乐域：读取车机电量
// --------------------------------------------------------------------------
static int send_inf_request(const uint8_t *payload, size_t payload_len, uint8_t *plain,
                            size_t plain_cap, size_t *plain_len)
{
    uint8_t ciphertext[512];
    uint8_t nonce[TESLA_NONCE_LEN];
    uint8_t tag[TESLA_KEY_TAG_LEN];
    uint8_t uuid[TESLA_UUID_LEN];
    uint8_t request_id[1 + TESLA_KEY_TAG_LEN];
    uint8_t packet[768];
    uint32_t counter = 0;
    uint32_t expires_at = 0;

    if (payload_len > sizeof(ciphertext)) {
        return TESLA_ERR_PROTO;
    }
    if (tesla_crypto_encrypt_command(&s_inf_session, payload, payload_len, ciphertext, nonce, tag,
                                     &counter, &expires_at) != ESP_OK) {
        return TESLA_ERR_CRYPTO;
    }
    tesla_gcm_sig_t signature = {
        .epoch = s_inf_session.epoch,
        .epoch_len = TESLA_EPOCH_LEN,
        .nonce = nonce,
        .nonce_len = TESLA_NONCE_LEN,
        .counter = counter,
        .expires_at = expires_at,
        .tag = tag,
        .tag_len = TESLA_KEY_TAG_LEN,
    };
    tesla_crypto_random(uuid, sizeof(uuid));
    size_t packet_len = tesla_proto_encrypted_envelope(
        TESLA_DOMAIN_INFOTAINMENT, s_inf_session.public_key, &signature, ciphertext, payload_len,
        s_from_routing, uuid, TESLA_FLAG_ENCRYPT_RESPONSE, packet, sizeof(packet));
    if (packet_len == 0) {
        return TESLA_ERR_PROTO;
    }
    request_id[0] = TESLA_SIG_TYPE_AES_GCM_PERSONALIZED;
    memcpy(request_id + 1, tag, TESLA_KEY_TAG_LEN);

    for (int attempt = 0; attempt < 2; attempt++) {
        rx_reset();
        if (send_frame(packet, packet_len) != 0) {
            return TESLA_ERR_TX;
        }
        if (!wait_message(RX_TIMEOUT_MS)) {
            continue;
        }
        tesla_routable_t message;
        if (!tesla_proto_parse_routable(s_frame, s_frame_len, &message)) {
            return TESLA_ERR_PROTO;
        }
        if (message.has_fault && message.fault != 0) {
            if (message.fault == 5 || message.fault == 6 || message.fault == 15) {
                return TESLA_ERR_SESSION_STALE;
            }
            return TESLA_ERR_VEHICLE;
        }
        if (!message.has_response || !message.has_payload) {
            continue;
        }
        if (tesla_crypto_decrypt_response(&s_inf_session, &message, request_id, plain, plain_cap,
                                          plain_len) != ESP_OK) {
            return TESLA_ERR_CRYPTO;
        }
        return TESLA_OK;
    }
    return TESLA_ERR_TIMEOUT;
}

int tesla_ble_read_charge(int *percent)
{
    if (percent == NULL) {
        return TESLA_ERR_PROTO;
    }
    if (!s_conn_active || !s_ready) {
        return TESLA_ERR_CONNECT;
    }
    int64_t now = esp_timer_get_time();
    if (!s_inf_session.ready || s_inf_handshake_us == 0 ||
        now - s_inf_handshake_us > 120LL * 1000 * 1000) {
        int rc = handshake_domain(TESLA_DOMAIN_INFOTAINMENT, &s_inf_session, &s_inf_handshake_us,
                                  true);
        if (rc != TESLA_OK) {
            return rc;
        }
    }

    uint8_t request[32];
    size_t request_len = tesla_proto_charge_request(request, sizeof(request));
    if (request_len == 0) {
        return TESLA_ERR_PROTO;
    }
    uint8_t plain[512];
    size_t plain_len = 0;
    int rc = send_inf_request(request, request_len, plain, sizeof(plain), &plain_len);
    if (rc != TESLA_OK) {
        if (rc == TESLA_ERR_SESSION_STALE || rc == TESLA_ERR_CRYPTO) {
            memset(&s_inf_session, 0, sizeof(s_inf_session));
        }
        return rc;
    }
    int value = -1;
    if (!tesla_proto_parse_charge_state(plain, plain_len, &value)) {
        return TESLA_ERR_PROTO;
    }
    s_charge_percent = value;
    *percent = value;
    return TESLA_OK;
}

int tesla_ble_get_charge(void)
{
    return s_charge_percent;
}

// 寻车：信息娱乐域先闪灯、再鸣笛。闪灯成功即视为寻车成功，鸣笛失败只告警。
static int find_car(void)
{
    int err = ensure_connected();
    if (err != TESLA_OK) {
        return err;
    }
    if (!s_ready || !s_authorized) {
        err = do_handshake();
        if (err == TESLA_ERR_NOT_AUTHORIZED) {
            s_authorized = false;
            return err;
        }
        if (err != TESLA_OK) {
            disconnect_locked();
            return err;
        }
    }

    const int64_t now = esp_timer_get_time();
    if (!s_inf_session.ready || s_inf_handshake_us == 0 ||
        now - s_inf_handshake_us > 120LL * 1000 * 1000) {
        err = handshake_domain(TESLA_DOMAIN_INFOTAINMENT, &s_inf_session, &s_inf_handshake_us,
                               true);
        if (err != TESLA_OK) {
            return err;
        }
    }

    uint8_t request[16];
    uint8_t plain[128];
    size_t plain_len = 0;
    size_t len = tesla_proto_flash_lights(request, sizeof(request));
    if (len == 0) {
        return TESLA_ERR_PROTO;
    }
    err = send_inf_request(request, len, plain, sizeof(plain), &plain_len);
    if (err != TESLA_OK) {
        if (err == TESLA_ERR_SESSION_STALE || err == TESLA_ERR_CRYPTO) {
            memset(&s_inf_session, 0, sizeof(s_inf_session));
        }
        return err;
    }

    len = tesla_proto_honk_horn(request, sizeof(request));
    if (len > 0) {
        int horn = send_inf_request(request, len, plain, sizeof(plain), &plain_len);
        if (horn != TESLA_OK) {
            ESP_LOGW(TAG, "寻车鸣笛失败: %d", horn);
        }
    }
    return TESLA_OK;
}

// --------------------------------------------------------------------------
// 公开接口
// --------------------------------------------------------------------------
esp_err_t tesla_ble_init(tesla_ble_event_cb_t cb)
{
    if (s_initialized) {
        s_event_cb = cb;
        return ESP_OK;
    }
    s_event_cb = cb;

    s_sync_sem = xSemaphoreCreateBinary();
    s_link_sem = xSemaphoreCreateBinary();
    s_mtu_sem = xSemaphoreCreateBinary();
    s_setup_sem = xSemaphoreCreateBinary();
    s_rx_sem = xSemaphoreCreateBinary();
    s_msg_mutex = xSemaphoreCreateMutex();
    s_disc_mutex = xSemaphoreCreateMutex();
    if (!s_sync_sem || !s_link_sem || !s_mtu_sem || !s_setup_sem || !s_rx_sem || !s_msg_mutex ||
        !s_disc_mutex) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    if (xTaskCreatePinnedToCore(host_task, "tesla_ble_host", NIMBLE_HS_STACK_SIZE, NULL,
                                configMAX_PRIORITIES - 4, NULL, NIMBLE_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "等待 NimBLE 同步超时");
        return ESP_ERR_TIMEOUT;
    }

    uint8_t public_key[TESLA_PUBKEY_LEN];
    err = tesla_crypto_key_ensure(public_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "静态密钥初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;
    set_state(TESLA_BLE_IDLE, TESLA_OK);
    return ESP_OK;
}

void tesla_ble_set_vin(const char *vin)
{
    memset(&s_vehicle, 0, sizeof(s_vehicle));
    if (vin == NULL) {
        s_vin[0] = '\0';
        s_target_name[0] = '\0';
        return;
    }
    strncpy((char *)s_vin, vin, sizeof(s_vin) - 1);
    s_vin[sizeof(s_vin) - 1] = '\0';
    if (tesla_ble_name_from_vin((const char *)s_vin, s_target_name) != ESP_OK) {
        s_target_name[0] = '\0';
    }
    s_authorized = false;
    s_ready = false;
}

void tesla_ble_get_status(tesla_ble_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->connected = s_conn_active;
    out->authorized = s_authorized;
    out->state = (uint8_t)s_state;
    out->rssi = s_rssi;
    out->last_error = s_last_error;
    out->detail = s_detail;
    out->charge = s_charge_percent;
    memcpy(out->vehicle_name, s_target_name, TESLA_NAME_LEN);
    out->vehicle_name[TESLA_NAME_LEN] = '\0';
}

void tesla_ble_get_vehicle_status(tesla_vehicle_status_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_vehicle;
}

void tesla_ble_fingerprint(char out[9])
{
    uint8_t public_key[TESLA_PUBKEY_LEN];
    if (tesla_crypto_key_ensure(public_key) == ESP_OK) {
        tesla_crypto_fingerprint(public_key, out);
    } else {
        strcpy(out, "????????");
    }
}

int tesla_ble_execute(tesla_cmd_t cmd, uint32_t *lock_state)
{
    if (!s_initialized) {
        return TESLA_ERR_INIT;
    }
    if (cmd == TESLA_CMD_FIND) {
        return find_car();
    }
    uint8_t request[256];
    size_t len = 0;
    switch (cmd) {
    case TESLA_CMD_UNLOCK:
        len = tesla_proto_rke(TESLA_RKE_UNLOCK, request, sizeof(request));
        break;
    case TESLA_CMD_LOCK:
        len = tesla_proto_rke(TESLA_RKE_LOCK, request, sizeof(request));
        break;
    case TESLA_CMD_FRUNK:
        len = tesla_proto_open_frunk(request, sizeof(request));
        break;
    case TESLA_CMD_TRUNK:
        len = tesla_proto_open_trunk(request, sizeof(request));
        break;
    case TESLA_CMD_TRUNK_CLOSE:
        len = tesla_proto_close_trunk(request, sizeof(request));
        break;
    case TESLA_CMD_STATUS:
        len = tesla_proto_info_status(request, sizeof(request));
        break;
    default:
        return TESLA_ERR_PROTO;
    }
    if (len == 0) {
        return TESLA_ERR_PROTO;
    }

    s_detail = 0;
    int last_err = TESLA_ERR_TIMEOUT;
    for (int attempt = 0; attempt < 2; attempt++) {
        int err = ensure_connected();
        if (err != TESLA_OK) {
            return err;
        }
        const int64_t now = esp_timer_get_time();
        const bool reuse = attempt == 0 && s_ready && s_session.ready && s_authorized &&
                           s_handshake_us > 0 &&
                           now - s_handshake_us < 120LL * 1000 * 1000;
        if (!reuse) {
            err = do_handshake();
            if (err == TESLA_ERR_NOT_AUTHORIZED) {
                s_authorized = false;
                return err;
            }
            if (err != TESLA_OK) {
                last_err = err;
                disconnect_locked();
                continue;
            }
        }
        err = send_command(request, len, lock_state);
        if (err == TESLA_OK || err == TESLA_ERR_VEHICLE || err == TESLA_ERR_NOT_AUTHORIZED ||
            err == TESLA_ERR_BUSY) {
            return err;
        }
        last_err = err;
        disconnect_locked();
    }
    if (last_err == TESLA_ERR_SESSION_STALE) {
        return TESLA_ERR_VEHICLE;
    }
    return last_err;
}

static int pair_vehicle(void);

int tesla_ble_pair(void)
{
    if (!s_initialized) {
        return TESLA_ERR_INIT;
    }
    s_pair_peer_valid = false;
    return pair_vehicle();
}

int tesla_ble_pair_peer(const uint8_t addr[6], uint8_t addr_type)
{
    if (!s_initialized) {
        return TESLA_ERR_INIT;
    }
    if (addr == NULL) {
        return TESLA_ERR_CONNECT;
    }
    tesla_ble_discover_stop();
    // 选中的广播如果当前不可连接（车辆蓝牙连接数已满时会转为不可连接广播），
    // 直接按连接失败处理，避免无谓的 GATT 初始化。
    if (s_disc_mutex != NULL && xSemaphoreTake(s_disc_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < TESLA_BLE_DISCOVERY_MAX; i++) {
            if (s_disc[i].used && s_disc[i].addr_type == addr_type &&
                memcmp(s_disc[i].addr, addr, sizeof(s_disc[i].addr)) == 0) {
                if (!s_disc[i].connectable) {
                    xSemaphoreGive(s_disc_mutex);
                    ESP_LOGW(TAG, "所选广播当前不可连接");
                    return TESLA_ERR_CONNECT;
                }
                break;
            }
        }
        xSemaphoreGive(s_disc_mutex);
    }
    memset(&s_pair_peer, 0, sizeof(s_pair_peer));
    s_pair_peer.type = addr_type;
    memcpy(s_pair_peer.val, addr, sizeof(s_pair_peer.val));
    s_pair_peer_valid = true;
    int err = pair_vehicle();
    s_pair_peer_valid = false;
    return err;
}

static int pair_vehicle(void)
{
    if (!s_initialized) {
        return TESLA_ERR_INIT;
    }
    int err = ensure_connected();
    if (err != TESLA_OK) {
        return err;
    }

    s_detail = 0;
    // 已授权的钥匙无需再次添加；握手成功说明车辆已认可本机。
    int precheck = do_handshake();
    if (precheck == TESLA_OK) {
        set_state(TESLA_BLE_READY, TESLA_OK);
        emit(TESLA_EVT_PAIR_PROGRESS, TESLA_OK, 100);
        return TESLA_OK;
    }

    uint8_t public_key[TESLA_PUBKEY_LEN];
    if (tesla_crypto_key_ensure(public_key) != ESP_OK) {
        return TESLA_ERR_CRYPTO;
    }
    uint8_t unsigned_msg[192];
    uint8_t envelope[256];
    size_t len = tesla_proto_whitelist_add_key(public_key, TESLA_KEY_FORM_ANDROID, unsigned_msg,
                                               sizeof(unsigned_msg));
    if (len == 0) {
        return TESLA_ERR_PROTO;
    }
    size_t envelope_len = tesla_proto_vcsec_envelope(unsigned_msg, len, TESLA_VCSEC_SIG_PRESENT_KEY,
                                                     envelope, sizeof(envelope));
    if (envelope_len == 0) {
        return TESLA_ERR_PROTO;
    }

    set_state(TESLA_BLE_PAIRING, TESLA_OK);
    emit(TESLA_EVT_PAIR_PROGRESS, TESLA_OK, 5);
    s_detail = 0;
    rx_reset();
    if (send_frame(envelope, envelope_len) != 0) {
        set_state(TESLA_BLE_IDLE, TESLA_ERR_TX);
        return TESLA_ERR_TX;
    }

    // 发送加钥匙请求后不再盲等：先解析车辆应答；
    // 车辆回 WAIT 后进入人工确认窗口，窗口内不重发，只做握手探测兜底。
    const int64_t start_us = esp_timer_get_time();
    int64_t last_tx_us = start_us;
    int64_t last_probe_us = start_us;
    int sends = 1;
    bool waiting = false;
    while (true) {
        const int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
        if (elapsed_ms >= PAIR_WAIT_WINDOW_MS) {
            break;
        }
        uint8_t progress = (uint8_t)(10 + elapsed_ms * 85 / PAIR_WAIT_WINDOW_MS);
        if (progress > 95) {
            progress = 95;
        }
        emit(TESLA_EVT_PAIR_PROGRESS, TESLA_OK, progress);

        if (wait_message(PAIR_RX_MS)) {
            const int reply = parse_pair_reply(s_frame, s_frame_len);
            if (reply == TESLA_OK) {
                (void)do_handshake();
                s_detail = 0;
                emit(TESLA_EVT_PAIR_PROGRESS, TESLA_OK, 100);
                set_state(TESLA_BLE_READY, TESLA_OK);
                return TESLA_OK;
            }
            if (reply == TESLA_ERR_VEHICLE) {
                set_state(TESLA_BLE_IDLE, TESLA_ERR_VEHICLE);
                return TESLA_ERR_VEHICLE;
            }
            if (reply == TESLA_ERR_BUSY) {
                waiting = true;
                continue;
            }
        }

        if (!s_conn_active) {
            if (ensure_connected() != TESLA_OK) {
                continue;
            }
            last_probe_us = esp_timer_get_time();
            continue;
        }

        const int64_t now_us = esp_timer_get_time();
        if (!waiting && sends < PAIR_MAX_SENDS &&
            now_us - last_tx_us >= (int64_t)PAIR_RESEND_MS * 1000) {
            rx_reset();
            if (send_frame(envelope, envelope_len) == 0) {
                sends++;
                last_tx_us = now_us;
                continue;
            }
        }
        if (now_us - last_probe_us >= (int64_t)PAIR_PROBE_MS * 1000) {
            last_probe_us = now_us;
            const uint16_t saved_detail = s_detail;
            const int probe = do_handshake_impl(true);
            if (probe == TESLA_OK) {
                s_detail = 0;
                emit(TESLA_EVT_PAIR_PROGRESS, TESLA_OK, 100);
                return TESLA_OK;
            }
            // 探测只用于发现“已入白名单”，失败原因仍以车辆应答为准。
            s_detail = saved_detail;
        }
    }
    set_state(TESLA_BLE_IDLE, TESLA_ERR_TIMEOUT);
    return TESLA_ERR_TIMEOUT;
}
