#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>

#include <font_awesome.h>
#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>
#include "afsk_demod.h"
#ifdef CONFIG_USE_BLUFI_NET_CONFIGURING
#include "esp_mac.h"
#include "blufi_wificfg.h"
#endif

static const char *TAG = "WifiBoard";

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::EnterWifiConfigMode()
{
#ifdef CONFIG_USE_BLUFI_NET_CONFIGURING

    ble_active_ = true; // 设置蓝牙为活跃状态

    bool is_got_ip = false;
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, [](void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
                               {
        bool *is_got_ip = (bool *)arg;
        *is_got_ip = true; }, &is_got_ip);
    

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    // static char blufi_device_name[18];
    // snprintf(blufi_device_name, sizeof(blufi_device_name), "DTXZ_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    // add by xlc-mqtt -begin
    #if 1//正式版 app sy0001
    // char blufi_device_name[18+2];
    // snprintf(blufi_device_name, sizeof(blufi_device_name), "SY_0001%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char blufi_device_name[18+2];
    snprintf(blufi_device_name, sizeof(blufi_device_name), "YL_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    #else//test 乐鑫官方app
    char blufi_device_name[18+2];
    strcpy(blufi_device_name, BLUFI_DEVICE_NAME);
    #endif
    // add by xlc -end


    blufi_wificfg_cbs_t cbs = {
        .sta_config_cb = [](const wifi_config_t *config, void *arg)
        {
            ESP_LOGI(TAG, "Received sta config, ssid: %s, password: %s", config->sta.ssid, config->sta.password);
            std::string ssid(reinterpret_cast<const char*>(config->sta.ssid));
            std::string password(reinterpret_cast<const char*>(config->sta.password));
            SsidManager::GetInstance().AddSsid(ssid, password); },
        .custom_data_cb = [](const uint8_t *data, size_t len, void *arg)
        {
            ESP_LOGI(TAG, "Received custom data: %.*s", (int)len, data);
            if (strncmp((char *)data, "AT+OTA=", 7) == 0) {
                std::string url(reinterpret_cast<const char*>(data+7), len-7);
                ESP_LOGI(TAG, "ota_url: %s", url.c_str());
                Settings settings("wifi", true);
                settings.SetString("ota_url", url);
            } 
            else if (len >= 8 && strncmp((char *)data, "AT+UUID=", 8) == 0) {
                std::string uuid_str(reinterpret_cast<const char*>(data+8), len-8);
                ESP_LOGI(TAG, "解析到APP UUID: %s", uuid_str.c_str());
                Settings settings("blufi", true);
                settings.SetString("blufi_app_uuid", uuid_str);//保存APP UUID到内存中
            } 
            else {
                ESP_LOGI(TAG, "接收到的自定义数据: %.*s", (int)len, data);
            }
        }
    };

    auto &application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);
    static char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    std::string msg = std::string(mac_str) + "\r\n" + "网络配置中";
    application.Alert(Lang::Strings::WIFI_CONFIG_MODE, msg.c_str(), "", Lang::Sounds::OGG_WIFICONFIG);

    vTaskDelay(pdMS_TO_TICKS(2000));
    // application.ReleaseDecoder();

    blufi_wificfg_start(true, blufi_device_name, cbs, this);

    while (!is_got_ip)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    Ota ota;
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒
    while (true)
    {
        if (!ota.CheckVersion())
        {
            retry_count++;
            if (retry_count >= MAX_RETRY)
            {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                ResetWifiConfiguration();
                return;
            }

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++)
            {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }

        auto &code = ota.GetActivationCode();
        ESP_LOGI(TAG, "Activation code: %s", code.c_str());
        if (!code.empty())
        {
            blufi_wificfg_send_custom((uint8_t *)code.c_str(), code.length());
        }
        else
        {
            uint8_t data[6] = {0};
            blufi_wificfg_send_custom(data, 6);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
        ble_active_ = false; // 设置蓝牙为非活跃状态
        esp_restart();
    }
#else
    auto &application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    auto& wifi_ap = WifiConfigurationAp::GetInstance();
    wifi_ap.SetLanguage(Lang::CODE);
    wifi_ap.SetSsidPrefix("Xiaozhi");
    wifi_ap.Start();

    // 显示 WiFi 配置 AP 的 SSID 和 Web 服务器 URL
    std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
    hint += wifi_ap.GetSsid();
    hint += Lang::Strings::ACCESS_VIA_BROWSER;
    hint += wifi_ap.GetWebServerUrl();
    hint += "\n\n";
    
    // 播报配置 WiFi 的提示
    application.Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_1_1_WIFI_CONFIG);

    #if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    auto display = Board::GetInstance().GetDisplay();
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = 1;
    if (codec) {
        channel = codec->input_channels();
    }
    ESP_LOGI(TAG, "Start receiving WiFi credentials from audio, input channels: %d", channel);
    audio_wifi_config::ReceiveWifiCredentialsFromAudio(&application, &wifi_ap, display, channel);
    #endif
    
    // Wait forever until reset after configuration
    while (true)
    {
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", free_sram, min_free_sram);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#endif
}

void WifiBoard::StartNetwork() {
    // User can press BOOT button while starting to enter WiFi configuration mode
    if (wifi_config_mode_) {
        EnterWifiConfigMode();
        return;
    }

    // If no WiFi SSID is configured, enter WiFi configuration mode
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
    });
    wifi_station.OnConnect([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECT_TO;
        notification += ssid;
        notification += "...";
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.OnConnected([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        auto& app = Application::GetInstance();
        std::string notification = Lang::Strings::CONNECTED_TO;
        notification += ssid;
        display->ShowNotification(notification.c_str(), 30000);
        // 播报 WiFi 连接成功提示
        app.PlaySound(Lang::Sounds::OGG_1_2_WIFI_CONNET_SUC);
    });
    wifi_station.Start();

    // Try to connect to WiFi, if failed, launch the WiFi configuration AP
    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }
    int8_t rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

// const char *WifiBoard::GetBleStateIcon()
// {
//     return ble_active_ ? FONT_AWESOME_BLUETOOTH : "";
// }

std::string WifiBoard::GetBoardJson() {
    // Set the board type for OTA
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = R"({)";
    board_json += R"("type":")" + std::string(BOARD_TYPE) + R"(",)";
    board_json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    if (!wifi_config_mode_) {
        board_json += R"("ssid":")" + wifi_station.GetSsid() + R"(",)";
        board_json += R"("rssi":)" + std::to_string(wifi_station.GetRssi()) + R"(,)";
        board_json += R"("channel":)" + std::to_string(wifi_station.GetChannel()) + R"(,)";
        board_json += R"("ip":")" + wifi_station.GetIpAddress() + R"(",)";
    }
    board_json += R"("mac":")" + SystemInfo::GetMacAddress() + R"(")";
    board_json += R"(})";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    // Set a flag and reboot the device to enter the network configuration mode
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    esp_restart();
}

std::string WifiBoard::GetDeviceStatusJson() {
    /*
     * 返回设备状态JSON
     * 
     * 返回的JSON结构如下：
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "wifi",
     *         "ssid": "Xiaozhi",
     *         "rssi": -60
     *     },
     *     "chip": {
     *         "temperature": 25
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        cJSON_AddStringToObject(screen, "theme", display->GetTheme().c_str());
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    auto& wifi_station = WifiStation::GetInstance();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi_station.GetSsid().c_str());
    int rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        cJSON_AddStringToObject(network, "signal", "strong");
    } else if (rssi >= -70) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else {
        cJSON_AddStringToObject(network, "signal", "weak");
    }
    cJSON_AddItemToObject(root, "network", network);

    // Chip
    float esp32temp = 0.0f;
    if (board.GetTemperature(esp32temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", esp32temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
