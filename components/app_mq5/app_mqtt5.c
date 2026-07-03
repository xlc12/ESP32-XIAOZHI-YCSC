/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
//#include "protocol_examples_common.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "app_mqtt5.h"
#include "app_mq_data.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "app_mqtt5";

// 新增宏：禁用MQTT5 Property功能，使用传统MQTT 3.1.1报文
#define DISABLE_MQTT5_PROPERTY 1

typedef struct {
    char client_id[48];
    char username[13];//device_id
    char password[65];
    char uuid[33];//uuid
} mq5_client_cfg_t;

#if 1
mq5_client_cfg_t g_mq5_client_cfg = {0};
#else
mq5_client_cfg_t g_mq5_client_cfg = {
    .client_id = "wifi_84f7037f19c1_1982827294154412034",
    .username = "84f7037f19c1",
    .password = "TA2sfFcSaQT9xCFzYKjOtxGg4Z0Wc9SenduW1ZJVyYDFyuCGM6+EU8e+yQPElGCm",
    .uuid = "1982827294154412034",
};
#endif

int app_mqtt5_set_cfg(char *client_id, char *username, char *password, char *uuid)
{
    if (client_id) {
        strncpy(g_mq5_client_cfg.client_id, client_id, sizeof(g_mq5_client_cfg.client_id));
    }
    if (username) {
        strncpy(g_mq5_client_cfg.username, username, sizeof(g_mq5_client_cfg.username));
    }
    if (password) {
        strncpy(g_mq5_client_cfg.password, password, sizeof(g_mq5_client_cfg.password));
    }
    if (uuid) {
        strncpy(g_mq5_client_cfg.uuid, uuid, sizeof(g_mq5_client_cfg.uuid));
    }
    return 0;
}



#if 1
#define C_DEIVCE_ID (char *)(g_mq5_client_cfg.username)
#define C_UUID_ID (char *)(g_mq5_client_cfg.uuid)
#define C_MQ5_CLIENT_ID (char *)(g_mq5_client_cfg.client_id)
#else
#define C_DEIVCE_ID "84f7037f19c1"
#define C_UUID_ID "1982827294154412034"
#define C_MQ5_CLIENT_ID "wifi_" C_DEIVCE_ID "_" C_UUID_ID //"wifi_84f7037f19c1_1982827294154412034"
#endif

#define MQTT_CFG_HOST "mqtt://175.27.244.110"//DianXinNet 
#define MQTT_CFG_PORT 1883
#define MQTT_CFG_USER  (char *)(g_mq5_client_cfg.username)
#define MQTT_CFG_PASS  (char *)(g_mq5_client_cfg.password)
#define MQTT_CFG_HEAT_SEC  50//10

// 在现有的全局变量后添加
static char g_dev2server_topic[64];
static char g_dev2app_topic[64]; 
static char g_app2dev_topic[64];
static char g_app2devUid_topic[64];  // 新增的110命令订阅主题

// 全局变量用于存储MQTT客户端句柄和定时器句柄
static esp_mqtt_client_handle_t g_mqtt_client = NULL;
static TaskHandle_t g_report_task_handle = NULL;

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

// 只有在启用MQTT5 Property时才定义这些结构体
#if !DISABLE_MQTT5_PROPERTY
static esp_mqtt5_user_property_item_t user_property_arr[] = {
        {"board", "esp32"},
        {"u", "user"},
        {"p", "password"}
    };

#define USE_PROPERTY_ARR_SIZE   sizeof(user_property_arr)/sizeof(esp_mqtt5_user_property_item_t)

static esp_mqtt5_publish_property_config_t publish_property = {
    .payload_format_indicator = 1,
    .message_expiry_interval = 1000,
    .topic_alias = 0,
    .response_topic = "/topic/test/response",
    .correlation_data = "123456",
    .correlation_data_len = 6,
};

static esp_mqtt5_subscribe_property_config_t subscribe_property = {
    .subscribe_id = 25555,
    .no_local_flag = false,
    .retain_as_published_flag = false,
    .retain_handle = 0,
    .is_share_subscribe = true,
    .share_name = "group1",
};

static esp_mqtt5_subscribe_property_config_t subscribe1_property = {
    .subscribe_id = 25555,
    .no_local_flag = true,
    .retain_as_published_flag = false,
    .retain_handle = 0,
};

static esp_mqtt5_unsubscribe_property_config_t unsubscribe_property = {
    .is_share_subscribe = true,
    .share_name = "group1",
};

static esp_mqtt5_disconnect_property_config_t disconnect_property = {
    .session_expiry_interval = 60,
    .disconnect_reason = 0,
};

