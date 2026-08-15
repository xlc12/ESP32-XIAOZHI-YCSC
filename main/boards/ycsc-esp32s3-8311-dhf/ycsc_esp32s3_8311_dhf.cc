#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "settings.h"
#include "led/gpio_led.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/uart.h>
#include <cstring>

#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif

//YCSC
// #include "boards/common/da218e.h"
// #include "gsensor_action.h"
#include <esp_lcd_gc9a01.h>
// #include "esp_lcd_gc9a01.h"



#if defined(LCD_TYPE_ILI9341_SERIAL)
#include "esp_lcd_ili9341.h"
#endif


#define TAG "YcscEsp32S3Es8311Dhf"


/***** 运动控制串口 -begin *****/

// 运动控制命令值（帧格式：0x55 0x5A + 命令 + 0x5B）
typedef enum {
    MOTION_CMD_STOP = 0xAA,     // 停止
    MOTION_CMD_FORWARD = 0x01,  // 前进
    MOTION_CMD_BACKWARD = 0x02, // 后退
    MOTION_CMD_LEFT = 0x03,     // 左转
    MOTION_CMD_RIGHT = 0x04,    // 右转
} motion_command_t;

/***** 运动控制串口 -end *****/


/***** 蓝牙遥控 -begin *****/

#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ble_remote.h"

static const char *BLE_TAG = "BLE_REMOTE";

/* ========================================================================
 *  键值定义（协议: [0x55][0x52][键值][0x5B]，键值为第3字节）
 *  根据你实际按键收到的值修改
 * ======================================================================== */
#define KEY_STOP     0xAA   // 示例：刚才收到的数据 55 52 11 5B，键值=0x11
#define KEY_UP        0x01
#define KEY_DOWN      0x02
#define KEY_LEFT      0x03
#define KEY_RIGHT     0x04

/***** 蓝牙遥控 -end *****/

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);

static const gc9a01_lcd_init_cmd_t gc9107_lcd_init_cmds[] = {
    //  {cmd, { data }, data_size, delay_ms}
    {0xfe, (uint8_t[]){0x00}, 0, 0},
    {0xef, (uint8_t[]){0x00}, 0, 0},
    {0xb0, (uint8_t[]){0xc0}, 1, 0},
    {0xb2, (uint8_t[]){0x2f}, 1, 0},
    {0xb3, (uint8_t[]){0x03}, 1, 0},
    {0xb6, (uint8_t[]){0x19}, 1, 0},
    {0xb7, (uint8_t[]){0x01}, 1, 0},
    {0xac, (uint8_t[]){0xcb}, 1, 0},
    {0xab, (uint8_t[]){0x0e}, 1, 0},
    {0xb4, (uint8_t[]){0x04}, 1, 0},
    {0xa8, (uint8_t[]){0x19}, 1, 0},
    {0xb8, (uint8_t[]){0x08}, 1, 0},
    {0xe8, (uint8_t[]){0x24}, 1, 0},
    {0xe9, (uint8_t[]){0x48}, 1, 0},
    {0xea, (uint8_t[]){0x22}, 1, 0},
    {0xc6, (uint8_t[]){0x30}, 1, 0},
    {0xc7, (uint8_t[]){0x18}, 1, 0},
    {0xf0,
    (uint8_t[]){0x1f, 0x28, 0x04, 0x3e, 0x2a, 0x2e, 0x20, 0x00, 0x0c, 0x06,
                0x00, 0x1c, 0x1f, 0x0f},
    14, 0},
    {0xf1,
    (uint8_t[]){0x00, 0x2d, 0x2f, 0x3c, 0x6f, 0x1c, 0x0b, 0x00, 0x00, 0x00,
                0x07, 0x0d, 0x11, 0x0f},
    14, 0},
};


