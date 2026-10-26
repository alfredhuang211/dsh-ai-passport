// main/app_settings.c —— 见头文件说明。
#include "app_settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "app_settings";
static const char *NVS_NAMESPACE = "apcfg";
static const char *KEY_SCREEN_OFF = "scr_off";
static const char *KEY_BRIGHT = "bright";
static const char *KEY_AUTO_OFF = "auto_off";
static const char *KEY_ALERT_VOL = "alert_vol";
static const char *KEY_POWERED_OFF = "pwr_off";

static ap_settings_t s_settings = {
    .screen_off_min = AP_SETTINGS_DEFAULT_SCREEN_OFF_MIN,
    .brightness = AP_SETTINGS_DEFAULT_BRIGHTNESS,
    .auto_off_min = AP_SETTINGS_DEFAULT_AUTO_OFF_MIN,
    .alert_volume = AP_SETTINGS_DEFAULT_ALERT_VOLUME,
};
static bool s_loaded = false;
static bool s_powered_off = false;

/** 读一个 int32；键不存在或类型不符时返回 fallback。 */
static int read_int(nvs_handle_t handle, const char *key, int fallback)
{
    int32_t value = 0;
    const esp_err_t err = nvs_get_i32(handle, key, &value);
    if (err == ESP_OK) return (int)value;
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取 %s 失败：%s（用默认值）", key, esp_err_to_name(err));
    }
    return fallback;
}

esp_err_t ap_settings_init(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // 命名空间还不存在（第一次开机）：全用默认值，等用户改设置时再创建
        ESP_LOGI(TAG, "设置命名空间不存在，使用默认值（%s）", esp_err_to_name(err));
        s_loaded = true;
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    uint8_t powered_off = 0;
    if (nvs_get_u8(handle, KEY_POWERED_OFF, &powered_off) == ESP_OK) {
        s_powered_off = (powered_off != 0);
    }
    s_settings.screen_off_min = read_int(handle, KEY_SCREEN_OFF, s_settings.screen_off_min);
    s_settings.brightness = read_int(handle, KEY_BRIGHT, s_settings.brightness);
    s_settings.auto_off_min = read_int(handle, KEY_AUTO_OFF, s_settings.auto_off_min);
    s_settings.alert_volume = read_int(handle, KEY_ALERT_VOL, s_settings.alert_volume);
    nvs_close(handle);

    // 存过野值（表外）时归一，避免出现"设置页显示不出来的档位"
    s_settings.screen_off_min =
        ap_settings_item(AP_SETTINGS_SCREEN_OFF)
            ->values[ap_settings_index_of(ap_settings_item(AP_SETTINGS_SCREEN_OFF), s_settings.screen_off_min)];
    s_settings.brightness =
        ap_settings_item(AP_SETTINGS_BRIGHTNESS)
            ->values[ap_settings_index_of(ap_settings_item(AP_SETTINGS_BRIGHTNESS), s_settings.brightness)];
    s_settings.auto_off_min =
        ap_settings_item(AP_SETTINGS_AUTO_OFF)
            ->values[ap_settings_index_of(ap_settings_item(AP_SETTINGS_AUTO_OFF), s_settings.auto_off_min)];
    s_settings.alert_volume =
        ap_settings_item(AP_SETTINGS_ALERT_VOLUME)
            ->values[ap_settings_index_of(ap_settings_item(AP_SETTINGS_ALERT_VOLUME), s_settings.alert_volume)];

    s_loaded = true;
    ESP_LOGI(TAG, "设置已加载：熄屏=%d 分钟（0=常亮）亮度=%d%% 自动休眠=%d 分钟（0=不休眠）提示音量=%d%%",
             s_settings.screen_off_min, s_settings.brightness, s_settings.auto_off_min,
             s_settings.alert_volume);
    return ESP_OK;
}

const ap_settings_t *ap_settings_get(void) { return &s_settings; }

bool ap_settings_is_powered_off(void) { return s_powered_off; }

esp_err_t ap_settings_set_powered_off(bool powered_off)
{
    if (s_powered_off == powered_off) return ESP_OK;
    s_powered_off = powered_off;

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开设置命名空间失败：%s（休眠标记只在本次开机内有效）", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(handle, KEY_POWERED_OFF, powered_off ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "写入休眠标记失败：%s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "休眠标记 = %d", powered_off ? 1 : 0);
    }
    return err;
}

esp_err_t ap_settings_set_value(ap_settings_item_t item, int value)
{
    const ap_settings_item_def_t *def = ap_settings_item(item);
    const int normalized = def->values[ap_settings_index_of(def, value)];
    int *slot = NULL;
    const char *key = NULL;
    switch (item) {
    case AP_SETTINGS_SCREEN_OFF:
        slot = &s_settings.screen_off_min;
        key = KEY_SCREEN_OFF;
        break;
    case AP_SETTINGS_BRIGHTNESS:
        slot = &s_settings.brightness;
        key = KEY_BRIGHT;
        break;
    case AP_SETTINGS_AUTO_OFF:
        slot = &s_settings.auto_off_min;
        key = KEY_AUTO_OFF;
        break;
    case AP_SETTINGS_ALERT_VOLUME:
        slot = &s_settings.alert_volume;
        key = KEY_ALERT_VOL;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (*slot == normalized) return ESP_OK;   // 值没变就不写 NVS（省擦写寿命）
    *slot = normalized;

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "打开设置命名空间失败：%s（本次开机内仍生效）", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_i32(handle, key, normalized);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "写入 %s 失败：%s（本次开机内仍生效）", key, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "%s 已保存为 %d（%s）", def->name, normalized, "");
    }
    return err;
}
