#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "time.h"
#include "cJSON.h"
#include "app_mq_data.h"
#include "app_mqtt5.h"

static const char *TAG = "app_mq_data";

// 设备状态实例
device_status_t g_device_status = {
    .power_state = POWER_STATE_CHARGING_COMPLETE,
    .mute_state = 0,
    .dormant_state = 0,
    .version = VER_MQTT5,
    .wifi_ssid = "MyWiFi",
    .device_type = 1,
    .moto_step_test = 0,
    .brightness = 60,
    .volume = 50,
    .current_position = 50,
    .current_orientation = "西北方向",
    .current_motion_status = 0,
    .battery = 75
};

// 全局回调函数指针
static mqtt_command_callback_t g_command_callback = NULL;

// ================== 初始化函数 ==================

void app_mq_data_init(void)
{
    ESP_LOGI(TAG, "MQTT data module initialized");
}

void app_mq_data_set_command_callback(mqtt_command_callback_t callback)
{
    g_command_callback = callback;
    ESP_LOGI(TAG, "Command callback registered");
}

// ================== MAC地址检查函数 ==================

static bool check_mac_address(const char* current_device_id, const char* mac, int cmd, int serial)
{
    if (mac == NULL || strcmp(mac, current_device_id) != 0) {
        ESP_LOGW(TAG, "MAC address mismatch: expected %s, got %s", 
                current_device_id, mac ? mac : "NULL");
        char* failed_response = create_failed_response(cmd, serial, "MAC address mismatch");
        if (failed_response) {
            app_mqtt5_dev2app(failed_response);
            free(failed_response);
        }
        return false;
    }
    return true;
}

// ================== 命令解析函数 ==================

int parse_and_handle_device_command(const char* json_str)
{
    if (json_str == NULL || g_command_callback == NULL) {
        ESP_LOGE(TAG, "Invalid parameters or callback not set");
        return -1;
    }

    command_header_t header;
    
    // 为不同类型的命令分配内存
    void* command_data = NULL;
    size_t data_size = 0;
    
    // 根据命令类型分配相应大小的内存
    cJSON *root = cJSON_Parse(json_str);
    if (root) {
        cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
        if (cJSON_IsNumber(cmd_item)) {
            int cmd = cmd_item->valueint;
            switch (cmd) {
                case CMD_QUERY_DEVICE_INFO:
                case CMD_DEVICE_ONLINE:
                    data_size = sizeof(device_query_cmd_t);
                    break;
                case CMD_WIFI_CONFIG:
                    data_size = sizeof(wifi_config_cmd_t);
                    break;
                case CMD_OTA_UPGRADE:
                    data_size = sizeof(ota_upgrade_cmd_t);
                    break;
                case CMD_TIMER_SET:
                    data_size = sizeof(timer_set_cmd_t);
                    break;
                // case CMD_TIMER_DELETE:
                //     data_size = sizeof(timer_delete_cmd_t);
                //     break;
                case CMD_DEVICE_CONTROL:
                    data_size = sizeof(device_control_cmd_t);
                    break;

                case CMD_GET_DEVICE_LIST:  // 新增110命令
                    // 110命令不需要payload数据，但为了统一处理，分配最小内存
                    data_size = sizeof(device_query_cmd_t);
                    break;
                    
                case CMD_SET_DEVICE_NICKNAME:
                    data_size = sizeof(device_nickname_cmd_t);
                    break;    

                default:
                    ESP_LOGW(TAG, "Unsupported command type: %d", cmd);
                    break;
            }
        }
        cJSON_Delete(root);
    }
    
    if (data_size > 0) {
        command_data = malloc(data_size);
        if (command_data) {
            memset(command_data, 0, data_size);
        }
    }
    
    if (parse_device_command(json_str, &header, command_data) == 0) {
        g_command_callback(header.cmd, &header, command_data);
        free_command_header(&header);
        if (command_data) {
            // 特殊处理定时器设置命令的内存释放
            if (header.cmd == CMD_TIMER_SET) {
                timer_set_cmd_t* timer_cmd = (timer_set_cmd_t*)command_data;
                if (timer_cmd->timers) {
                    free(timer_cmd->timers);
                }
            }
            free(command_data);
        }
        return 0;
    } else {
        ESP_LOGE(TAG, "Failed to parse device command");
        if (command_data) {
            // 特殊处理定时器设置命令的内存释放
            if (command_data) {
                timer_set_cmd_t* timer_cmd = (timer_set_cmd_t*)command_data;
                if (timer_cmd && timer_cmd->timers) {
                    free(timer_cmd->timers);
                }
            }
            free(command_data);
        }
        return -1;
    }
}

