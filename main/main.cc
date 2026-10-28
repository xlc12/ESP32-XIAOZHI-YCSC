#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>

#include "application.h"
#include "system_info.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // ===== 调试期日志策略 =====
    // 全局只保留 ERROR，把 WiFi/LVGL/AFE 等刷屏信息压掉；
    // 只给音频链路和状态机放到 INFO，方便在串口里一眼看到关键标记。
    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set("main", ESP_LOG_INFO);
    esp_log_level_set("Application", ESP_LOG_INFO);
    esp_log_level_set("Ci130xAudioCodec", ESP_LOG_INFO);
    esp_log_level_set("Ci130xProtocol", ESP_LOG_INFO);
    // 会话建立/断开必须看得见，否则"唤醒后没进 listening"无从判断
    esp_log_level_set("WS", ESP_LOG_INFO);
    esp_log_level_set("Protocol", ESP_LOG_INFO);
    esp_log_level_set("MQTT", ESP_LOG_INFO);

    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
