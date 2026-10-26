// main/app_pair.c —— 见头文件说明。
#include "app_pair.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "app_pair";
static const char *NVS_NAMESPACE = "apcfg";
static const char *KEY_SECRET = "ps_secret";
static const char *KEY_TOKEN = "ps_token";
static const char *KEY_HOST = "ps_host";

static uint8_t s_secret[AP_PAIR_SECRET_BYTES];
static bool s_has_secret = false;
static char s_token[AP_PAIR_TOKEN_HEX + 1];      // 空串 = 未配对
static char s_host[32];
static char s_code[AP_PAIR_CODE_DIGITS + 1];
static char s_device_id[16];
static ap_pair_rate_t s_rate;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out)
{
    static const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/** 读一个 blob；不存在返回 false。 */
static bool read_blob(nvs_handle_t handle, const char *key, void *out, size_t len)
{
    size_t size = len;
    return nvs_get_blob(handle, key, out, &size) == ESP_OK && size == len;
}

esp_err_t ap_pair_init(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开 NVS 失败：%s（配对状态只在本次开机内有效）", esp_err_to_name(err));
    } else {
        s_has_secret = read_blob(handle, KEY_SECRET, s_secret, sizeof(s_secret));
        if (!s_has_secret) {
            esp_fill_random(s_secret, sizeof(s_secret));
            if (nvs_set_blob(handle, KEY_SECRET, s_secret, sizeof(s_secret)) == ESP_OK) {
                (void)nvs_commit(handle);
                s_has_secret = true;
                ESP_LOGI(TAG, "首次开机：已生成设备密钥");
            } else {
                ESP_LOGW(TAG, "设备密钥写入失败（本次开机仍可用）");
                s_has_secret = true;
            }
        }

        size_t token_len = sizeof(s_token);
        if (nvs_get_str(handle, KEY_TOKEN, s_token, &token_len) != ESP_OK) {
            s_token[0] = '\0';
        }
        size_t host_len = sizeof(s_host);
        if (nvs_get_str(handle, KEY_HOST, s_host, &host_len) != ESP_OK) {
            s_host[0] = '\0';
        }
        nvs_close(handle);
    }

    // 设备短 ID：取蓝牙 MAC 后 3 字节（换设备就换 ID，主机信任表按它索引）
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    } else {
        snprintf(s_device_id, sizeof(s_device_id), "unknown");
    }

    // 本次上电的配对码：密钥 + 随机 nonce → 每次重启都换新码
    ap_pair_code(s_secret, sizeof(s_secret), esp_random(), s_code);
    ap_pair_rate_reset(&s_rate);

    ESP_LOGI(TAG, "设备 %s 配对状态：%s（本次配对码 %s）", s_device_id,
             s_token[0] ? "已配对" : "未配对", s_token[0] ? "不适用" : s_code);
    return ESP_OK;
}

const char *ap_pair_device_id(void) { return s_device_id; }
const char *ap_pair_code_str(void) { return s_code; }
bool ap_pair_is_paired(void) { return s_token[0] != '\0'; }
const char *ap_pair_host_name(void) { return s_host; }

bool ap_pair_host_token_ok(const char *token_hex)
{
    if (s_token[0] == '\0') return false;      // 未配对：任何 token 都不算数
    return ap_pair_token_equal(s_token, token_hex);
}

int ap_pair_attempts_left(void)
{
    if (ap_pair_rate_allow(&s_rate, now_ms())) {
        const int left = AP_PAIR_MAX_ATTEMPTS - (int)s_rate.attempts;
        return left > 0 ? left : 0;
    }
    return 0;
}

esp_err_t ap_pair_begin(const char *code, const char *host_name,
                        char out_token[AP_PAIR_TOKEN_HEX + 1])
{
    const uint32_t now = now_ms();
    if (!ap_pair_rate_allow(&s_rate, now)) {
        ESP_LOGW(TAG, "配对尝试过于频繁，已限流");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ap_pair_code_equal(s_code, code)) {
        ap_pair_rate_record_failure(&s_rate, now);
        ESP_LOGW(TAG, "配对码错误（本窗口还剩 %d 次）", ap_pair_attempts_left());
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t token[AP_PAIR_TOKEN_BYTES];
    esp_fill_random(token, sizeof(token));
    bytes_to_hex(token, sizeof(token), out_token);
    snprintf(s_token, sizeof(s_token), "%s", out_token);
    snprintf(s_host, sizeof(s_host), "%s", host_name ? host_name : "");
    ap_pair_rate_reset(&s_rate);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_TOKEN, s_token);
        if (err == ESP_OK) err = nvs_set_str(handle, KEY_HOST, s_host);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "token 写入 NVS 失败：%s（本次开机内仍有效）", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "配对成功：主机=%s", s_host);
    return ESP_OK;
}

esp_err_t ap_pair_reset(void)
{
    s_token[0] = '\0';
    s_host[0] = '\0';
    // 换新码：解除配对后旧码作废
    ap_pair_code(s_secret, sizeof(s_secret), esp_random(), s_code);
    ap_pair_rate_reset(&s_rate);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        (void)nvs_erase_key(handle, KEY_TOKEN);
        (void)nvs_erase_key(handle, KEY_HOST);
        err = nvs_commit(handle);
        nvs_close(handle);
    }
    ESP_LOGW(TAG, "已解除配对（新配对码 %s）", s_code);
    return err;
}