// ================== 具体命令解析函数 ==================

// 解析设备查询命令 (CMD 101)
static int parse_query_payload(cJSON* payload_obj, device_query_cmd_t* cmd)
{
    if (payload_obj == NULL || cmd == NULL) {
        return -1;
    }

    cJSON *mac_item = cJSON_GetObjectItem(payload_obj, "mac");
    if (cJSON_IsString(mac_item) && (mac_item->valuestring != NULL)) {
        strncpy(cmd->mac, mac_item->valuestring, sizeof(cmd->mac) - 1);
        return 0;
    }
    
    ESP_LOGE(TAG, "Missing or invalid 'mac' field in query payload");
    return -1;
}

// 解析WiFi配置命令 (CMD 104)
static int parse_wifi_config_payload(cJSON* payload_obj, wifi_config_cmd_t* cmd)
{
    if (payload_obj == NULL || cmd == NULL) {
        return -1;
    }

    int parse_success = 1;

    cJSON *mac_item = cJSON_GetObjectItem(payload_obj, "mac");
    cJSON *ssid_item = cJSON_GetObjectItem(payload_obj, "wifiSsid");
    cJSON *pwd_item = cJSON_GetObjectItem(payload_obj, "wifiPwd");

    if (cJSON_IsString(mac_item) && (mac_item->valuestring != NULL)) {
        strncpy(cmd->mac, mac_item->valuestring, sizeof(cmd->mac) - 1);
    } else {
        parse_success = 0;
    }

    if (cJSON_IsString(ssid_item) && (ssid_item->valuestring != NULL)) {
        strncpy(cmd->wifi_ssid, ssid_item->valuestring, sizeof(cmd->wifi_ssid) - 1);
    } else {
        parse_success = 0;
    }

    if (cJSON_IsString(pwd_item) && (pwd_item->valuestring != NULL)) {
        strncpy(cmd->wifi_pwd, pwd_item->valuestring, sizeof(cmd->wifi_pwd) - 1);
    } else {
        parse_success = 0;
    }

    return parse_success ? 0 : -1;
}

// 解析OTA升级命令 (CMD 105)
static int parse_ota_upgrade_payload(cJSON* payload_obj, ota_upgrade_cmd_t* cmd)
{
    if (payload_obj == NULL || cmd == NULL) {
        return -1;
    }

    int parse_success = 1;

    cJSON *version_item = cJSON_GetObjectItem(payload_obj, "version");
    cJSON *url_item = cJSON_GetObjectItem(payload_obj, "upgradeUrl");
    cJSON *silence_item = cJSON_GetObjectItem(payload_obj, "isSilence");

    if (cJSON_IsString(version_item) && (version_item->valuestring != NULL)) {
        strncpy(cmd->version, version_item->valuestring, sizeof(cmd->version) - 1);
    } else {
        parse_success = 0;
    }

    if (cJSON_IsString(url_item) && (url_item->valuestring != NULL)) {
        strncpy(cmd->upgrade_url, url_item->valuestring, sizeof(cmd->upgrade_url) - 1);
    } else {
        parse_success = 0;
    }

    if (cJSON_IsNumber(silence_item)) {
        cmd->is_silence = silence_item->valueint;
    } else {
        parse_success = 0;
    }

    return parse_success ? 0 : -1;
}