static void print_user_property(mqtt5_user_property_handle_t user_property)
{
    if (user_property) {
        uint8_t count = esp_mqtt5_client_get_user_property_count(user_property);
        if (count) {
            esp_mqtt5_user_property_item_t *item = malloc(count * sizeof(esp_mqtt5_user_property_item_t));
            if (esp_mqtt5_client_get_user_property(user_property, item, &count) == ESP_OK) {
                for (int i = 0; i < count; i ++) {
                    esp_mqtt5_user_property_item_t *t = &item[i];
                    ESP_LOGI(TAG, "key is %s, value is %s", t->key, t->value);
                    free((char *)t->key);
                    free((char *)t->value);
                }
            }
            free(item);
        }
    }
}
#endif // !DISABLE_MQTT5_PROPERTY

#if 0
static char report_data[512];
static char buf[128];

// 5秒上报函数
static void test_send_periodic_report(void)
{
    if (g_mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized, skip reporting");
        return;
    }

    // 构造上报数据
    static int report_count = 0;
    report_count++;
    
    // 获取系统信息
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    
    // 构造JSON格式的上报数据
    snprintf(report_data, sizeof(report_data),
             "{\"count\":%d,\"free_heap\":%lu,\"min_heap\":%lu,\"timestamp\":%lu,\"device\":\"%s\"}",
             report_count, free_heap, min_heap, (unsigned long)esp_timer_get_time()/1000, C_DEIVCE_ID);
    
#if DISABLE_MQTT5_PROPERTY
    // 禁用Property功能，使用传统MQTT发布
    int msg_id = esp_mqtt_client_publish(g_mqtt_client, 
                                        "device/report/data",
                                        report_data, 
                                        0, 1, 0);  // QoS 1, 不保留
#else
    // 启用MQTT5 Property功能
    esp_mqtt5_client_set_user_property(&publish_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_publish_property(g_mqtt_client, &publish_property);
    
    int msg_id = esp_mqtt_client_publish(g_mqtt_client, 
                                        "device/report/data",
                                        report_data, 
                                        0, 1, 0);  // QoS 1, 不保留
    
    // 清理用户属性
    esp_mqtt5_client_delete_user_property(publish_property.user_property);
    publish_property.user_property = NULL;
#endif
    
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to send periodic report, msg_id=%d", msg_id);
    } else {
        ESP_LOGI(TAG, "Periodic report sent successfully, count=%d, msg_id=%d", report_count, msg_id);
    }
    


// 初始化主题名称
    snprintf(g_dev2server_topic, sizeof(g_dev2server_topic), "dev2server");
    snprintf(g_dev2app_topic, sizeof(g_dev2app_topic), "dev2app/%s",  C_UUID_ID); // 使用您的UID
    snprintf(g_app2dev_topic, sizeof(g_app2dev_topic), "app2dev/%s", C_DEIVCE_ID); // 使用设备MAC作为Client ID
    snprintf(g_app2devUid_topic, sizeof(g_app2devUid_topic), "app2dev/%s", C_UUID_ID); // 新增110命令
    
#if 0 
    unsigned long timestamp = (unsigned long)esp_timer_get_time()/1000000;   
    // 发送连接成功消息到 dev2app
    snprintf(buf, sizeof(buf), "{\"status\":\"connected\",\"device\":\"%s\"}", C_DEIVCE_ID);
    msg_id = esp_mqtt_client_publish(g_mqtt_client, g_dev2app_topic, buf, 0, 1, 1);
    ESP_LOGI(TAG, "Connection announcement sent to %s, msg_id=%d", g_dev2app_topic, msg_id);
    
    // 发送设备上线消息到 dev2server
    snprintf(buf, sizeof(buf), "{\"type\":\"online\",\"device\":\"%s\",\"timestamp\":%lu}", 
             C_DEIVCE_ID, (unsigned long)esp_timer_get_time()/1000000);
    msg_id = esp_mqtt_client_publish(g_mqtt_client, g_dev2server_topic, buf, 0, 1, 0);
    ESP_LOGI(TAG, "Online message sent to %s, msg_id=%d", g_dev2server_topic, msg_id);
#endif

    // 订阅 app2dev 主题
    msg_id = esp_mqtt_client_subscribe(g_mqtt_client, g_app2dev_topic, 1);
    ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", g_app2dev_topic, msg_id);
}
#endif

