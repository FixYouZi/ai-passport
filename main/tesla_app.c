// main/tesla_app.c
// 卡片钥匙应用界面（240x320）：主页动作、执行进度、结果、设置、车辆信息、
// 蓝牙钥匙授权、热点配置、关于。三键：上/下选择，OK 执行，长按 OK 返回。
#include "tesla_app.h"

#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "car_art.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "tesla_ble.h"
#include "tesla_config.h"
#include "tesla_web.h"

static const char *TAG = "tesla_app";

LV_FONT_DECLARE(tesla_font_16);
LV_FONT_DECLARE(tesla_font_20);
LV_FONT_DECLARE(tesla_font_26);
// 主页大字号锁车状态（36 px）子集字体。
LV_FONT_DECLARE(tesla_font_lock);
// 广播列表的信号强度只用 ASCII 数字，复用内置 Montserrat 14。
LV_FONT_DECLARE(lv_font_montserrat_14);

#define C_BG 0x0F1115
#define C_HEADER 0x15181E
#define C_CARD_TOP 0x1D222B
#define C_CARD_BOT 0x171B22
#define C_CARD_SEL_TOP 0x2C3440
#define C_CARD_SEL_BOT 0x232A34
#define C_STROKE 0x2B323C
#define C_BORDER 0x3A424F
#define C_LINE 0x242A33
#define C_TEXT 0xE9EDF2
#define C_MUTED 0x8B95A5
#define C_DIM 0x6F7A8A
#define C_RED 0xE82127
#define C_GREEN 0x34C759
#define C_AMBER 0xF5A524

#define SCREEN_W 240
#define SCREEN_H 320

// 无操作 3 分钟熄屏；离开车辆（蓝牙断开）且无操作 10 分钟进入深睡。
#define SCREEN_OFF_DELAY_US (3LL * 60 * 1000 * 1000)
#define DEEP_SLEEP_DELAY_US (10LL * 60 * 1000 * 1000)
#define SCREEN_BRIGHTNESS 90

typedef enum {
    PAGE_MAIN = 0,
    PAGE_SETTINGS,
    PAGE_INFO,
    PAGE_PAIR,
    PAGE_HOTSPOT,
    PAGE_ABOUT,
    PAGE_RUNNING,
    PAGE_RESULT,
    PAGE_CONFIRM,
} page_t;

enum {
    EVT_KEY = 0,
    EVT_BLE,
    EVT_RESULT,
    EVT_CONFIG,
};

typedef struct {
    uint8_t kind;
    uint8_t action;
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
    tesla_ble_event_t ble;
    int error;
    uint32_t lock;
    bool has_lock;
} ui_event_t;

typedef struct {
    uint8_t action;
    uint8_t addr_type;
    uint8_t addr[6];
} app_action_t;

enum {
    ACT_UNLOCK = 0,
    ACT_LOCK,
    ACT_FRUNK,
    ACT_TRUNK,
    ACT_TRUNK_CLOSE,
    ACT_PAIR,
    ACT_STATUS,
};

static const char *SETTINGS_TITLES[6] = {"车辆信息", "蓝牙钥匙授权", "热点配置", "关于本机",
                                         "清空车辆信息", "重置蓝牙密钥"};
static const char *SETTINGS_GLYPHS[6] = {"车", "钥", "热", "关", "清", "重"};
static const char *RUNNING_TITLES[7] = {"解锁车辆", "上锁车辆", "打开前备箱", "打开后备箱",
                                        "关闭后备箱", "蓝牙钥匙授权", "读取车辆状态"};

static lv_obj_t *s_root;
static page_t s_page;
static int s_selected;
static bool s_dirty;
static bool s_busy;
static uint8_t s_action;
static int s_battery = -1;
static int s_battery_tick;
static bool s_web_on;
static uint8_t s_pair_progress;
static tesla_ble_peer_t s_peers[TESLA_BLE_DISCOVERY_MAX];
static int s_peer_count;
static int s_pair_cursor;
static bool s_pair_scanning;
static int64_t s_pair_refresh_us;
static char s_result_title[32];
static char s_result_detail[192];
static bool s_result_ok;
static tesla_ble_status_t s_ble;
static tesla_config_t s_config;
static volatile bool s_config_dirty;
static char s_ap_ssid[33];
static char s_ap_pass[33];
static char s_ap_ip[16];

static volatile bool s_screen_on = true;
static volatile bool s_wake_request;
static volatile bool s_wake_ignore;   // 点亮屏幕的那次按键在松手前不再触发动作
static volatile bool s_sleep_armed;
static int64_t s_last_activity;
static int64_t s_away_since;
static int64_t s_sleep_retry;
static bool s_was_connected;

enum {
    CONFIRM_CLEAR_VIN = 0,
    CONFIRM_RESET_KEY,
};

static uint8_t s_confirm_action;
static uint8_t s_confirm_choice; // 0 = 取消，1 = 确定

static QueueHandle_t s_ui_queue;
static QueueHandle_t s_action_queue;

// --------------------------------------------------------------------------
// 基础绘制
// --------------------------------------------------------------------------
static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int width, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static const char *battery_text(char out[16])
{
    if (s_battery < 0) {
        strcpy(out, "--");
    } else {
        snprintf(out, 16, "%d%%", s_battery);
    }
    return out;
}

static const char *ble_state_text(void)
{
    if (!s_ble.initialized) {
        return "蓝牙未初始化";
    }
    switch (s_ble.state) {
    case TESLA_BLE_SCANNING: return "正在搜索车辆";
    case TESLA_BLE_CONNECTING: return "正在连接车辆";
    case TESLA_BLE_HANDSHAKE: return "正在建立安全会话";
    case TESLA_BLE_PAIRING: return "正在等待车辆授权";
    case TESLA_BLE_READY: return "正在发送命令";
    default: return s_busy ? "正在准备" : "准备就绪";
    }
}