class YcscEsp32S3Es8311Dhf : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button lamp_button_;
    bool lamp_on_ = false;
    Display* display_;
    light_mode_t light_mode_ = LIGHT_MODE_ALWAYS_ON;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
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
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void SetLamp(bool on) {
        lamp_on_ = on;
        gpio_set_level(LAMP_GPIO, on ? 1 : 0);
    }

    void ToggleLamp() {
        SetLamp(!lamp_on_);
    }

    void InitializeLamp() {
        // 灯（IO16）输出，默认关闭
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << LAMP_GPIO);
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));
        SetLamp(false);

        // 按键（IO48）：按一次亮，再按一次灭
        lamp_button_.OnClick([this]() {
            ToggleLamp();
        });
    }

    static void SendMotionFrame(motion_command_t cmd) {
        uint8_t frame[4] = {0x55, 0x5A, (uint8_t)cmd, 0x5B};
        uart_write_bytes(MOTION_UART_PORT, frame, sizeof(frame));
    }

    void InitializeMotionUart() {
        uart_config_t uart_cfg = {
            .baud_rate = 9600,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        // 仅发送，不安装接收缓冲区
        ESP_ERROR_CHECK(uart_driver_install(MOTION_UART_PORT, 256, 0, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(MOTION_UART_PORT, &uart_cfg));
        ESP_ERROR_CHECK(uart_set_pin(MOTION_UART_PORT, MOTION_UART_TX_PIN,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    }


    
    // 运动控制接口：发送一帧运动指令
    void SendMotionCommand(motion_command_t cmd) {
        SendMotionFrame(cmd);
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;


#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };
#elif defined(LCD_TYPE_GC9A01_160X160_SERIAL)
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io, &panel_config, &panel));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
#endif

        esp_lcd_panel_reset(panel);


        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef  LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
            {
                .text_font = &font_puhui_16_4,
                .icon_font = &font_awesome_16_4,
        #if CONFIG_USE_WECHAT_MESSAGE_STYLE
                .emoji_font = font_emoji_32_init(),
        #else
                .emoji_font = DISPLAY_HEIGHT >= 240 ? font_emoji_64_init() : font_emoji_32_init(),
        #endif
            });

    }


    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();

        // 运动控制 MCP 工具：供语音/AI 调用，action 参数控制前后左右和停止
        mcp_server.AddTool(
            "self.motion.control",
            "控制设备运动。action 可选值：forward(前进)、backward(后退)、left(左转)、right(右转)、stop(停止)。"
            "每次调用发送一帧对应指令。"
            "当用户要求跳舞或表演时，自己组合10个动作。",
            PropertyList({
                Property("action", kPropertyTypeString, std::string("stop"))
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string action = properties["action"].value<std::string>();

                motion_command_t cmd = MOTION_CMD_STOP;
                bool valid = true;

                if (action == "forward" || action == "前进") {
                    cmd = MOTION_CMD_FORWARD;
                } else if (action == "backward" || action == "后退") {
                    cmd = MOTION_CMD_BACKWARD;
                } else if (action == "left" || action == "左转") {
                    cmd = MOTION_CMD_LEFT;
                } else if (action == "right" || action == "右转") {
                    cmd = MOTION_CMD_RIGHT;
                } else if (action == "stop" || action == "停止" || action == "停下") {
                    cmd = MOTION_CMD_STOP;
                } else {
                    valid = false;
                }

                if (!valid) {
                    return std::string("无效的运动指令: ") + action;
                }

                SendMotionCommand(cmd);
                return std::string("运动指令已执行: ") + action;
            });

        // 灯光控制 MCP 工具：供语音/AI 控制灯（IO16）
        mcp_server.AddTool(
            "self.lamp.turn_on",
            "打开灯",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                SetLamp(true);
                return true;
            });

        mcp_server.AddTool(
            "self.lamp.turn_off",
            "关闭灯",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                SetLamp(false);
                return true;
            });

        mcp_server.AddTool(
            "self.lamp.toggle",
            "切换灯的亮灭状态",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ToggleLamp();
                return lamp_on_;
            });

        mcp_server.AddTool(
            "self.lamp.get_state",
            "获取灯的当前状态",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                return lamp_on_ ? "{\"power\": true}" : "{\"power\": false}";
            });
    }


    /***** 蓝牙遥控 -begin *****/

    /* ========================================================================
 *  按键回调：在这里判断键值，执行你的控制逻辑
 * ======================================================================== */
    static void on_key_event(const ble_remote_key_event_t *event)
    {
        ESP_LOGI(BLE_TAG, "key=0x%02X (raw len=%u)", (unsigned)event->key_code, event->raw_len);

        switch (event->key_code) {
        case KEY_STOP:
            ESP_LOGE(BLE_TAG, ">>> STOP 按下，执行停止");
            // 这里写你的控制代码，比如控制 GPIO、发消息等
            SendMotionFrame(MOTION_CMD_STOP);
            break;

        case KEY_UP:
            ESP_LOGE(BLE_TAG, ">>> UP 按下");
            SendMotionFrame(MOTION_CMD_FORWARD);
            break;

        case KEY_LEFT:
            ESP_LOGE(BLE_TAG, ">>> LEFT 按下");
            SendMotionFrame(MOTION_CMD_LEFT);
            break;

        case KEY_RIGHT:
            ESP_LOGE(BLE_TAG, ">>> RIGHT 按下");
            SendMotionFrame(MOTION_CMD_RIGHT);
            break;

        case KEY_DOWN:
            ESP_LOGE(BLE_TAG, ">>> DOWN 按下");
            SendMotionFrame(MOTION_CMD_BACKWARD);
            break;

        default:
            ESP_LOGW(BLE_TAG, ">>> 未知按键 key=0x%02X, raw:", (unsigned)event->key_code);
            for (uint16_t i = 0; i < event->raw_len; i++) {
                printf("%02X ", event->raw[i]);
            }
            printf("\n");
            break;
        }
    }


        /* ========================================================================
    *  连接事件回调（推荐）：连上 / 就绪 / 断开，适合做提示
    * ======================================================================== */
    static void on_conn_event(ble_remote_conn_event_t event)
    {
        switch (event) {
        case BLE_REMOTE_CONN_CONNECTED:
            ESP_LOGI(BLE_TAG, ">>> 遥控器已连接");
            // 这里做连接成功提示，比如亮灯、蜂鸣器等
            break;

        case BLE_REMOTE_CONN_READY:
            ESP_LOGI(BLE_TAG, ">>> 遥控器就绪，可以接收按键");
            // 这里做就绪提示
            break;

        case BLE_REMOTE_CONN_DISCONNECTED:
            ESP_LOGW(BLE_TAG, ">>> 遥控器已断开，正在自动重连...");
            // 这里做断开提示，比如闪灯、提示音等
            break;

        default:
            break;
        }
    }

    /* ========================================================================
    *  状态回调（可选）：细粒度状态变化
    * ======================================================================== */
    static void on_state_change(ble_remote_state_t state)
    {
        switch (state) {
        case BLE_REMOTE_STATE_IDLE:
            ESP_LOGW(BLE_TAG, "=== 已断开 ===");
            break;
        case BLE_REMOTE_STATE_SCANNING:
            ESP_LOGI(BLE_TAG, "=== 扫描中 ===");
            break;
        case BLE_REMOTE_STATE_CONNECTING:
            ESP_LOGI(BLE_TAG, "=== 连接中 ===");
            break;
        case BLE_REMOTE_STATE_CONNECTED:
            ESP_LOGI(BLE_TAG, "=== 已连接 ===");
            break;
        case BLE_REMOTE_STATE_NOTIFY_READY:
            ESP_LOGI(BLE_TAG, "=== 就绪，可以接收按键 ===");
            break;
        default:
            break;
        }
    }

    

    /***** 蓝牙遥控 -end *****/