// static void test_report(int cnt){
//     ESP_LOGI(TAG, "cnt=%d", cnt);
//     app_mqtt5_dev2server();
//     snprintf(buf, sizeof(buf), "app_mqtt5_dev2app(%d) device", cnt);
//     app_mqtt5_dev2app(buf);
// }
// // 上报设备状态（封装了JSON生成和MQTT发送）
// void report_device_status(const device_status_t* status)
// {
//     char* json_str = report_device_status_to_json(status);
//     if (json_str) {
//         app_mqtt5_dev2app(json_str);
//         free(json_str);
//     }
// }

// file: components/app_mq5/app_mqtt5.c
// 定时上报任务函数 - 优化版本
// file: components/app_mq5/app_mqtt5.c
static void report_task(void *pvParameters)
{
 #define C_SEC_DELAY_ONE (5*1000)  // 每5秒查一次
 #define C_SEC_DELAY_TOTAL (60*60*1000)  // 每10秒上报一次

    ESP_LOGI(TAG, "Report task started");

    // 初始化连续上报3次，每秒一次
    ESP_LOGI(TAG, "Initializing with 3 consecutive reports");
    for (int i = 0; i < 3; i++) {
        report_device_status();
        ESP_LOGI(TAG, "Initial status report %d/3", i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));  // 每秒一次
    }

    ESP_LOGI(TAG, "Initial reports completed, switching to periodic mode");
    
    int cnt = 0;
    while (1) {
        // 直接延时1分钟，然后上报cnt次
        cnt++;
        vTaskDelay(pdMS_TO_TICKS(C_SEC_DELAY_ONE));  // 1分钟
        
        if (cnt % (C_SEC_DELAY_TOTAL / C_SEC_DELAY_ONE) != 0) {
            continue; // 不是上报时间，继续等待
        }
        ESP_LOGI(TAG, "Periodic report %d/%d", cnt, C_SEC_DELAY_TOTAL / C_SEC_DELAY_ONE);
        report_device_status();
       
    }
    // 删除上报任务
    ESP_LOGW(TAG, "Deleting report task");
    vTaskDelete(NULL);
    

}


// 创建定时上报任务
static void create_periodic_report_task(void)
{
    ESP_LOGI(TAG, "create_periodic_report_task");
    if (g_report_task_handle != NULL) {
        ESP_LOGW(TAG, "Report task already created, skip");
        return;
    }
    
    BaseType_t result = xTaskCreate(
        report_task,
        "ReportTask",
        4096,  // 堆栈大小
        NULL,
        2,     // 优先级
        &g_report_task_handle
    );
    
    if (result == pdPASS) {
        ESP_LOGI(TAG, "Periodic report task created successfully");
    } else {
        ESP_LOGE(TAG, "Failed to create report task");
    }
}