static const char *link_short_text(void)
{
    if (!s_ble.initialized) {
        return "未就绪";
    }
    if (s_busy) {
        switch (s_ble.state) {
        case TESLA_BLE_SCANNING: return "搜索中";
        case TESLA_BLE_CONNECTING: return "连接中";
        case TESLA_BLE_HANDSHAKE: return "握手中";
        case TESLA_BLE_READY: return "发送中";
        case TESLA_BLE_PAIRING: return "授权中";
        default: return "准备中";
        }
    }
    if (s_config.vin[0] == '\0') {
        return "待配置";
    }
    if (s_ble.connected && s_ble.authorized) {
        return "已授权";
    }
    if (s_ble.connected) {
        return "未授权";
    }
    return "待命";
}

static void build_header(const char *title)
{
    make_box(s_root, 0, 0, SCREEN_W, 54, C_HEADER, 0);
    make_label(s_root, 14, 14, 150, title, &tesla_font_20, C_TEXT);

    uint32_t dot = C_DIM;
    if (s_ble.connected && s_ble.authorized) {
        dot = C_GREEN;
    } else if (s_ble.connected) {
        dot = C_AMBER;
    }
    make_box(s_root, 170, 22, 12, 12, C_STROKE, LV_RADIUS_CIRCLE);
    make_box(s_root, 172, 24, 8, 8, dot, LV_RADIUS_CIRCLE);

    uint32_t battery_color = C_MUTED;
    if (s_battery >= 0 && s_battery <= 15) {
        battery_color = C_RED;
    } else if (s_battery >= 0 && s_battery <= 30) {
        battery_color = C_AMBER;
    }
    char battery[16];
    lv_obj_t *label =
        make_label(s_root, 186, 16, 40, battery_text(battery), &tesla_font_16, battery_color);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);

    make_box(s_root, 0, 53, SCREEN_W, 1, C_LINE, 0);
}