// 解析定时器设置命令 (CMD 107) - 新版本
static int parse_timer_set_payload(cJSON* payload_obj, timer_set_cmd_t* cmd)
{
    if (payload_obj == NULL || cmd == NULL) {
        return -1;
    }

    memset(cmd, 0, sizeof(timer_set_cmd_t));

    cJSON *timers_array = cJSON_GetObjectItem(payload_obj, "timers");
    if (!cJSON_IsArray(timers_array)) {
        ESP_LOGE(TAG, "Missing or invalid 'timers' array in payload");
        return -1;
    }

    int timer_count = cJSON_GetArraySize(timers_array);
    if (timer_count == 0) {
        ESP_LOGW(TAG, "Empty timers array");
        return 0; // 空数组不算错误
    }

    // 分配定时器数组内存
    cmd->timers = (timer_item_t*)malloc(timer_count * sizeof(timer_item_t));
    if (cmd->timers == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for timers");
        return -1;
    }
    memset(cmd->timers, 0, timer_count * sizeof(timer_item_t));
    cmd->timer_count = timer_count;

    // 解析每个定时器项
    for (int i = 0; i < timer_count; i++) {
        cJSON *timer_item = cJSON_GetArrayItem(timers_array, i);
        if (!cJSON_IsObject(timer_item)) {
            ESP_LOGE(TAG, "Invalid timer item at index %d", i);
            continue;
        }

        timer_item_t *timer = &cmd->timers[i];

        // 解析id
        cJSON *id_item = cJSON_GetObjectItem(timer_item, "id");
        if (cJSON_IsString(id_item) && (id_item->valuestring != NULL)) {
            strncpy(timer->id, id_item->valuestring, sizeof(timer->id) - 1);
        } else {
            ESP_LOGE(TAG, "Missing or invalid 'id' in timer item %d", i);
        }

        // 解析timestamp
        cJSON *timestamp_item = cJSON_GetObjectItem(timer_item, "timestamp");
        if (cJSON_IsNumber(timestamp_item)) {
            timer->timestamp = (uint32_t)timestamp_item->valuedouble;
        } else {
            ESP_LOGE(TAG, "Missing or invalid 'timestamp' in timer item %d", i);
        }

        // 解析task
        cJSON *task_item = cJSON_GetObjectItem(timer_item, "task");
        if (cJSON_IsString(task_item) && (task_item->valuestring != NULL)) {
            strncpy(timer->task, task_item->valuestring, sizeof(timer->task) - 1);
        } else {
            ESP_LOGE(TAG, "Missing or invalid 'task' in timer item %d", i);
        }

        ESP_LOGI(TAG, "Parsed timer: id=%s, timestamp=%lu, task[%d]=%s\n", timer->id, timer->timestamp, strlen(timer->task),timer->task);
        //printf("Parsed timer: id=%s, timestamp=%lu, task[%d]=%s\n", timer->id, timer->timestamp, strlen(timer->task),timer->task);
    }

    return 0;
}

// 解析定时器删除命令 (CMD 108)
// static int parse_timer_delete_payload(cJSON* payload_obj, timer_delete_cmd_t* cmd)
// {
//     if (payload_obj == NULL || cmd == NULL) {
//         return -1;
//     }

//     cJSON *timer_id_item = cJSON_GetObjectItem(payload_obj, "timerId");
//     if (cJSON_IsString(timer_id_item) && (timer_id_item->valuestring != NULL)) {
//         strncpy(cmd->timer_id, timer_id_item->valuestring, sizeof(cmd->timer_id) - 1);
//         return 0;
//     }
    
//     ESP_LOGE(TAG, "Missing or invalid 'timerId' field in timer delete payload");
//     return -1;
// }

// 解析控制命令payload
static int parse_control_payload(cJSON* payload_obj, device_control_cmd_t* cmd)
{
    if (payload_obj == NULL || cmd == NULL) {
        return -1;
    }

    int parse_success = 1;

    // 解析mac地址
    cJSON *mac_item = cJSON_GetObjectItem(payload_obj, "mac");
    if (cJSON_IsString(mac_item) && (mac_item->valuestring != NULL)) {
        strncpy(cmd->mac, mac_item->valuestring, sizeof(cmd->mac) - 1);
    } else {
        ESP_LOGE(TAG, "Missing or invalid 'mac' field in payload");
        parse_success = 0;
    }

    // 解析控制类型
    cJSON *control_type_item = cJSON_GetObjectItem(payload_obj, "controlType");
    if (cJSON_IsString(control_type_item) && (control_type_item->valuestring != NULL)) {
        const char* control_type_str = control_type_item->valuestring;
        
        if (strcmp(control_type_str, "volume") == 0) {
            cmd->control_type = CONTROL_TYPE_VOLUME;
        } else if (strcmp(control_type_str, "brightness") == 0) {
            cmd->control_type = CONTROL_TYPE_BRIGHTNESS;
        } else if (strcmp(control_type_str, "compassAngle") == 0){
            cmd->control_type = CONTROL_TYPE_COMPASS_ANGLE;
        } else if (strcmp(control_type_str, "calibrationMode") == 0){
            cmd->control_type = CONTROL_TYPE_CALIBRATION_MODE;
        } else {
            ESP_LOGE(TAG, "Unknown control type: %s", control_type_str);
            parse_success = 0;
        }
    } else {
        ESP_LOGE(TAG, "Missing or invalid 'controlType' field in payload");
        parse_success = 0;
    }

    // 解析控制值（接受 controlValue 或 controlvalue 两种大小写）
    cJSON *control_value_item = cJSON_GetObjectItem(payload_obj, "controlValue");
    if (control_value_item == NULL) {
        control_value_item = cJSON_GetObjectItem(payload_obj, "controlvalue");
    }
    if (cJSON_IsNumber(control_value_item)) {
        int value = control_value_item->valueint;
        // 根据 control_type 调整合法范围：指南针允许 0-360，其他保留 0-100
        if (cmd->control_type == CONTROL_TYPE_COMPASS_ANGLE) {
            if (value >= 0 && value <= 360) {
                cmd->control_value = value;
            } else {
                ESP_LOGE(TAG, "Compass angle out of range: %d", value);
                parse_success = 0;
            }
        }else if (cmd->control_type == CONTROL_TYPE_CALIBRATION_MODE) {
            if (value == 0 || value == 1) {
                cmd->control_value = value;
            } else {
                ESP_LOGE(TAG, "Calibration mode out of range: %d", value);
                parse_success = 0;
            }
        }
        else {
            if (value >= 0 && value <= 100) {
                cmd->control_value = value;
            } else {
                ESP_LOGE(TAG, "Control value out of range: %d", value);
                parse_success = 0;
            }
        }
    } else {
        ESP_LOGE(TAG, "Missing or invalid 'controlValue/controlvalue' field in payload");
        parse_success = 0;
    }

    return parse_success ? 0 : -1;
}

