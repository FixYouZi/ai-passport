// main/tesla_web.c
// SoftAP + HTTP 配置页。启动后 10 分钟无操作自动关闭，避免长期占用射频。
#include "tesla_web.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "tesla_ble.h"

static const char *TAG = "tesla_web";

#define AP_PASSWORD "tesla1234"
#define AP_TIMEOUT_US (10 * 60 * 1000 * 1000ULL)
#define PAGE_SIZE 6144

static bool s_netif_ready;
static bool s_running;
static httpd_handle_t s_server;
static esp_timer_handle_t s_timer;
static char s_ssid[33];
static char s_password[33];
static tesla_config_t s_config;
static tesla_web_saved_cb_t s_on_saved;

static void auto_stop(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "热点配置超时，自动关闭");
    tesla_web_stop();
}

static void html_escape(const char *src, char *dst, size_t cap)
{
    size_t out = 0;
    for (const char *p = src; p != NULL && *p != '\0' && out + 1 < cap; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        if (rep != NULL) {
            size_t len = strlen(rep);
            if (out + len >= cap) {
                break;
            }
            memcpy(dst + out, rep, len);
            out += len;
        } else {
            dst[out++] = *p;
        }
    }
    dst[out] = '\0';
}

static void url_decode(const char *src, char *dst, size_t cap)
{
    size_t out = 0;
    while (*src != '\0' && out + 1 < cap) {
        if (*src == '+') {
            dst[out++] = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) &&
                   isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], 0};
            dst[out++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[out++] = *src++;
        }
    }
    dst[out] = '\0';
}

static esp_err_t send_page(httpd_req_t *req, const char *page)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    char *page = malloc(PAGE_SIZE);
    if (page == NULL) {
        return httpd_resp_send_500(req);
    }
    char vin[64];
    char plate[96];
    char fingerprint[9];
    char query[32];
    bool is_model_y = strstr(s_config.model, "Y") != NULL;
    const char *pick_3 = is_model_y ? "" : " selected";
    const char *pick_y = is_model_y ? " selected" : "";
    bool saved = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                 strstr(query, "ok=1") != NULL;
    html_escape(s_config.vin, vin, sizeof(vin));
    html_escape(s_config.plate, plate, sizeof(plate));
    tesla_ble_fingerprint(fingerprint);

    int written = snprintf(
        page, PAGE_SIZE,
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>特斯拉卡片钥匙</title><style>"
        ":root{color-scheme:dark}"
        "body{margin:0;background:#0f1115;color:#e9edf2;font-family:system-ui,-apple-system,"
        "'PingFang SC','Microsoft YaHei',sans-serif}"
        ".wrap{max-width:520px;margin:0 auto;padding:24px 18px 40px}"
        ".badge{display:inline-block;padding:4px 10px;border-radius:999px;background:#2a1215;"
        "color:#ff5b61;font-size:12px;letter-spacing:.08em}"
        "h1{font-size:24px;margin:14px 0 6px}"
        "p.sub{color:#8b95a5;font-size:13px;margin:0 0 22px}"
        "form{background:#171b22;border:1px solid #242a33;border-radius:16px;padding:18px}"
        "label{display:block;font-size:13px;color:#9aa4b2;margin:14px 0 6px}"
        "input,select{width:100%%;box-sizing:border-box;background:#0f1217;"
        "border:1px solid #2b323c;"
        "border-radius:10px;color:#e9edf2;padding:12px 12px;font-size:16px;outline:none}"
        "input:focus,select:focus{border-color:#e82127}"
        "button{width:100%%;margin-top:20px;background:#e82127;border:0;border-radius:10px;"
        "color:#fff;font-size:16px;font-weight:600;padding:13px}"
        ".meta{margin-top:18px;font-size:12px;color:#6f7a8a;line-height:1.7}"
        ".ok{background:#10231a;color:#4ade80;border:1px solid #1f4a33;border-radius:10px;"
        "padding:10px 12px;font-size:13px;margin-bottom:16px}"
        "</style></head><body><div class=\"wrap\">"
        "<span class=\"badge\">TESLA BLE KEY</span>"
        "<h1>车辆配置</h1>"
        "<p class=\"sub\">连接本机热点后填写车辆信息，保存后立即生效。</p>"
        "%s"
        "<form method=\"post\" action=\"/save\">"
        "<label>车辆识别码 VIN（17 位）</label>"
        "<input name=\"vin\" maxlength=\"17\" autocapitalize=\"characters\" "
        "placeholder=\"例如 5YJ3E1EA7KF000000\" value=\"%s\" required>"
        "<label>车牌号</label>"
        "<input name=\"plate\" maxlength=\"32\" placeholder=\"例如 沪A·D12345\" value=\"%s\">"
        "<label>车型</label>"
        "<select name=\"model\">"
        "<option value=\"Model 3\"%s>Model 3</option>"
        "<option value=\"Model Y\"%s>Model Y</option>"
        "</select>"
        "<button type=\"submit\">保存配置</button></form>"
        "<div class=\"meta\">本机公钥指纹：%s<br>热点：%s<br>配置地址：http://192.168.4.1</div>"
        "</div></body></html>",
        saved ? "<div class=\"ok\">配置已保存，蓝牙将在下一次操作时使用新 VIN。</div>" : "",
        vin, plate, pick_3, pick_y, fingerprint, s_ssid);
    if (written < 0 || written >= PAGE_SIZE) {
        free(page);
        return httpd_resp_send_500(req);
    }
    esp_err_t err = send_page(req, page);
    free(page);
    return err;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    char body[512];
    size_t total = 0;
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求内容过长");
        return ESP_FAIL;
    }
    while (remaining > 0) {
        int received = httpd_req_recv(req, body + total, remaining);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "读取失败");
            return ESP_FAIL;
        }
        total += (size_t)received;
        remaining -= received;
    }
    body[total] = '\0';

    tesla_config_t next = s_config;
    char raw_vin[64] = {0};
    char *save = NULL;
    for (char *pair = strtok_r(body, "&", &save); pair != NULL;
         pair = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(pair, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char value[96];
        url_decode(eq + 1, value, sizeof(value));
        if (strcmp(pair, "vin") == 0) {
            strncpy(raw_vin, value, sizeof(raw_vin) - 1);
        } else if (strcmp(pair, "plate") == 0) {
            strncpy(next.plate, value, sizeof(next.plate) - 1);
        } else if (strcmp(pair, "model") == 0) {
            strncpy(next.model, value, sizeof(next.model) - 1);
        }
    }

    // 车型只保留下拉框的两个选项，旧数据或异常值回落到 Model 3。
    strncpy(next.model, strstr(next.model, "Y") != NULL ? "Model Y" : "Model 3",
            sizeof(next.model) - 1);
    next.model[sizeof(next.model) - 1] = '\0';

    char normalized[TESLA_VIN_BUF];
    if (!tesla_config_normalize_vin(raw_vin, normalized)) {
        char *page = malloc(1024);
        if (page != NULL) {
            snprintf(page, 1024,
                     "<!doctype html><meta charset=\"utf-8\">"
                     "<body style=\"background:#0f1115;color:#e9edf2;font-family:sans-serif;"
                     "padding:32px\"><h2>VIN 无效</h2>"
                     "<p style=\"color:#8b95a5\">VIN 必须为 17 位大写字母和数字，且不含 I、O、Q。</p>"
                     "<a style=\"color:#ff5b61\" href=\"/\">返回修改</a></body>");
            send_page(req, page);
            free(page);
        }
        return ESP_OK;
    }
    strncpy(next.vin, normalized, sizeof(next.vin) - 1);

    if (tesla_config_save(&next) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
        return ESP_FAIL;
    }
    s_config = next;
    if (s_on_saved != NULL) {
        s_on_saved(&s_config);
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?ok=1");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t start_wifi(void)
{
    if (!s_netif_ready) {
        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&init);
        if (err != ESP_OK) {
            return err;
        }
        s_netif_ready = true;
    }

    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "TeslaKey-%02X%02X", mac[4], mac[5]);
    strncpy(s_password, AP_PASSWORD, sizeof(s_password) - 1);

    wifi_config_t wifi = {0};
    strncpy((char *)wifi.ap.ssid, s_ssid, sizeof(wifi.ap.ssid) - 1);
    wifi.ap.ssid_len = (uint8_t)strlen(s_ssid);
    strncpy((char *)wifi.ap.password, s_password, sizeof(wifi.ap.password) - 1);
    wifi.ap.max_connection = 2;
    wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "热点已启动：%s", s_ssid);
    return ESP_OK;
}