static void build_row(int y, int h, const char *glyph, const char *title, bool selected,
                      const char *right, const lv_font_t *font, bool danger)
{
    lv_obj_t *card = make_box(s_root, 12, y, 216, h, selected ? C_CARD_SEL_TOP : C_CARD_TOP, 12);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(selected ? C_CARD_SEL_BOT : C_CARD_BOT), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    if (selected) {
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(C_BORDER), 0);
        make_box(card, 0, (h - 20) / 2, 4, 20, C_RED, 2);
    }
    int icon = h - 12;
    lv_obj_t *icon_box = make_box(card, 12, 6, icon, icon, selected ? 0x333B47 : C_STROKE, 8);
    lv_obj_t *glyph_label =
        make_label(icon_box, 0, 0, icon, glyph, font, danger ? C_AMBER : C_TEXT);
    lv_obj_set_style_text_align(glyph_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(glyph_label, LV_ALIGN_CENTER, 0, 0);

    int text_y = (h - (font == &tesla_font_20 ? 27 : 22)) / 2;
    make_label(card, 50, text_y, 118, title, font, selected ? C_TEXT : 0xC9D1DC);
    if (right != NULL) {
        lv_obj_t *tail = make_label(card, 178, (h - 22) / 2, 24, right, &tesla_font_16, C_DIM);
        lv_obj_set_style_text_align(tail, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void build_footer(const char *hint)
{
    make_box(s_root, 12, 290, 216, 1, C_LINE, 0);
    lv_obj_t *label = make_label(s_root, 0, 296, SCREEN_W, hint, &tesla_font_16, C_DIM);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static void screen_reset(void)
{
    lv_obj_clean(s_root);
}

// --------------------------------------------------------------------------
// 页面
// --------------------------------------------------------------------------
static const char *lock_state_text(uint8_t state)
{
    switch (state) {
    case 0: return "已解锁";
    case 1: return "已锁车";
    case 2: return "内锁";
    case 3: return "未全锁";
    default: return "锁车未知";
    }
}

// 主界面状态行：16 px 下单行宽度不超过 208 px，最长文案为
// “前备箱开 · 信号 -100 dBm”，无需换行。
static void vehicle_status_line(char *out, size_t cap, uint32_t *color)
{
    tesla_vehicle_status_t vehicle;
    tesla_ble_get_vehicle_status(&vehicle);
    if (!vehicle.valid) {
        snprintf(out, cap, "按 OK 读取状态");
        *color = C_MUTED;
        return;
    }

    static const char *const OPEN_TEXTS[3] = {"车门开", "尾门开", "前备箱开"};
    const bool open[3] = {vehicle.doors_open, vehicle.rear_trunk_open, vehicle.front_trunk_open};
    int first = -1;
    int opens = 0;
    for (int i = 0; i < 3; i++) {
        if (open[i]) {
            if (first < 0) {
                first = i;
            }
            opens++;
        }
    }

    if (opens == 0) {
        snprintf(out, cap, "门箱全关");
        *color = C_MUTED;
    } else if (opens == 1) {
        snprintf(out, cap, "%s", OPEN_TEXTS[first]);
        *color = C_AMBER;
    } else {
        snprintf(out, cap, "%s等", OPEN_TEXTS[first]);
        *color = C_AMBER;
    }
}

static void build_main(void)
{
    // 方案 C · 极简大字：不用卡片边框，只靠字号与留白建立层级。
    uint32_t link_color = C_DIM;
    if (s_ble.connected && s_ble.authorized) {
        link_color = C_GREEN;
    } else if (s_ble.connected || s_busy) {
        link_color = C_AMBER;
    }
    make_box(s_root, 16, 20, 6, 6, link_color, LV_RADIUS_CIRCLE);

    char line[96];
    if (s_config.vin[0] == '\0') {
        snprintf(line, sizeof(line), "未录入车辆 · 待配置");
    } else {
        snprintf(line, sizeof(line), "%s · %s", s_config.model[0] ? s_config.model : "特斯拉",
                 link_short_text());
    }
    make_label(s_root, 26, 10, 148, line, &tesla_font_16, C_MUTED);

    uint32_t battery_color = C_MUTED;
    if (s_battery >= 0 && s_battery <= 15) {
        battery_color = C_RED;
    } else if (s_battery >= 0 && s_battery <= 30) {
        battery_color = C_AMBER;
    }
    make_box(s_root, 176, 20, 6, 6, battery_color, LV_RADIUS_CIRCLE);
    char battery[16];
    lv_obj_t *batt =
        make_label(s_root, 186, 10, 38, battery_text(battery), &tesla_font_16, battery_color);
    lv_obj_set_style_text_align(batt, LV_TEXT_ALIGN_RIGHT, 0);

    tesla_vehicle_status_t vehicle;
    tesla_ble_get_vehicle_status(&vehicle);
    const char *big = "状态未知";
    uint32_t big_color = C_MUTED;
    if (vehicle.valid) {
        big = lock_state_text(vehicle.lock_state);
        big_color = vehicle.lock_state == 1 ? C_GREEN : C_AMBER;
    }
    make_label(s_root, 16, 52, 208, big, &tesla_font_lock, big_color);

    uint32_t status_color;
    if (s_config.vin[0] == '\0') {
        snprintf(line, sizeof(line), "长按 OK 进入设置");
        status_color = C_MUTED;
    } else {
        vehicle_status_line(line, sizeof(line), &status_color);
        if (vehicle.valid && s_ble.connected && s_ble.rssi != 0) {
            snprintf(line + strlen(line), sizeof(line) - strlen(line), " · 信号 %d dBm",
                     s_ble.rssi);
        }
    }
    make_label(s_root, 16, 100, 208, line, &tesla_font_16, status_color);

    // 车型图：整幅深色横带，无圆角。
    make_box(s_root, 0, 132, SCREEN_W, 108, 0x14181F, 0);
    const lv_image_dsc_t *art =
        strstr(s_config.model, "Y") != NULL ? &car_model_y : &car_model_3;
    lv_obj_t *car = lv_image_create(s_root);
    lv_image_set_src(car, art);
    lv_obj_set_pos(car, 12, 136);
    lv_obj_remove_flag(car, LV_OBJ_FLAG_SCROLLABLE);

    make_box(s_root, 16, 252, 36, 3, C_RED, 2);
    lv_obj_t *hint = make_label(s_root, 0, 266, SCREEN_W, "上键 单击锁车 · 双击解锁",
                                &tesla_font_16, 0xC9D1DC);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    hint = make_label(s_root, 0, 288, SCREEN_W, "下键 单击尾门 · 双击关", &tesla_font_16,
                      0xC9D1DC);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_tile(int x, int y, const char *glyph, const char *title, bool selected)
{
    lv_obj_t *tile = make_box(s_root, x, y, 104, 68, selected ? C_CARD_SEL_TOP : C_CARD_TOP, 12);
    lv_obj_set_style_bg_grad_color(tile, lv_color_hex(selected ? C_CARD_SEL_BOT : C_CARD_BOT), 0);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, 0);
    if (selected) {
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(C_BORDER), 0);
        make_box(tile, 0, 22, 3, 24, C_RED, 2);
    }
    lv_obj_t *icon = make_box(tile, 37, 7, 30, 30, selected ? 0x333B47 : C_STROKE, 9);
    lv_obj_t *mark = make_label(icon, 0, 0, 30, glyph, &tesla_font_20, C_TEXT);
    lv_obj_set_style_text_align(mark, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mark, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *label = make_label(tile, 0, 43, 104, title, &tesla_font_16,
                                 selected ? C_TEXT : 0xC9D1DC);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_settings(void)
{
    build_header("设置");
    for (int i = 0; i < 4; i++) {
        int x = (i % 2 == 0) ? 12 : 124;
        int y = 58 + (i / 2) * 76;
        build_tile(x, y, SETTINGS_GLYPHS[i], SETTINGS_TITLES[i], i == s_selected);
    }
    build_row(208, 38, SETTINGS_GLYPHS[4], SETTINGS_TITLES[4], s_selected == 4, NULL,
              &tesla_font_16, true);
    build_row(250, 38, SETTINGS_GLYPHS[5], SETTINGS_TITLES[5], s_selected == 5, NULL,
              &tesla_font_16, true);
    build_footer("上/下 选择   OK 进入   长按 返回");
}

static void info_line(int y, const char *key, const char *value)
{
    make_label(s_root, 16, y, 82, key, &tesla_font_16, C_MUTED);
    lv_obj_t *label = make_label(s_root, 100, y, 124, value, &tesla_font_16, C_TEXT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    make_box(s_root, 16, y + 30, 208, 1, C_LINE, 0);
}

static void build_info(void)
{
    build_header("车辆信息");
    char fingerprint[9];
    tesla_ble_fingerprint(fingerprint);

    make_label(s_root, 16, 62, 208, "VIN", &tesla_font_16, C_MUTED);
    make_label(s_root, 16, 82, 208, s_config.vin[0] ? s_config.vin : "未配置", &tesla_font_16,
               C_TEXT);
    make_box(s_root, 16, 104, 208, 1, C_LINE, 0);

    info_line(114, "车牌号", s_config.plate[0] ? s_config.plate : "未配置");
    info_line(152, "车型", s_config.model[0] ? s_config.model : "未配置");

    make_label(s_root, 16, 190, 208, "BLE 名称", &tesla_font_16, C_MUTED);
    make_label(s_root, 16, 210, 208, s_ble.vehicle_name[0] ? s_ble.vehicle_name : "未生成",
               &tesla_font_16, C_TEXT);
    make_box(s_root, 16, 232, 208, 1, C_LINE, 0);

    info_line(242, "公钥指纹", fingerprint);
    build_footer("长按 OK 返回");
}

// 授权页一行：左侧绿点标记与 VIN 匹配的目标车辆，右侧显示信号强度。
static void build_peer_row(int y, int h, const tesla_ble_peer_t *peer, bool selected)
{
    lv_obj_t *card = make_box(s_root, 12, y, 216, h, selected ? C_CARD_SEL_TOP : C_CARD_TOP, 10);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(selected ? C_CARD_SEL_BOT : C_CARD_BOT), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    if (selected) {
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(C_BORDER), 0);
        make_box(card, 0, (h - 18) / 2, 4, 18, C_RED, 2);
    }
    int name_x = 16;
    if (peer->target) {
        make_box(card, 10, (h - 6) / 2, 6, 6, C_GREEN, LV_RADIUS_CIRCLE);
        name_x = 22;
    }
    lv_obj_t *name = make_label(card, name_x, (h - 22) / 2, 140 - name_x,
                                peer->name[0] ? peer->name : "未知设备", &tesla_font_16,
                                peer->target ? C_GREEN : (selected ? C_TEXT : 0xC9D1DC));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

    char rssi[16];
    snprintf(rssi, sizeof(rssi), "%d dBm", peer->rssi);
    lv_obj_t *tail = make_label(card, 148, (h - 18) / 2, 56, rssi, &lv_font_montserrat_14,
                                peer->target ? C_GREEN : C_MUTED);
    lv_obj_set_style_text_align(tail, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_pair(void)
{
    build_header("蓝牙钥匙授权");
    if (s_peer_count <= 0) {
        lv_obj_t *label = make_label(s_root, 16, 118, 208,
                                     s_pair_scanning ? "正在搜索车辆" : "未发现设备",
                                     &tesla_font_20, C_MUTED);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        build_footer("长按 OK 返回");
        return;
    }
    for (int i = 0; i < s_peer_count; i++) {
        build_peer_row(58 + i * 36, 34, &s_peers[i], i == s_pair_cursor);
    }
    build_footer("上/下 选择   OK 确认");
}

// 每秒从 BLE 层取一次最新广播列表（名称、信号强度会持续变化）。
static void pair_refresh(void)
{
    s_peer_count = tesla_ble_discover_list(s_peers, TESLA_BLE_DISCOVERY_MAX);
    if (s_pair_cursor >= s_peer_count) {
        s_pair_cursor = s_peer_count > 0 ? s_peer_count - 1 : 0;
    }
    s_dirty = true;
}

static void build_confirm(void)
{
    build_header("确认操作");
    make_label(s_root, 16, 66, 208,
               s_confirm_action == CONFIRM_CLEAR_VIN
                   ? "将清空 VIN、车牌号和车型，需要重新配置后才能使用。"
                   : "将删除本机蓝牙密钥，需要重新完成钥匙授权。",
               &tesla_font_16, C_TEXT);
    build_row(150, 44, "取", "取消", s_confirm_choice == 0, NULL, &tesla_font_20, false);
    build_row(202, 44, "确", "确定", s_confirm_choice == 1, NULL, &tesla_font_20, false);
    build_footer("上/下 选择   OK 确认   长按 返回");
}

static void build_hotspot(void)
{
    build_header("热点配置");
    tesla_web_get_ap(s_ap_ssid, s_ap_pass, s_ap_ip);
    info_line(64, "热点名称", s_ap_ssid[0] ? s_ap_ssid : "未启动");
    info_line(102, "密码", s_ap_pass[0] ? s_ap_pass : "未启动");
    info_line(140, "配置地址", s_ap_ip);
    make_label(s_root, 16, 182, 208,
               "手机连接热点后，用浏览器打开配置地址，填写 VIN、车牌号和车型。",
               &tesla_font_16, C_MUTED);

    lv_obj_t *pill = make_box(s_root, 16, 256, 132, 26, s_web_on ? 0x12351F : 0x222831, 13);
    make_box(pill, 10, 9, 8, 8, s_web_on ? C_GREEN : C_DIM, LV_RADIUS_CIRCLE);
    make_label(pill, 26, 3, 96, s_web_on ? "热点已开启" : "热点已关闭", &tesla_font_16,
               s_web_on ? C_GREEN : C_DIM);
    build_footer("OK 开关热点   长按 返回");
}

static void build_about(void)
{
    build_header("关于本机");
    make_label(s_root, 16, 70, 208, "特斯拉卡片钥匙", &tesla_font_26, C_TEXT);
    make_label(s_root, 16, 112, 208, "版本 1.0.0", &tesla_font_16, C_MUTED);
    make_label(s_root, 16, 140, 208, "BLE · Tesla VCSEC", &tesla_font_16, C_MUTED);
    make_label(s_root, 16, 168, 208, "ESP32-C3 · LVGL", &tesla_font_16, C_MUTED);
    make_label(s_root, 16, 208, 208,
               "非官方项目，仅用于自有车辆。请妥善保管设备与密钥。", &tesla_font_16, C_DIM);
    build_footer("长按 OK 返回");
}

static const char *running_hint(void)
{
    switch (s_ble.state) {
    case TESLA_BLE_SCANNING: return "请靠近车辆，确认车辆蓝牙已唤醒";
    case TESLA_BLE_CONNECTING: return "正在连接车辆，请保持靠近";
    case TESLA_BLE_HANDSHAKE: return "正在建立加密会话";
    case TESLA_BLE_READY: return "正在发送命令";
    case TESLA_BLE_PAIRING: return "请在车辆屏幕确认并刷卡";
    default: return "请保持设备靠近车辆";
    }
}

static int running_progress(void)
{
    if (s_action == ACT_PAIR && s_ble.state == TESLA_BLE_PAIRING) {
        return s_pair_progress > 0 ? s_pair_progress : 5;
    }
    switch (s_ble.state) {
    case TESLA_BLE_SCANNING: return 15;
    case TESLA_BLE_CONNECTING: return 35;
    case TESLA_BLE_HANDSHAKE: return 60;
    case TESLA_BLE_READY: return 80;
    case TESLA_BLE_PAIRING: return s_pair_progress;
    default: return 5;
    }
}

static void build_running(void)
{
    build_header(RUNNING_TITLES[s_action < 7 ? s_action : 0]);
    lv_obj_t *label = make_label(s_root, 16, 100, 208, ble_state_text(), &tesla_font_20, C_TEXT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    int value = running_progress();
    lv_obj_t *bar = lv_bar_create(s_root);
    lv_obj_set_pos(bar, 30, 152);
    lv_obj_set_size(bar, 180, 6);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_STROKE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_RED), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", value);
    label = make_label(s_root, 0, 172, SCREEN_W, percent, &tesla_font_16, C_MUTED);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *hint = make_label(s_root, 16, 204, 208, running_hint(), &tesla_font_16, C_MUTED);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    build_footer("操作进行中，请稍候");
}

static void build_result(void)
{
    build_header("执行结果");
    lv_obj_t *circle = make_box(s_root, 82, 76, 76, 76, s_result_ok ? 0x13351F : 0x3A1517,
                                LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(circle, 2, 0);
    lv_obj_set_style_border_color(circle, lv_color_hex(s_result_ok ? C_GREEN : C_RED), 0);
    lv_obj_t *mark = make_label(circle, 0, 24, 76, s_result_ok ? "成功" : "失败", &tesla_font_20,
                                s_result_ok ? C_GREEN : C_RED);
    lv_obj_set_style_text_align(mark, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *title = make_label(s_root, 16, 170, 208, s_result_title, &tesla_font_20, C_TEXT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *detail = make_label(s_root, 20, 204, 200, s_result_detail, &tesla_font_16, C_MUTED);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
    build_footer("按 OK 返回主页");
}

static void build_page(void)
{
    screen_reset();
    switch (s_page) {
    case PAGE_MAIN: build_main(); break;
    case PAGE_SETTINGS: build_settings(); break;
    case PAGE_INFO: build_info(); break;
    case PAGE_PAIR: build_pair(); break;
    case PAGE_HOTSPOT: build_hotspot(); break;
    case PAGE_ABOUT: build_about(); break;
    case PAGE_RUNNING: build_running(); break;
    case PAGE_RESULT: build_result(); break;
    case PAGE_CONFIRM: build_confirm(); break;
    default: break;
    }
}

// --------------------------------------------------------------------------
// 结果文案
// --------------------------------------------------------------------------
static const char *fault_text(uint32_t fault)
{
    switch (fault) {
    case 1: return "车辆正忙，请稍后重试";
    case 3: return "车辆不认识这把钥匙，请重新完成授权";
    case 5: return "签名校验失败，请重新授权钥匙";
    case 6: return "密钥计数器失效，请重新授权钥匙";
    case 7: return "钥匙权限不足，无法执行该操作";
    case 15: return "安全会话已过期，请重试";
    case 17: return "命令已过期，请重试";
    case 20: return "命令有效期过长，请重试";
    default: return NULL;
    }
}

static const char *whitelist_text(uint32_t code)
{
    switch (code) {
    case 4: return "车辆钥匙白名单已满，请先删除不用的钥匙";
    case 5: return "本机无权添加钥匙，请重置蓝牙密钥后重试";
    case 6: return "钥匙公钥无效，请重置蓝牙密钥后重试";
    case 12: return "本机钥匙不在车辆白名单中，请重新授权";
    case 13: return "本机已在钥匙白名单中，无需重复添加";
    case 14: return "请把钥匙卡放到车辆读卡区";
    case 24: return "车辆屏幕拒绝了添加钥匙请求";
    case 25: return "刷卡超时，请把卡片放到读卡区后重试";
    case 26: return "等待车辆确认超时，请重试";
    case 27: return "车辆当前状态拒绝了添加钥匙请求";
    case 28: return "添加钥匙操作已取消，请重试";
    default: return NULL;
    }
}

static const char *error_text(int err, uint16_t detail, bool pairing)
{
    const char *specific = pairing ? whitelist_text(detail) : fault_text(detail);
    if (specific != NULL) {
        return specific;
    }
    switch (err) {
    case TESLA_OK: return "已完成";
    case TESLA_ERR_NO_VIN: return "尚未配置 VIN，请长按 OK 进入设置，用热点网页录入车辆信息";
    case TESLA_ERR_INIT: return "蓝牙未初始化，请重启设备";
    case TESLA_ERR_SCAN_TIMEOUT: return "未搜索到车辆。请确认车辆蓝牙已唤醒并靠近车辆";
    case TESLA_ERR_CONNECT: return "连接车辆失败，请靠近车辆后重试";
    case TESLA_ERR_SETUP: return "车辆服务发现失败，请重试";
    case TESLA_ERR_TX: return "发送命令失败，请重试";
    case TESLA_ERR_TIMEOUT: return "车辆未响应，请重试";
    case TESLA_ERR_PROTO: return "车辆响应格式异常";
    case TESLA_ERR_CRYPTO: return "安全会话校验失败，请重试或重新授权钥匙";
    case TESLA_ERR_NOT_AUTHORIZED: return "本机钥匙未获车辆授权，请先完成蓝牙钥匙授权";
    case TESLA_ERR_VEHICLE: return "车辆拒绝了该操作";
    case TESLA_ERR_BUSY: return "车辆正忙，请稍后重试";
    case TESLA_ERR_SESSION_STALE: return "安全会话已过期，请重试";
    default: return "未知错误";
    }
}

static void show_result(bool ok, const char *title, const char *detail)
{
    s_result_ok = ok;
    snprintf(s_result_title, sizeof(s_result_title), "%s", title);
    snprintf(s_result_detail, sizeof(s_result_detail), "%s", detail);
    s_page = PAGE_RESULT;
    s_dirty = true;
}

static void finish_action(uint8_t action, int err, uint32_t lock, bool has_lock)
{
    s_busy = false;
    tesla_ble_status_t status;
    tesla_ble_get_status(&status);
    s_ble = status;

    char detail[192];
    if (err == TESLA_OK) {
        if (action == ACT_STATUS) {
            // 状态读取成功直接回主页，主界面状态已更新，不再弹结果页。
            s_page = PAGE_MAIN;
            s_selected = 0;
            s_dirty = true;
            return;
        }
        const char *title = "操作成功";
        switch (action) {
        case ACT_UNLOCK: title = "车辆已解锁"; break;
        case ACT_LOCK: title = "车辆已上锁"; break;
        case ACT_FRUNK: title = "前备箱已打开"; break;
        case ACT_TRUNK: title = "后备箱已打开"; break;
        case ACT_PAIR: title = "蓝牙钥匙已授权"; break;
        default: break;
        }
        if (has_lock) {
            snprintf(detail, sizeof(detail), "当前车锁状态：%s", lock == 1 ? "已锁定" : "已解锁");
        } else if (action == ACT_PAIR) {
            snprintf(detail, sizeof(detail), "车辆已把本机加入钥匙白名单");
        } else {
            snprintf(detail, sizeof(detail), "车辆已确认执行该操作");
        }
        show_result(true, title, detail);
        return;
    }

    if (action == ACT_PAIR && err == TESLA_ERR_TIMEOUT) {
        const char *reason = whitelist_text(s_ble.detail);
        if (reason != NULL) {
            snprintf(detail, sizeof(detail), "%s", reason);
        } else {
            snprintf(detail, sizeof(detail), "请确认车辆处于添加钥匙模式，并将卡片放到读卡区后重试");
        }
    } else {
        snprintf(detail, sizeof(detail), "%s", error_text(err, s_ble.detail, action == ACT_PAIR));
    }
    show_result(false, action == ACT_PAIR    ? "授权未完成"
                        : action == ACT_STATUS ? "读取状态失败"
                                               : "操作失败",
                detail);
}

// --------------------------------------------------------------------------
// 事件
// --------------------------------------------------------------------------
static void push_event(const ui_event_t *event)
{
    if (s_ui_queue != NULL) {
        (void)xQueueSend(s_ui_queue, event, 0);
    }
}

static void on_ble_event(const tesla_ble_event_t *event)
{
    ui_event_t ui = {.kind = EVT_BLE, .ble = *event};
    push_event(&ui);
}

static void on_config_saved(const tesla_config_t *config)
{
    s_config = *config;
    tesla_ble_set_vin(config->vin);
    s_config_dirty = true;
}

static void start_action(uint8_t action)
{
    if (s_busy) {
        return;
    }
    if (!tesla_config_valid(&s_config)) {
        show_result(false, "需要先配置车辆",
                    "长按 OK 进入设置，打开热点后用手机网页录入 VIN、车牌号和车型");
        return;
    }
    s_busy = true;
    s_action = action;
    s_pair_progress = 0;
    s_page = PAGE_RUNNING;
    s_dirty = true;
    app_action_t item = {.action = action};
    if (s_action_queue != NULL) {
        (void)xQueueSend(s_action_queue, &item, 0);
    }
}

// 确认列表中的车辆后进入对配：停止扫描，按选定的广播地址连接。
static void start_pair_peer(void)
{
    if (s_busy || s_pair_cursor < 0 || s_pair_cursor >= s_peer_count) {
        return;
    }
    if (!tesla_config_valid(&s_config)) {
        show_result(false, "需要先配置车辆",
                    "长按 OK 进入设置，打开热点后用手机网页录入 VIN、车牌号和车型");
        return;
    }
    tesla_ble_peer_t peer = s_peers[s_pair_cursor];
    tesla_ble_discover_stop();
    s_pair_scanning = false;
    s_busy = true;
    s_action = ACT_PAIR;
    s_pair_progress = 0;
    s_page = PAGE_RUNNING;
    s_dirty = true;
    app_action_t item = {.action = ACT_PAIR, .addr_type = peer.addr_type};
    memcpy(item.addr, peer.addr, sizeof(item.addr));
    if (s_action_queue != NULL) {
        (void)xQueueSend(s_action_queue, &item, 0);
    }
}

static void apply_confirm(void)
{
    if (s_confirm_choice == 0) {
        s_page = PAGE_SETTINGS;
        s_dirty = true;
        return;
    }
    if (s_confirm_action == CONFIRM_CLEAR_VIN) {
        esp_err_t err = tesla_config_erase();
        memset(&s_config, 0, sizeof(s_config));
        tesla_ble_set_vin(NULL);
        if (err == ESP_OK) {
            show_result(true, "车辆信息已清空", "请重新用热点网页录入 VIN、车牌号和车型");
        } else {
            show_result(false, "清空失败", "配置存储写入失败，请重启设备后重试");
        }
        return;
    }
    esp_err_t err = tesla_ble_reset_key();
    if (err == ESP_OK) {
        show_result(true, "蓝牙密钥已重置", "请重新完成蓝牙钥匙授权，并在车辆端重新添加钥匙");
    } else {
        show_result(false, "重置失败", "请重启设备后重试");
    }
}

static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        switch (s_page) {
        case PAGE_MAIN:
            s_page = PAGE_SETTINGS;
            s_selected = 0;
            break;
        case PAGE_CONFIRM:
            s_page = PAGE_SETTINGS;
            break;
        case PAGE_SETTINGS:
        case PAGE_INFO:
        case PAGE_HOTSPOT:
        case PAGE_ABOUT:
        case PAGE_RESULT:
            s_page = PAGE_MAIN;
            s_selected = 0;
            break;
        case PAGE_PAIR:
            // 离开授权页时停止扫描，避免持续占用蓝牙。
            tesla_ble_discover_stop();
            s_pair_scanning = false;
            s_peer_count = 0;
            s_page = PAGE_MAIN;
            s_selected = 0;
            break;
        default:
            break;
        }
        s_dirty = true;
        return;
    }
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_DOUBLE) {
        return;
    }

    switch (s_page) {
    case PAGE_MAIN:
        // 主页快捷键：上单击锁车、上双击解锁；下单击开尾门、下双击关尾门；
        // OK 单击读取车辆状态。
        if (btn == BSP_BTN_UP) {
            start_action(ev == BSP_BTN_DOUBLE ? ACT_UNLOCK : ACT_LOCK);
        } else if (btn == BSP_BTN_DOWN) {
            start_action(ev == BSP_BTN_DOUBLE ? ACT_TRUNK_CLOSE : ACT_TRUNK);
        } else if (btn == BSP_BTN_OK) {
            start_action(ACT_STATUS);
        }
        break;
    case PAGE_SETTINGS:
        if (btn == BSP_BTN_UP) {
            s_selected = (s_selected + 5) % 6;
            s_dirty = true;
        } else if (btn == BSP_BTN_DOWN) {
            s_selected = (s_selected + 1) % 6;
            s_dirty = true;
        } else if (btn == BSP_BTN_OK) {
            switch (s_selected) {
            case 0: s_page = PAGE_INFO; break;
            case 1:
                s_page = PAGE_PAIR;
                s_pair_progress = 0;
                s_pair_cursor = 0;
                s_peer_count = 0;
                s_pair_refresh_us = esp_timer_get_time();
                s_pair_scanning = tesla_ble_discover_start() == ESP_OK;
                break;
            case 2:
                s_page = PAGE_HOTSPOT;
                if (!tesla_web_running()) {
                    tesla_web_start(&s_config, on_config_saved);
                }
                break;
            case 3: s_page = PAGE_ABOUT; break;
            case 4:
                s_confirm_action = CONFIRM_CLEAR_VIN;
                s_confirm_choice = 0;
                s_page = PAGE_CONFIRM;
                break;
            default:
                s_confirm_action = CONFIRM_RESET_KEY;
                s_confirm_choice = 0;
                s_page = PAGE_CONFIRM;
                break;
            }
            s_dirty = true;
        }
        break;
    case PAGE_CONFIRM:
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            s_confirm_choice = (uint8_t)(s_confirm_choice == 0 ? 1 : 0);
            s_dirty = true;
        } else if (btn == BSP_BTN_OK) {
            apply_confirm();
        }
        break;
    case PAGE_PAIR:
        // 上下键循环移动光标；确认后进入对配。
        if (btn == BSP_BTN_UP && s_peer_count > 0) {
            s_pair_cursor = (s_pair_cursor + s_peer_count - 1) % s_peer_count;
            s_dirty = true;
        } else if (btn == BSP_BTN_DOWN && s_peer_count > 0) {
            s_pair_cursor = (s_pair_cursor + 1) % s_peer_count;
            s_dirty = true;
        } else if (btn == BSP_BTN_OK) {
            start_pair_peer();
        }
        break;
    case PAGE_HOTSPOT:
        if (btn == BSP_BTN_OK) {
            if (tesla_web_running()) {
                tesla_web_stop();
            } else {
                tesla_web_start(&s_config, on_config_saved);
            }
            s_dirty = true;
        }
        break;
    case PAGE_RESULT:
        if (btn == BSP_BTN_OK) {
            s_page = PAGE_MAIN;
            s_selected = 0;
            s_dirty = true;
        }
        break;
    default:
        break;
    }
}

static void apply_event(const ui_event_t *event)
{
    switch (event->kind) {
    case EVT_KEY:
        s_last_activity = esp_timer_get_time();
        handle_key(event->btn, event->ev);
        break;
    case EVT_BLE:
        if (event->ble.type == TESLA_EVT_PAIR_PROGRESS) {
            s_pair_progress = event->ble.progress;
            if (s_page == PAGE_PAIR || s_page == PAGE_RUNNING) {
                s_dirty = true;
            }
        } else if (event->ble.type == TESLA_EVT_STATE) {
            if (s_page == PAGE_RUNNING || s_page == PAGE_MAIN) {
                s_dirty = true;
            }
        }
        break;
    case EVT_RESULT:
        finish_action(event->action, event->error, event->lock, event->has_lock);
        break;
    default:
        break;
    }
}

static void screen_sleep(void)
{
    if (!s_screen_on) {
        return;
    }
    if (s_page == PAGE_PAIR && s_pair_scanning) {
        tesla_ble_discover_stop();
        s_pair_scanning = false;
    }
    s_screen_on = false;
    bsp_display_backlight(0);
}

static void screen_wake(void)
{
    s_screen_on = true;
    bsp_display_backlight(SCREEN_BRIGHTNESS);
    s_last_activity = esp_timer_get_time();
    if (s_page == PAGE_PAIR && !s_pair_scanning) {
        s_pair_refresh_us = s_last_activity;
        s_pair_scanning = tesla_ble_discover_start() == ESP_OK;
    }
    s_dirty = true;
}

// 深睡在独立任务里执行：不能从 LVGL 定时器里再次拿 LVGL 锁，
// esp_timer 任务的栈也不够停 Wi-Fi/蓝牙。
static void deep_sleep_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(200));

    tesla_ble_status_t status;
    tesla_ble_get_status(&status);
    if (s_busy || s_web_on || s_screen_on || status.connected) {
        s_sleep_retry = esp_timer_get_time() + 3LL * 1000 * 1000;
        s_sleep_armed = false;
        vTaskDelete(NULL);
        return;
    }
    int mv = bsp_button_read_mv();
    if (mv >= 0 && mv < BSP_BTN_MV_RELEASED_MIN) {
        ESP_LOGI(TAG, "按键被按住，暂不深睡");
        s_sleep_retry = esp_timer_get_time() + 3LL * 1000 * 1000;
        s_sleep_armed = false;
        vTaskDelete(NULL);
        return;
    }

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "深睡前无法停止 LVGL 刷屏，重启恢复外设");
        esp_restart();
    }
    tesla_web_stop();
    tesla_ble_disconnect();
    (void)bsp_battery_sleep();
    (void)bsp_audio_sleep();
    (void)bsp_audio_prepare_deep_sleep();
    (void)bsp_i2c_prepare_deep_sleep();

    int level = 0;
    if (bsp_button_prepare_deep_sleep(&level) != ESP_OK) {
        ESP_LOGE(TAG, "按键交回失败，重启恢复外设");
        esp_restart();
    }
    // 三个按键都会把共享 ADC 脚拉到低电平，按低电平唤醒即可“任意键唤醒”。
    (void)esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (bsp_display_prepare_deep_sleep() != ESP_OK) {
        ESP_LOGE(TAG, "显示进入深睡失败，重启恢复外设");
        esp_restart();
    }
    ESP_LOGI(TAG, "进入深睡，按任意键唤醒");
    esp_deep_sleep_start();
    // 深睡准备接口是终端的；意外返回时只能重启。
    esp_restart();
}

static void ui_tick(lv_timer_t *timer)
{
    (void)timer;
    bool repaint = false;
    int64_t now = esp_timer_get_time();
    ui_event_t event;
    while (s_ui_queue != NULL && xQueueReceive(s_ui_queue, &event, 0) == pdTRUE) {
        apply_event(&event);
        repaint = true;
    }

    s_battery_tick++;
    if (s_battery_tick >= 40) {
        s_battery_tick = 0;
        int soc = bsp_battery_soc();
        if (soc != s_battery) {
            s_battery = soc;
            repaint = true;
        }
    }

    // 授权页每秒刷新一次广播列表与信号强度。
    if (s_page == PAGE_PAIR && s_pair_scanning && now - s_pair_refresh_us >= 1000 * 1000) {
        s_pair_refresh_us = now;
        pair_refresh();
        repaint = true;
    }

    tesla_ble_status_t status;
    tesla_ble_get_status(&status);
    if (memcmp(&status, &s_ble, sizeof(status)) != 0) {
        s_ble = status;
        repaint = true;
    }
    if (status.connected != s_was_connected) {
        s_was_connected = status.connected;
        if (!status.connected) {
            s_away_since = now;
        }
    }

    if (s_wake_request) {
        s_wake_request = false;
        if (!s_screen_on) {
            screen_wake();
        } else {
            s_last_activity = now;
        }
        repaint = true;
    }
    if (s_screen_on && !s_busy && now - s_last_activity >= SCREEN_OFF_DELAY_US) {
        screen_sleep();
    }
    if (!s_screen_on && !s_sleep_armed && !s_busy && !s_web_on && !status.connected &&
        now >= s_sleep_retry && now - s_last_activity >= DEEP_SLEEP_DELAY_US &&
        now - s_away_since >= DEEP_SLEEP_DELAY_US) {
        s_sleep_armed = true;
        if (xTaskCreate(deep_sleep_task, "tesla_sleep", 5120, NULL, 2, NULL) != pdPASS) {
            s_sleep_armed = false;
        }
    }

    bool web = tesla_web_running();
    if (web != s_web_on) {
        s_web_on = web;
        repaint = true;
    }
    if (s_config_dirty) {
        s_config_dirty = false;
        s_last_activity = now;
        repaint = true;
    }
    if (s_dirty) {
        s_dirty = false;
        repaint = true;
    }
    if (repaint) {
        if (s_screen_on) {
            build_page();
        } else {
            // 熄屏期间只记录需要重绘，等唤醒后一次性重建。
            s_dirty = true;
        }
    }
}

// --------------------------------------------------------------------------
// 后台动作
// --------------------------------------------------------------------------
static void action_task(void *arg)
{
    (void)arg;
    app_action_t item;
    for (;;) {
        if (xQueueReceive(s_action_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint32_t lock = 0;
        int err;
        if (item.action == ACT_PAIR) {
            err = tesla_ble_pair_peer(item.addr, item.addr_type);
        } else {
            // ACT_STATUS 的枚举值与 TESLA_CMD_* 不连续，需要单独映射。
            tesla_cmd_t cmd =
                item.action == ACT_STATUS ? TESLA_CMD_STATUS : (tesla_cmd_t)item.action;
            err = tesla_ble_execute(cmd, &lock);
        }
        ui_event_t event = {
            .kind = EVT_RESULT,
            .action = item.action,
            .error = err,
            .lock = lock,
            .has_lock = (item.action == ACT_UNLOCK || item.action == ACT_LOCK),
        };
        push_event(&event);
    }
}

// --------------------------------------------------------------------------
// 入口
// --------------------------------------------------------------------------
void tesla_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // 熄屏时任意键的第一下只点亮屏幕：按下就唤醒，松手前的所有事件都丢弃。
    if (s_wake_ignore) {
        if (ev == BSP_BTN_RELEASE) {
            s_wake_ignore = false;
        }
        return;
    }
    if (!s_screen_on) {
        if (ev == BSP_BTN_PRESS) {
            s_wake_ignore = true;
            s_wake_request = true;
        }
        return;
    }
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) {
        return;
    }
    ui_event_t event = {.kind = EVT_KEY, .btn = btn, .ev = ev};
    push_event(&event);
}

void tesla_app_start(void)
{
    s_ui_queue = xQueueCreate(16, sizeof(ui_event_t));
    s_action_queue = xQueueCreate(4, sizeof(app_action_t));
    if (s_ui_queue == NULL || s_action_queue == NULL) {
        ESP_LOGE(TAG, "创建队列失败");
        return;
    }
    xTaskCreate(action_task, "tesla_action", 8192, NULL, 4, NULL);

    s_last_activity = esp_timer_get_time();
    s_away_since = s_last_activity;

    tesla_config_load(&s_config);
    esp_err_t err = tesla_ble_init(on_ble_event);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE 初始化失败: %s", esp_err_to_name(err));
    }
    if (tesla_config_valid(&s_config)) {
        tesla_ble_set_vin(s_config.vin);
    }

    if (!bsp_lvgl_lock(2000)) {
        ESP_LOGE(TAG, "无法获取 LVGL 锁");
        return;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    s_root = lv_obj_create(screen);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_size(s_root, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_page = PAGE_MAIN;
    s_selected = 0;
    s_battery_tick = 39;
    tesla_ble_get_status(&s_ble);
    tesla_web_get_ap(s_ap_ssid, s_ap_pass, s_ap_ip);
    build_page();
    lv_timer_create(ui_tick, 120, NULL);
    bsp_lvgl_unlock();

    ESP_LOGI(TAG, "应用已启动，VIN=%s", s_config.vin[0] ? s_config.vin : "(未配置)");
}