// 停止定时上报任务
static void stop_periodic_report_task(void)
{
    ESP_LOGI(TAG, "stop_periodic_report_task");
    if (g_report_task_handle != NULL) {
        vTaskDelete(g_report_task_handle);
        g_report_task_handle = NULL;
        ESP_LOGI(TAG, "Periodic report task stopped");
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */

// 简洁版本 - 根据宏选择是否使用Property
static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    char buf[128];

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        g_mqtt_client = client;
        
#if DISABLE_MQTT5_PROPERTY
        ESP_LOGI(TAG, "MQTT Property disabled, using MQTT 3.1.1 protocol");
#else
        ESP_LOGI(TAG, "MQTT5 Property enabled");
        print_user_property(event->property->user_property);
#endif
        
        // 初始化主题名称
        snprintf(g_dev2server_topic, sizeof(g_dev2server_topic), "dev2server");
        snprintf(g_dev2app_topic, sizeof(g_dev2app_topic), "dev2app/%s",  C_UUID_ID); // 使用您的UID
        snprintf(g_app2dev_topic, sizeof(g_app2dev_topic), "app2dev/%s", C_DEIVCE_ID); // 使用设备MAC作为Client ID
        snprintf(g_app2devUid_topic, sizeof(g_app2devUid_topic), "app2dev/%s", C_UUID_ID); // 新增110命令
        // 发送连接成功消息到 dev2app
 #if 0
        snprintf(buf, sizeof(buf), "{\"status\":\"connected\",\"device\":\"%s\"}", C_DEIVCE_ID);
        msg_id = esp_mqtt_client_publish(g_mqtt_client, g_dev2app_topic, buf, 0, 1, 1);
        ESP_LOGI(TAG, "Connection announcement sent to %s, msg_id=%d", g_dev2app_topic, msg_id);
        
        // 发送设备上线消息到 dev2server
        snprintf(buf, sizeof(buf), "{\"type\":\"online\",\"device\":\"%s\",\"timestamp\":%lu}", 
                C_DEIVCE_ID, (unsigned long)esp_timer_get_time()/1000000);
        msg_id = esp_mqtt_client_publish(g_mqtt_client, g_dev2server_topic, buf, 0, 1, 0);
        ESP_LOGI(TAG, "Online message sent to %s, msg_id=%d", g_dev2server_topic, msg_id);
#endif        
        // 订阅 app2dev 主题
        msg_id = esp_mqtt_client_subscribe(g_mqtt_client, g_app2dev_topic, 1);
        ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", g_app2dev_topic, msg_id);
        // 订阅新增的110命令主题
        msg_id = esp_mqtt_client_subscribe(g_mqtt_client, g_app2devUid_topic, 1);
        ESP_LOGI(TAG, "Subscribed to 110 command topic: %s, msg_id=%d", g_app2devUid_topic, msg_id);

#ifdef TEST_MQTT5_PERIODIC_REPORT
        // 启动定时上报
        create_periodic_report_task();
#endif
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        //stop_periodic_report_timer();
        stop_periodic_report_task();

        g_mqtt_client = NULL;
#if !DISABLE_MQTT5_PROPERTY
        print_user_property(event->property->user_property);
#endif
        break;
        
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
#if !DISABLE_MQTT5_PROPERTY
        print_user_property(event->property->user_property);
#endif
        break;

#if 1
    // 修改 MQTT_EVENT_DATA 事件处理
    case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
    printf("DATA=%.*s\r\n", event->data_len, event->data);
    
    // 检查是否是订阅的 app2dev 主题
    if (event->topic_len == strlen(g_app2dev_topic) && 
        strncmp(event->topic, g_app2dev_topic, event->topic_len) == 0) {
        
        ESP_LOGI(TAG, "Received message from app2dev topic");
        
        // 直接解析并处理命令
        char* data_str = malloc(event->data_len + 1);
        if (data_str) {
            memcpy(data_str, event->data, event->data_len);
            data_str[event->data_len] = '\0';
            
            // 一行代码搞定：解析并自动回调处理
            parse_and_handle_device_command(data_str);
            
            free(data_str);
        }
    // 检查是否是新增的 110 命令主题
        } else if (event->topic_len == strlen(g_app2devUid_topic) && 
             strncmp(event->topic, g_app2devUid_topic, event->topic_len) == 0) {
        
        ESP_LOGI(TAG, "Received message from 110 command topic (app2devUid)");
        
        // 解析并处理命令
        char* data_str = malloc(event->data_len + 1);
        if (data_str) {
            memcpy(data_str, event->data, event->data_len);
            data_str[event->data_len] = '\0';
            parse_and_handle_device_command(data_str);
            free(data_str);
        }
    }

    break;

#else
case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
    printf("DATA=%.*s\r\n", event->data_len, event->data);
    
    // 检查是否是订阅的 app2dev 主题
    if (event->topic_len == strlen(g_app2dev_topic) && 
        strncmp(event->topic, g_app2dev_topic, event->topic_len) == 0) {
        
        ESP_LOGI(TAG, "Received message from app2dev topic");
        
        // 解析收到的JSON数据
        char* data_str = malloc(event->data_len + 1);
        if (data_str) {
            memcpy(data_str, event->data, event->data_len);
            data_str[event->data_len] = '\0';
            
            ESP_LOGI(TAG, "App2Dev message: %s", data_str);
            
            // 这里可以添加具体的命令处理逻辑
            // 例如：解析JSON并执行相应操作
            
            free(data_str);
        }
    }
    break;
    #endif

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        //stop_periodic_report_timer();
        stop_periodic_report_task();
        g_mqtt_client = NULL;
#if !DISABLE_MQTT5_PROPERTY
        print_user_property(event->property->user_property);
        ESP_LOGI(TAG, "MQTT5 return code is %d", event->error_handle->connect_return_code);
#endif
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
        }
        break;
        
    default:
        break;
    }
}

void show_mqtt5_cfg(void)
{
    ESP_LOGW(TAG, "MQTT_CFG_HOST=%s", MQTT_CFG_HOST);
    ESP_LOGI(TAG, "MQTT_CFG_PORT=%d", MQTT_CFG_PORT);
    ESP_LOGI(TAG, "MQTT_CFG_USER=%s", MQTT_CFG_USER);
    //;ESP_LOGI(TAG, "MQTT_CFG_PASS=%s", MQTT_CFG_PASS);
    ESP_LOGI(TAG, "MQTT_CFG_HEAT_SEC=%d", MQTT_CFG_HEAT_SEC);
#if DISABLE_MQTT5_PROPERTY
    ESP_LOGI(TAG, "MQTT5_PROPERTY: DISABLED (Using MQTT 3.1.1)");
#else
    ESP_LOGI(TAG, "MQTT5_PROPERTY: ENABLED");
#endif
    ESP_LOGW(TAG, "MQTT_CFG_CLIENT_ID=%s", C_MQ5_CLIENT_ID);
}

