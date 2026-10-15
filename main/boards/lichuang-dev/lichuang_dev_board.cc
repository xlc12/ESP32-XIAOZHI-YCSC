#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/circular_strip.h"
#include "pn532.h"
#include "power_manager.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include <lvgl.h>


#define TAG "LichuangDevBoard"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

// 校验 NDEF 文本中的 MAC 地址是否合理。
// 期望格式：6 组 2 位十六进制，用 ':' 或 '-' 分隔（如 "AC:A7:04:EB:EC:20"）。
// 校验通过时把去掉首尾空白后的地址写入 out_mac 并返回 true。
static bool ParseMacAddress(const std::string& text, std::string& out_mac) {
    // 去除首尾空白（NDEF 文本可能带空格或换行）
    const char* kSpace = " \t\r\n";
    size_t begin = text.find_first_not_of(kSpace);
    if (begin == std::string::npos) {
        return false;
    }
    size_t end = text.find_last_not_of(kSpace);
    std::string mac = text.substr(begin, end - begin + 1);

    // 形如 XX:XX:XX:XX:XX:XX（共 17 个字符）
    if (mac.size() != 17) {
        return false;
    }
    char sep = mac[2];
    if (sep != ':' && sep != '-') {
        return false;
    }

    uint8_t octet[6] = {0};
    for (int i = 0; i < 6; ++i) {
        size_t pos = static_cast<size_t>(i) * 3;
        if (i > 0 && mac[pos - 1] != sep) {
            return false;
        }
        for (int j = 0; j < 2; ++j) {
            char c = mac[pos + j];
            uint8_t v;
            if (c >= '0' && c <= '9') {
                v = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                v = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                v = c - 'A' + 10;
            } else {
                return false;
            }
            octet[i] = static_cast<uint8_t>((octet[i] << 4) | v);
        }
    }

    // 合理性检查：排除全 0、广播地址，以及组播地址（首字节最低位为 1）
    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < 6; ++i) {
        all_zero = all_zero && (octet[i] == 0x00);
        all_ff = all_ff && (octet[i] == 0xFF);
    }
    if (all_zero || all_ff || (octet[0] & 0x01)) {
        return false;
    }

    out_mac = mac;
    return true;
}

class LichuangDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button button2_;
    LcdDisplay* display_;
    Pn532* nfc_ = nullptr;
    PowerManager* power_manager_;
    std::string last_ndef_text_;   // 上一次已处理的卡片 NDEF 内容，同一张卡不重复处理

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_46;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_21;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            // app.ToggleChatState();
        });

        boot_button_.OnPressDown([this]() {
            ESP_LOGI(TAG, "[BOOT] 按键按下，开始聆听");
            Application::GetInstance().StartListening();
        });
        boot_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, "[BOOT] 按键松开，停止聆听");
            Application::GetInstance().StopListening();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif

        button2_.OnClick([this]() {
            Application::GetInstance().ToggleChatState();
        });
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = GPIO_NUM_39;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    {
                                        .text_font = &font_puhui_20_4,
                                        .icon_font = &font_awesome_20_4,
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
                                        .emoji_font = font_emoji_32_init(),
#else
                                        .emoji_font = font_emoji_64_init(),
#endif
                                    });
    }

    // 初始化 PN532 NFC 模块并启动后台寻卡任务
    void InitializeNfc() {
        nfc_ = new Pn532(PN532_UART_PORT, PN532_UART_TX_PIN, PN532_UART_RX_PIN,
                         PN532_UART_BAUDRATE);

        // 卡片接入回调（在 PN532 寻卡任务上下文中执行，勿做阻塞操作）
        nfc_->OnCardDetected([this](const Pn532::CardInfo& info) {
            char uid_str[32] = {0};
            for (uint8_t i = 0; i < info.uid_len; ++i) {
                snprintf(uid_str + i * 3, sizeof(uid_str) - i * 3, "%02X ", info.uid[i]);
            }
            ESP_LOGI(TAG, "[NFC] 卡片接入 UID=%s", uid_str);
            ESP_LOGI(TAG, "[NFC] 读到的地址(NDEF)=\"%s\"", info.ndef_text.c_str());

            // 同一张卡片（NDEF 内容相同）不重复处理（例如拿走后再刷同一张）
            if (!info.ndef_text.empty() && last_ndef_text_ == info.ndef_text) {
                ESP_LOGI(TAG, "[NFC] 与上次处理的卡片相同，忽略");
                return;
            }
            last_ndef_text_ = info.ndef_text;

            // 以卡片内写入的 MAC 地址作为 Device-Id 重连 WS
            std::string mac;
            if (!ParseMacAddress(info.ndef_text, mac)) {
                ESP_LOGW(TAG, "[NFC] MAC 地址不合理，忽略本次切换: \"%s\"", info.ndef_text.c_str());
                // 播报错误提示音（切到主循环执行，避免在寻卡任务中操作音频/显示）
                Application::GetInstance().Schedule([]() {
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);
                });
                return;
            }

            ESP_LOGI(TAG, "[NFC] 使用卡片读取的 MAC 地址重连: %s", mac.c_str());
            Application::GetInstance().ReconnectWithMacAddress(mac);
        });

        // 卡片移除回调
        nfc_->OnCardRemoved([]() {
            ESP_LOGI(TAG, "[NFC] 卡片已移除");

            // TODO: 在这里处理卡片离开后的逻辑（如恢复默认角色）
        });

        nfc_->Start();
    }

    // ADC 电池管理
    void InitializePowerManager() {
        power_manager_ = new PowerManager(POWER_CHARGE_DETECT_PIN, POWER_CHARGE_COMPLETE_PIN,
                                          POWER_ADC_UNIT, POWER_ADC_CHANNEL);
        power_manager_->InitializeMcpTools();
    }

public:
    LichuangDevBoard() : boot_button_(BOOT_BUTTON_GPIO), button2_(BUTTON2_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeNfc();
        InitializePowerManager();

        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = power_manager_->IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual Led* GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, 30);
        return &led;
    }
};

DECLARE_BOARD(LichuangDevBoard);