// 解析设置昵称命令 (CMD 111)
static int parse_nickname_payload(cJSON* payload_obj, device_nickname_cmd_t* cmd)
{
    if (payload_obj == NULL || cmd == NULL) {
        return -1;
    }

    int parse_success = 1;

    cJSON *mac_item = cJSON_GetObjectItem(payload_obj, "mac");
    cJSON *my_name_item = cJSON_GetObjectItem(payload_obj, "myName");
    cJSON *tangma_name_item = cJSON_GetObjectItem(payload_obj, "tangmaName");

    if (cJSON_IsString(mac_item) && (mac_item->valuestring != NULL)) {
        strncpy(cmd->mac, mac_item->valuestring, sizeof(cmd->mac) - 1);
    } else {
        parse_success = 0;
    }

    if (cJSON_IsString(my_name_item) && (my_name_item->valuestring != NULL)) {
        strncpy(cmd->my_name, my_name_item->valuestring, sizeof(cmd->my_name) - 1);
    } else {
        parse_success = 0;
    }

    if (cJSON_IsString(tangma_name_item) && (tangma_name_item->valuestring != NULL)) {
        strncpy(cmd->tangma_name, tangma_name_item->valuestring, sizeof(cmd->tangma_name) - 1);
    } else {
        parse_success = 0;
    }

    return parse_success ? 0 : -1;
}

// ================== 主解析函数 ==================