public:
    YcscEsp32S3Es8311Dhf() : boot_button_(BOOT_BUTTON_GPIO), lamp_button_(LAMP_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeLamp();

        InitializeTools();
        InitializeMotionUart();
        SendMotionFrame(MOTION_CMD_FORWARD);
        GetBacklight()->RestoreBrightness();

        /***** 蓝牙遥控 -begin *****/
        ESP_LOGE(BLE_TAG, "=== BLE Remote App Start ===");
        /* 注册回调 */
        ble_remote_register_key_callback(on_key_event);
        ble_remote_register_conn_callback(on_conn_event);     // 连接事件（连上/就绪/断开）
        ble_remote_register_state_callback(on_state_change);  // 细粒度状态（可选）

        /* 初始化并自动连接 */
        esp_err_t ret = ble_remote_init();
        if (ret != ESP_OK) {
            ESP_LOGE(BLE_TAG, "ble_remote_init failed: %s", esp_err_to_name(ret));
            return;
        }
        /***** 蓝牙遥控 -end *****/
    }


    virtual AudioCodec* GetAudioCodec() override {
         static Es8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Led* GetLed() override {
        static GpioLed led(BUILTIN_LED_GPIO, 0);
        return &led;
    }

};

DECLARE_BOARD(YcscEsp32S3Es8311Dhf);
