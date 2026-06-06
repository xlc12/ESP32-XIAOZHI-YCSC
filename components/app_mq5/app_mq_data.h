#ifndef APP_MQ_DATA
#define APP_MQ_DATA

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// 命令号定义
#define CMD_QUERY_DEVICE_INFO     101  // 查询设备信息
#define CMD_REPORT_DEVICE_STATUS  102  // 上报设备状态（设备主动）
#define CMD_DEVICE_ONLINE         103  // 设备上线通知
#define CMD_WIFI_CONFIG           104  // WiFi配置
#define CMD_OTA_UPGRADE           105  // OTA升级
#define CMD_OTA_PROGRESS          106  // OTA进度上报（设备主动）
#define CMD_TIMER_SET             107  // 定时器设置
#define CMD_TIMER_REACH           108  // 定时器到达通知
#define CMD_DEVICE_CONTROL        109  // 设备控制
#define CMD_GET_DEVICE_LIST       110  // 获取设备列表
#define CMD_SET_DEVICE_NICKNAME   111  // 设置设备昵称和称呼




// 时间戳类型选择宏
#define USE_ESP_TIMER_TIMESTAMP   0   // 1:使用ESP定时器时间戳 0:使用标准UNIX时间戳

#if USE_ESP_TIMER_TIMESTAMP
    // 使用ESP定时器时间戳 (单位:秒)
    #define GET_CURRENT_TIMESTAMP() ((uint32_t)(esp_timer_get_time() / 1000000))
#else
    // 使用标准UNIX时间戳 (从1970-01-01开始的秒数)
    long app_get_timestamp();
    #define GET_CURRENT_TIMESTAMP() (app_get_timestamp()-8*60*60)//;appl 环境时间戳-8小时
#endif

// 应答状态枚举
typedef enum {
    RESPONSE_STATUS_SUCCESS = 0,
    RESPONSE_STATUS_FAILED = -1,
    RESPONSE_STATUS_ERROR = -2
} response_status_t;

// 电量状态枚举
typedef enum {
    POWER_STATE_LOW_BATTERY = 0,
    POWER_STATE_CHARGING,
    POWER_STATE_CHARGING_COMPLETE
} power_state_t;

// OTA状态枚举
typedef enum {
    OTA_STATUS_START = 0,
    OTA_STATUS_DOWNLOADING,
    OTA_STATUS_SUCCESS,
    OTA_STATUS_FAILED
} ota_status_t;

// 控制类型枚举
typedef enum {
    CONTROL_TYPE_UNKNOWN = 0,
    CONTROL_TYPE_VOLUME,      // 音量控制   
    CONTROL_TYPE_BRIGHTNESS,   // 亮度控制  
    CONTROL_TYPE_COMPASS_ANGLE, // 指南针角度标定
    CONTROL_TYPE_CALIBRATION_MODE //校准模式
} control_type_t;

// WiFi配置结构体
typedef struct {
    char mac[32];
    char wifi_ssid[64];
    char wifi_pwd[64];
} wifi_config_cmd_t;

// OTA升级结构体
typedef struct {
    char version[32];
    char upgrade_url[256];
    int is_silence;
} ota_upgrade_cmd_t;

// 定时器项结构体
typedef struct {
    char id[32];           // 定时器ID
    uint32_t timestamp;    // 时间戳
    char task[512];////[128];        // 任务描述
} timer_item_t;

// 定时器设置结构体
typedef struct {
    timer_item_t *timers;  // 定时器数组
    size_t timer_count;    // 定时器数量
} timer_set_cmd_t;

// 添加设置昵称结构体
typedef struct {
    char mac[32];              // 设备mac地址
    char my_name[32];          // 对我的称呼
    char tangma_name[32];      // 设备昵称
} device_nickname_cmd_t;

// 定时器删除结构体
// typedef struct {
//     char timer_id[32];
// } timer_delete_cmd_t;

// 设备查询结构体
typedef struct {
    char mac[32];
} device_query_cmd_t;

// 设备状态结构体
typedef struct {
    power_state_t power_state;
    int mute_state;
    int dormant_state;
    char version[16];
    char wifi_ssid[32];
    int device_type;
    int moto_step_test;
    int brightness;           // 亮度 0-100
    int volume;              // 音量 0-100
    int compass_angle;       // 指南针角度 0-360  
    int calibration_mode;    // 校准模式 0-退出 1-进入
    int current_position;     // 当前位置 0-100
    char current_orientation[16]; // 当前方向
    int current_motion_status; // 当前运动状态
    int battery;             // 电池电量 0-100
} device_status_t;

// 设备控制命令结构体
typedef struct {
    char mac[32];             // 设备mac地址
    control_type_t control_type; // 控制类型
    int control_value;        // 控制值 0-100
} device_control_cmd_t;

// 命令头结构体
typedef struct {
    int cmd;                  // 命令号
    int serial;               // 序列号
    char uuid[32];            // UUID
    char keytype[8];          // 密钥类型
    char vendor[32];          // 厂商
    char* payload;            // 负载数据（需要释放）
} command_header_t;

// 统一命令结构体
typedef struct {
    command_header_t header;
    union {
        device_query_cmd_t query_cmd;
        device_control_cmd_t control_cmd;
        wifi_config_cmd_t wifi_cmd;
        ota_upgrade_cmd_t ota_cmd;
        timer_set_cmd_t timer_set_cmd;
        //timer_delete_cmd_t timer_delete_cmd;
    };
} unified_command_t;

// 命令处理回调函数类型定义
typedef void (*mqtt_command_callback_t)(int cmd, const command_header_t* header, const void* command_data);

// 支持两种payload格式的宏
#define SUPPORT_DUAL_PAYLOAD_FORMAT 0  // 1:str 0:obj

// ================== 核心函数声明 ==================

// 初始化MQTT数据模块
void app_mq_data_init(void);

// 设置命令处理回调函数
void app_mq_data_set_command_callback(mqtt_command_callback_t callback);

// 解析并处理MQTT命令
int parse_and_handle_device_command(const char* json_str);

// 解析完整命令函数
int parse_device_command(const char* json_str, command_header_t* header, void* command_data);

// 释放命令头内存
void free_command_header(command_header_t* header);

// ================== 设备状态相关 ==================

// 获取设备状态
device_status_t *get_device_status(void);

// 设备状态上报函数
char* report_device_status_to_json(const device_status_t* status);

// 上报设备状态
void report_device_status(void);

// 上报设备信息（CMD 101响应）
//void report_device_info(void);

// ================== 应答相关函数 ==================

// 
char* create_108_timer_reach_response_json(int original_cmd, int original_serial);
char* create_response_json(int original_cmd, int original_serial, response_status_t status, const char* message);

// 便捷应答函数
char* create_success_response(int original_cmd, int original_serial);
char* create_failed_response(int original_cmd, int original_serial, const char* message);
char* create_error_response(int original_cmd, int original_serial, const char* message);

// 发送应答
void send_command_response(int original_cmd, int original_serial, bool success, const char* message);

// ================== 主动上报函数 ==================

// 上报OTA进度
// 在 app_mq_data.h 中修改函数声明
// 上报OTA进度
void report_ota_progress(ota_status_t status, int progress, const char* version);
void report_ota_start(const char* version);
void report_ota_downloading(int progress, const char* version);
void report_ota_success(const char* version);
void report_ota_failed(const char* version);

// 上报设备上线
void report_device_online(void);

#ifdef __cplusplus
}
#endif

#endif