int parse_device_command(const char* json_str, command_header_t* header, void* command_data)
{
    if (json_str == NULL || header == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return -1;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            ESP_LOGE(TAG, "JSON parse error before: %s", error_ptr);
        }
        return -1;
    }

    memset(header, 0, sizeof(command_header_t));

    int parse_success = 1;

    // 解析命令头字段
    cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
    cJSON *serial_item = cJSON_GetObjectItem(root, "serial");
    cJSON *uuid_item = cJSON_GetObjectItem(root, "uuid");
    cJSON *keytype_item = cJSON_GetObjectItem(root, "keytype");
    cJSON *vendor_item = cJSON_GetObjectItem(root, "vendor");
    cJSON *payload_item = cJSON_GetObjectItem(root, "payload");

    if (cJSON_IsNumber(cmd_item)) {
        header->cmd = cmd_item->valueint;
    } else {
        ESP_LOGE(TAG, "Missing or invalid 'cmd' field");
        parse_success = 0;
    }

    if (cJSON_IsNumber(serial_item)) {
        header->serial = serial_item->valueint;
    } else {
        ESP_LOGE(TAG, "Missing or invalid 'serial' field");
        parse_success = 0;
    }

    if (cJSON_IsString(uuid_item) && (uuid_item->valuestring != NULL)) {
        strncpy(header->uuid, uuid_item->valuestring, sizeof(header->uuid) - 1);
    } else {
        ESP_LOGE(TAG, "Missing or invalid 'uuid' field");
        parse_success = 0;
    }

    if (cJSON_IsString(keytype_item) && (keytype_item->valuestring != NULL)) {
        strncpy(header->keytype, keytype_item->valuestring, sizeof(header->keytype) - 1);
    } else {
        ESP_LOGE(TAG, "Missing or invalid 'keytype' field");
        parse_success = 0;
    }

    if (cJSON_IsString(vendor_item) && (vendor_item->valuestring != NULL)) {
        strncpy(header->vendor, vendor_item->valuestring, sizeof(header->vendor) - 1);
    } else {
        ESP_LOGE(TAG, "Missing or invalid 'vendor' field");
        parse_success = 0;
    }

    // 解析payload
    if (payload_item != NULL && cJSON_IsObject(payload_item)) {
        int payload_parse_result = -1;
        
        // 根据命令类型调用不同的解析函数
        switch (header->cmd) {
            case CMD_QUERY_DEVICE_INFO:
            case CMD_DEVICE_ONLINE:
                if (command_data) {
                    payload_parse_result = parse_query_payload(payload_item, (device_query_cmd_t*)command_data);
                }
                break;
                
            case CMD_WIFI_CONFIG:
                if (command_data) {
                    payload_parse_result = parse_wifi_config_payload(payload_item, (wifi_config_cmd_t*)command_data);
                }
                break;
                
            case CMD_OTA_UPGRADE:
                if (command_data) {
                    payload_parse_result = parse_ota_upgrade_payload(payload_item, (ota_upgrade_cmd_t*)command_data);
                }
                break;
                
            case CMD_TIMER_SET:
                if (command_data) {
                    payload_parse_result = parse_timer_set_payload(payload_item, (timer_set_cmd_t*)command_data);
                }
                break;
                
            // case CMD_TIMER_DELETE:
            //     if (command_data) {
            //         payload_parse_result = parse_timer_delete_payload(payload_item, (timer_delete_cmd_t*)command_data);
            //     }
            //     break;
                
            case CMD_DEVICE_CONTROL:
                if (command_data) {
                    payload_parse_result = parse_control_payload(payload_item, (device_control_cmd_t*)command_data);
                }
                break;

            case CMD_GET_DEVICE_LIST:  // 新增110命令
                // 110命令的payload为空，不需要特殊解析
                payload_parse_result = 0;
                ESP_LOGI(TAG, "CMD_110 received, empty payload");
                break;

            case CMD_SET_DEVICE_NICKNAME:
                if (command_data) {
                    payload_parse_result = parse_nickname_payload(payload_item, (device_nickname_cmd_t*)command_data);
                }
                break;

            default:
                ESP_LOGW(TAG, "Unhandled command type in payload parsing: %d", header->cmd);
                break;
        }
        
        if (payload_parse_result != 0) {
            ESP_LOGE(TAG, "Failed to parse payload for command %d", header->cmd);
            parse_success = 0;
        }
        
        // 保存payload字符串
        char *payload_str = cJSON_PrintUnformatted(payload_item);
        if (payload_str != NULL) {
            header->payload = payload_str;
        }
    } else {
        ESP_LOGE(TAG, "Missing or invalid 'payload' field");
        parse_success = 0;
    }

    cJSON_Delete(root);

    if (!parse_success) {
        free_command_header(header);
        return -1;
    }

    ESP_LOGI(TAG, "Parsed command: cmd=%d, serial=%d", header->cmd, header->serial);
    return 0;
}

// ================== 内存管理函数 ==================

void free_command_header(command_header_t* header)
{
    if (header && header->payload) {
        free(header->payload);
        header->payload = NULL;
    }
}

// ================== 设备状态相关函数 ==================

device_status_t *get_device_status(void)
{
    return &g_device_status;
}