esp_err_t tesla_web_start(const tesla_config_t *current, tesla_web_saved_cb_t on_saved)
{
    if (s_running) {
        return ESP_OK;
    }
    if (current != NULL) {
        s_config = *current;
    } else {
        tesla_config_load(&s_config);
    }
    s_on_saved = on_saved;

    esp_err_t err = start_wifi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动热点失败: %s", esp_err_to_name(err));
        return err;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动 HTTP 失败: %s", esp_err_to_name(err));
        esp_wifi_stop();
        return err;
    }
    httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_handler};
    httpd_register_uri_handler(s_server, &index);
    httpd_register_uri_handler(s_server, &save);

    if (s_timer == NULL) {
        esp_timer_create_args_t args = {.callback = auto_stop, .name = "tesla_web"};
        if (esp_timer_create(&args, &s_timer) != ESP_OK) {
            s_timer = NULL;
        }
    }
    if (s_timer != NULL) {
        esp_timer_stop(s_timer);
        esp_timer_start_once(s_timer, AP_TIMEOUT_US);
    }
    s_running = true;
    return ESP_OK;
}

void tesla_web_stop(void)
{
    if (s_timer != NULL) {
        esp_timer_stop(s_timer);
    }
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_running) {
        esp_wifi_stop();
    }
    s_running = false;
}

bool tesla_web_running(void)
{
    return s_running;
}

void tesla_web_get_ap(char ssid[33], char pass[33], char ip[16])
{
    if (ssid != NULL) {
        strncpy(ssid, s_ssid, 32);
        ssid[32] = '\0';
    }
    if (pass != NULL) {
        strncpy(pass, s_password, 32);
        pass[32] = '\0';
    }
    if (ip != NULL) {
        strncpy(ip, "192.168.4.1", 15);
        ip[15] = '\0';
    }
}