static void mqtt5_app_start(void)
{
#if DISABLE_MQTT5_PROPERTY
    // 禁用Property功能时使用MQTT 3.1.1配置
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_CFG_HOST,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,  // 使用MQTT 3.1.1
        .network.disable_auto_reconnect = false,
        .credentials.username = MQTT_CFG_USER,
        .credentials.client_id = C_MQ5_CLIENT_ID,
        .credentials.authentication.password = MQTT_CFG_PASS,
        .session.keepalive = MQTT_CFG_HEAT_SEC,
        .session.last_will.topic = "/topic/will",
        .session.last_will.msg = "i will leave",
        .session.last_will.msg_len = 12,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,

        .buffer.size = 4096,  // 增加接收缓冲区大小
        .buffer.out_size = 1024,  // 增加输出缓冲区大小
    };
#else
    // 启用MQTT5 Property功能
    esp_mqtt5_connection_property_config_t connect_property = {
        .session_expiry_interval = 10,
        .maximum_packet_size = 1024,
        .receive_maximum = 65535,
        .topic_alias_maximum = 2,
        .request_resp_info = true,
        .request_problem_info = true,
        .will_delay_interval = 10,
        .payload_format_indicator = true,
        .message_expiry_interval = 10,
        .response_topic = "/test/response",
        .correlation_data = "123456",
        .correlation_data_len = 6,
    };

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_CFG_HOST,
        .session.protocol_ver = MQTT_PROTOCOL_V_5,  // 使用MQTT 5.0
        .network.disable_auto_reconnect = false,
        .credentials.username = MQTT_CFG_USER,
        .credentials.client_id = C_DEIVCE_ID,
        .credentials.authentication.password = MQTT_CFG_PASS,
        .session.last_will.topic = "/topic/will",
        .session.last_will.msg = "i will leave",
        .session.last_will.msg_len = 12,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };
#endif

#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.broker.address.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.broker.address.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);

#if !DISABLE_MQTT5_PROPERTY
    /* Set connection properties and user properties for MQTT5 */
    esp_mqtt5_client_set_user_property(&connect_property.user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_user_property(&connect_property.will_user_property, user_property_arr, USE_PROPERTY_ARR_SIZE);
    esp_mqtt5_client_set_connect_property(client, &connect_property);

    /* If you call esp_mqtt5_client_set_user_property to set user properties, DO NOT forget to delete them */
    esp_mqtt5_client_delete_user_property(connect_property.user_property);
    esp_mqtt5_client_delete_user_property(connect_property.will_user_property);
#endif

    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void test_app_mqtt5(void)
{
    show_mqtt5_cfg();
    mqtt5_app_start();
}

// 手动触发上报的函数（可选）
int app_mqtt5_dev2server(const char *msg)
{
    if (g_mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return -1;
    }
    if (msg == NULL) {
        ESP_LOGE(TAG, "Invalid message");
        return -1;
    }
    
    int msg_id;
    msg_id = esp_mqtt_client_publish(g_mqtt_client, g_dev2server_topic, msg, 0, 1, 0);
    ESP_LOGI(TAG, "Message sent to %s, msg_id=%d", g_dev2server_topic, msg_id);
    ESP_LOGW(TAG, "[%s] len=%d", msg, strlen(msg));
    return msg_id;
}
//g_dev2server_topicg_dev2server_topic
int app_mqtt5_dev2app(const char *msg)
{
    if (g_mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return -1;
    }
    if (msg == NULL) {
        ESP_LOGE(TAG, "Invalid message");
        return -1;
    }
    
    int msg_id;
    msg_id = esp_mqtt_client_publish(g_mqtt_client, g_dev2app_topic, msg, 0, 1, 0);
    ESP_LOGI(TAG, "Message sent to %s, msg_id=%d", g_dev2app_topic, msg_id);
    ESP_LOGW(TAG, "[%s] len=%d", msg, strlen(msg));
    return msg_id;
}
const char* app_mqtt5_get_device_id(void)
{
    return C_DEIVCE_ID;
}

const char* app_mqtt5_get_uuid(void)
{
    return C_UUID_ID;
}