char* report_device_status_to_json(const device_status_t* status)
{
    if (status == NULL) {
        ESP_LOGE(TAG, "status is NULL");
        return NULL;
    }

    cJSON *status_root = cJSON_CreateObject();
    if (status_root == NULL) {
        ESP_LOGE(TAG, "Failed to create status JSON object");
        return NULL;
    }

    const char* power_state_str = "unknown";
    switch (status->power_state) {
        case POWER_STATE_LOW_BATTERY: power_state_str = "low_battery"; break;
        case POWER_STATE_CHARGING: power_state_str = "charging"; break;
        case POWER_STATE_CHARGING_COMPLETE: power_state_str = "charging_complete"; break;
    }

    // 添加所有状态字段
    cJSON_AddStringToObject(status_root, "powerState", power_state_str);
    cJSON_AddNumberToObject(status_root, "muteState", status->mute_state);
    cJSON_AddNumberToObject(status_root, "dormantState", status->dormant_state);
    cJSON_AddStringToObject(status_root, "mac", app_mqtt5_get_device_id());
    cJSON_AddNumberToObject(status_root, "deviceType", status->device_type);
    cJSON_AddStringToObject(status_root, "version", status->version);
    cJSON_AddStringToObject(status_root, "wifiSsid", status->wifi_ssid);
    cJSON_AddNumberToObject(status_root, "motoStepTest", status->moto_step_test);
    cJSON_AddNumberToObject(status_root, "brightness", status->brightness);
    cJSON_AddNumberToObject(status_root, "volume", status->volume);
    cJSON_AddNumberToObject(status_root, "compassAngle", status->compass_angle);
    cJSON_AddNumberToObject(status_root, "calibrationMode", status->calibration_mode);      
    cJSON_AddNumberToObject(status_root, "currentPosition", status->current_position);
    cJSON_AddStringToObject(status_root, "currentOrientation", status->current_orientation);
    cJSON_AddNumberToObject(status_root, "currentMotionStatus", status->current_motion_status);
    cJSON_AddNumberToObject(status_root, "battery", status->battery);

    char *status_json_str = cJSON_PrintUnformatted(status_root);
    cJSON_Delete(status_root);
    
    if (status_json_str == NULL) {
        ESP_LOGE(TAG, "Failed to generate status JSON string");
        return NULL;
    }

    cJSON *command_root = cJSON_CreateObject();
    if (command_root == NULL) {
        free(status_json_str);
        return NULL;
    }

    cJSON_AddNumberToObject(command_root, "cmd", CMD_REPORT_DEVICE_STATUS);
    cJSON_AddNumberToObject(command_root, "serial", (int)GET_CURRENT_TIMESTAMP());
    cJSON_AddStringToObject(command_root, "uuid", app_mqtt5_get_device_id());
    cJSON_AddStringToObject(command_root, "keytype", "NK");
    cJSON_AddStringToObject(command_root, "vendor", "starlinker");

#if SUPPORT_DUAL_PAYLOAD_FORMAT
    cJSON_AddStringToObject(command_root, "payload", status_json_str);
    free(status_json_str);
#else
    cJSON *payload_obj = cJSON_Parse(status_json_str);
    free(status_json_str);
    if (payload_obj != NULL) {
        cJSON_AddItemToObject(command_root, "payload", payload_obj);
    } else {
        cJSON_Delete(command_root);
        return NULL;
    }
#endif

    char *command_json_str = cJSON_PrintUnformatted(command_root);
    cJSON_Delete(command_root);
    
    if (command_json_str == NULL) {
        ESP_LOGE(TAG, "Failed to generate command JSON string");
        return NULL;
    }

    ESP_LOGD(TAG, "Generated device status report");
    return command_json_str;
}

void report_device_status(void)
{
    char* json_str = report_device_status_to_json(get_device_status());
    if (json_str) {
        app_mqtt5_dev2app(json_str);
        free(json_str);
        ESP_LOGI(TAG, "Device status reported (CMD 102)");
    } else {
        ESP_LOGE(TAG, "Failed to report device status");
    }
}

// ================== 设备信息上报函数 ==================
#if 0
void report_device_info(void)
{
    cJSON *command_root = cJSON_CreateObject();
    if (command_root == NULL) {
        return;
    }

    device_status_t* status = get_device_status();
    
    cJSON_AddNumberToObject(command_root, "cmd", CMD_RESPONSE);
    cJSON_AddNumberToObject(command_root, "serial", (int)GET_CURRENT_TIMESTAMP());
    cJSON_AddStringToObject(command_root, "uuid", app_mqtt5_get_device_id());
    cJSON_AddStringToObject(command_root, "keytype", "NK");
    cJSON_AddStringToObject(command_root, "vendor", "starlinker");

    cJSON *payload_obj = cJSON_CreateObject();
    if (payload_obj) {
        cJSON_AddStringToObject(payload_obj, "deviceId", app_mqtt5_get_device_id());
        cJSON_AddStringToObject(payload_obj, "version", status->version);
        cJSON_AddNumberToObject(payload_obj, "deviceType", status->device_type);
        cJSON_AddStringToObject(payload_obj, "wifiSsid", status->wifi_ssid);
        cJSON_AddNumberToObject(payload_obj, "battery", status->battery);
        
        cJSON_AddItemToObject(command_root, "payload", payload_obj);
    }

    char *command_json_str = cJSON_PrintUnformatted(command_root);
    cJSON_Delete(command_root);
    
    if (command_json_str) {
        app_mqtt5_dev2app(command_json_str);
        free(command_json_str);
        ESP_LOGI(TAG, "Device info reported");
    }
}
#endif
// ================== 应答相关函数 ==================
char* create_108_timer_reach_response_json(int original_cmd, int original_serial)
{
    cJSON *payload_obj = cJSON_CreateObject();
    if (payload_obj == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(payload_obj, "code", app_mqtt5_get_device_id());

    cJSON *command_root = cJSON_CreateObject();
    if (command_root == NULL) {
        cJSON_Delete(payload_obj);
        return NULL;
    }

    cJSON_AddNumberToObject(command_root, "cmd", original_cmd);
    cJSON_AddNumberToObject(command_root, "serial", (int)GET_CURRENT_TIMESTAMP());
    cJSON_AddStringToObject(command_root, "uuid", app_mqtt5_get_device_id());
    cJSON_AddStringToObject(command_root, "keytype", "NK");
    cJSON_AddStringToObject(command_root, "vendor", "starlinker");

#if SUPPORT_DUAL_PAYLOAD_FORMAT
    char *payload_str = cJSON_PrintUnformatted(payload_obj);
    if (payload_str != NULL) {
        cJSON_AddStringToObject(command_root, "payload", payload_str);
        free(payload_str);
    }
    cJSON_Delete(payload_obj);
#else
    cJSON_AddItemToObject(command_root, "payload", payload_obj);
#endif

    char *command_json_str = cJSON_PrintUnformatted(command_root);
    cJSON_Delete(command_root);
    
    if (command_json_str == NULL) {
        ESP_LOGE(TAG, "Failed to generate response JSON string");
        return NULL;
    }

    //ESP_LOGD(TAG, "Generated response: state=%d, msg=%s", status, message);
    return command_json_str;
}

char* create_response_json(int original_cmd, int original_serial, response_status_t status, const char* message)
{
    cJSON *payload_obj = cJSON_CreateObject();
    if (payload_obj == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(payload_obj, "state", status);
    cJSON_AddStringToObject(payload_obj, "msg", message ? message : "success");

    cJSON *command_root = cJSON_CreateObject();
    if (command_root == NULL) {
        cJSON_Delete(payload_obj);
        return NULL;
    }

    cJSON_AddNumberToObject(command_root, "cmd", original_cmd);
    cJSON_AddNumberToObject(command_root, "serial", (int)GET_CURRENT_TIMESTAMP());
    cJSON_AddStringToObject(command_root, "uuid", app_mqtt5_get_device_id());
    cJSON_AddStringToObject(command_root, "keytype", "NK");
    cJSON_AddStringToObject(command_root, "vendor", "starlinker");

#if SUPPORT_DUAL_PAYLOAD_FORMAT
    char *payload_str = cJSON_PrintUnformatted(payload_obj);
    if (payload_str != NULL) {
        cJSON_AddStringToObject(command_root, "payload", payload_str);
        free(payload_str);
    }
    cJSON_Delete(payload_obj);
#else
    cJSON_AddItemToObject(command_root, "payload", payload_obj);
#endif

    char *command_json_str = cJSON_PrintUnformatted(command_root);
    cJSON_Delete(command_root);
    
    if (command_json_str == NULL) {
        ESP_LOGE(TAG, "Failed to generate response JSON string");
        return NULL;
    }

    ESP_LOGD(TAG, "Generated response: state=%d, msg=%s", status, message);
    return command_json_str;
}

char* create_success_response(int original_cmd, int original_serial)
{
    return create_response_json(original_cmd, original_serial, RESPONSE_STATUS_SUCCESS, "success");
}

char* create_failed_response(int original_cmd, int original_serial, const char* message)
{
    return create_response_json(original_cmd, original_serial, RESPONSE_STATUS_FAILED, message);
}

char* create_error_response(int original_cmd, int original_serial, const char* message)
{
    return create_response_json(original_cmd, original_serial, RESPONSE_STATUS_ERROR, message);
}

void send_command_response(int original_cmd, int original_serial, bool success, const char* message)
{
    char* response;
    if (success) {
        response = create_success_response(original_cmd, original_serial);
    } else {
        response = create_failed_response(original_cmd, original_serial, message);
    }
    
    if (response) {
        app_mqtt5_dev2app(response);
        free(response);
        ESP_LOGI(TAG, "Command response sent: %s", message);
    }
}

// ================== 主动上报函数 ==================

// void report_ota_progress(ota_status_t status, int progress)
// {
//     // TODO: 实现OTA进度上报
//     ESP_LOGI(TAG, "OTA progress: status=%d, progress=%d", status, progress);
// }
// OTA进度上报函数
char* report_ota_progress_to_json(ota_status_t status, int progress, const char* version)
{
    cJSON *payload_obj = cJSON_CreateObject();
    if (payload_obj == NULL) {
        ESP_LOGE(TAG, "Failed to create OTA progress payload object");
        return NULL;
    }

    // 添加OTA状态字段
    const char* ota_status_str = "unknown";

    switch (status) {
        case OTA_STATUS_START: ota_status_str = "OTA_START"; break;
        case OTA_STATUS_DOWNLOADING: ota_status_str = "OTA_EXECUTING"; break;
        case OTA_STATUS_SUCCESS: ota_status_str = "OTA_SUCCESS"; break;
        case OTA_STATUS_FAILED: ota_status_str = "OTA_FAIL"; break;
    }
   
    
    cJSON_AddStringToObject(payload_obj, "otaStatus", ota_status_str);
    cJSON_AddNumberToObject(payload_obj, "progress", progress);
    
    if (version != NULL) {
        cJSON_AddStringToObject(payload_obj, "version", version);
    }
    
    cJSON_AddStringToObject(payload_obj, "mac", app_mqtt5_get_device_id());

    cJSON *command_root = cJSON_CreateObject();
    if (command_root == NULL) {
        cJSON_Delete(payload_obj);
        return NULL;
    }

    cJSON_AddNumberToObject(command_root, "cmd", CMD_OTA_PROGRESS);
    cJSON_AddNumberToObject(command_root, "serial", (int)GET_CURRENT_TIMESTAMP());
    cJSON_AddStringToObject(command_root, "uuid", app_mqtt5_get_device_id());
    cJSON_AddStringToObject(command_root, "keytype", "NK");
    cJSON_AddStringToObject(command_root, "vendor", "starlinker");

#if SUPPORT_DUAL_PAYLOAD_FORMAT
    char *payload_str = cJSON_PrintUnformatted(payload_obj);
    if (payload_str != NULL) {
        cJSON_AddStringToObject(command_root, "payload", payload_str);
        free(payload_str);
    }
    cJSON_Delete(payload_obj);
#else
    cJSON_AddItemToObject(command_root, "payload", payload_obj);
#endif

    char *command_json_str = cJSON_PrintUnformatted(command_root);
    cJSON_Delete(command_root);
    
    if (command_json_str == NULL) {
        ESP_LOGE(TAG, "Failed to generate OTA progress JSON string");
        return NULL;
    }

    ESP_LOGD(TAG, "Generated OTA progress report: status=%d, progress=%d", status, progress);
    return command_json_str;
}

// 上报OTA进度
void report_ota_progress(ota_status_t status, int progress, const char* version)
{
    char* json_str = report_ota_progress_to_json(status, progress, version);
    if (json_str) {
        app_mqtt5_dev2app(json_str);
        free(json_str);
        ESP_LOGI(TAG, "OTA progress reported (CMD 106): status=%d, progress=%d", status, progress);
    } else {
        ESP_LOGE(TAG, "Failed to report OTA progress");
    }
}

// 便捷的OTA进度上报函数
void report_ota_start(const char* version)
{
    report_ota_progress(OTA_STATUS_START, 0, version);
}

void report_ota_downloading(int progress, const char* version)
{
    report_ota_progress(OTA_STATUS_DOWNLOADING, progress, version);
}

void report_ota_success(const char* version)
{
    report_ota_progress(OTA_STATUS_SUCCESS, 100, version);
}

void report_ota_failed(const char* version)
{
    report_ota_progress(OTA_STATUS_FAILED, 0, version);
}

void report_device_online(void)
{
    // TODO: 实现设备上线上报
    ESP_LOGI(TAG, "Device online reported");
}

long app_get_timestamp()
{
	time_t now;
	time(&now);
	return now;
}