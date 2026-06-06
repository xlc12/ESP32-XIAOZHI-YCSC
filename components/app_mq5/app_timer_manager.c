#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "app_timer_manager.h"

static const char *TAG = "app_timer_manager";

// 事件组定义
#define TIMER_UPDATE_BIT (1 << 0)
static EventGroupHandle_t g_timer_event_group = NULL;

// 链表节点结构体（内部使用）
typedef struct timer_node_s {
    timer_item_t data;           // 定时器数据
    bool active;                 // 是否激活
    struct timer_node_s* next;   // 下一个节点
} timer_node_t;

// 定时器管理器状态
typedef struct {
    timer_node_t* head;               // 链表头（按时间戳排序）
    size_t timer_count;
    timer_alarm_callback_t alarm_callback;
    TaskHandle_t monitor_task_handle;
    bool running;
    uint32_t next_alarm_time;         // 下一个报警时间
} timer_manager_t;

static timer_manager_t g_timer_manager = {0};

// 内部函数声明
static void timer_monitor_task(void *pvParameters);
static timer_node_t* find_timer_node(const char* timer_id);
static void insert_timer_sorted(timer_node_t* new_node);
static void remove_timer_node(const char* timer_id);
static void check_and_trigger_alarms(void);
static void update_next_alarm_time(void);
static void free_timer_list(void);

#define EXPIRED_TIMER_SEC 60//默认过期时间1小时
#define MAX_TIMER_LIST_SIZE 10//最大定时器链表节点数

static void cleanup_expired_timers(void);
static void limit_list_size(size_t max_size);//限制链表大小不超过max_size


// ================== 公共函数实现 ==================

// 初始化定时器管理器
void app_timer_manager_init(void)
{
    if (g_timer_manager.running) {
        ESP_LOGW(TAG, "Timer manager already initialized");
        return;
    }

    memset(&g_timer_manager, 0, sizeof(g_timer_manager));
    g_timer_manager.head = NULL;
    g_timer_manager.next_alarm_time = UINT32_MAX;
    
    // 创建事件组
    g_timer_event_group = xEventGroupCreate();
    if (g_timer_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return;
    }

    // 创建监控任务
    BaseType_t result = xTaskCreate(
        timer_monitor_task,
        "TimerMonitor",
        4096,
        NULL,
        3,  // 中等优先级
        &g_timer_manager.monitor_task_handle
    );

    if (result == pdPASS) {
        g_timer_manager.running = true;
        ESP_LOGI(TAG, "Timer manager initialized successfully");
    } else {
        ESP_LOGE(TAG, "Failed to create timer monitor task");
        vEventGroupDelete(g_timer_event_group);
        g_timer_event_group = NULL;
    }
}

// 设置定时器报警回调函数
void app_timer_manager_set_callback(timer_alarm_callback_t callback)
{
    g_timer_manager.alarm_callback = callback;
    ESP_LOGI(TAG, "Timer alarm callback registered");
}

// 添加/更新定时器
int app_timer_manager_add_timers(timer_item_t* timers, size_t count)
{
    if (timers == NULL || count == 0) {
        ESP_LOGE(TAG, "Invalid timers data");
        return -1;
    }

    if (!g_timer_manager.running) {
        ESP_LOGE(TAG, "Timer manager not initialized");
        return -1;
    }

    ESP_LOGI(TAG, "Adding/updating %d timers", count);

    uint32_t current_time = GET_CURRENT_TIMESTAMP(); // 获取当前时间
    
    for (size_t i = 0; i < count; i++) {
        // 创建链表节点
        timer_node_t* new_node = (timer_node_t*)malloc(sizeof(timer_node_t));
        if (new_node == NULL) {
            ESP_LOGE(TAG, "Failed to create timer node for: %s", timers[i].id);
            continue;
        }
        
        // 复制数据
        memcpy(&new_node->data, &timers[i], sizeof(timer_item_t));
        //;new_node->active = true;
        // 根据时间戳判断是否激活：只有未来时间的定时器才激活
        if (timers[i].timestamp > current_time) {
            new_node->active = true;
            ESP_LOGI(TAG, "Timer %s activated, timestamp: %lu (in %lu seconds)", 
                    new_node->data.id, new_node->data.timestamp, 
                    new_node->data.timestamp - current_time);
        } else {
            new_node->active = false;
            ESP_LOGW(TAG, "Timer %s deactivated (expired), timestamp: %lu", 
                    new_node->data.id, new_node->data.timestamp);
        }
        
        
        new_node->next = NULL;

        // 先移除同ID的旧定时器（如果存在）
        remove_timer_node(new_node->data.id);
        
        // 按时间戳排序插入
        insert_timer_sorted(new_node);
        
        ESP_LOGI(TAG, "Timer %s added/updated, timestamp: %lu", 
                new_node->data.id, new_node->data.timestamp);
    }

// 清理过期节点，限制链表大小
    //cleanup_expired_timers();
    limit_list_size(MAX_TIMER_LIST_SIZE); // 最多保留10个节点

    // 更新下一个报警时间
    update_next_alarm_time();

    // 触发事件组，让监控任务立即检查
    if (g_timer_event_group) {
        xEventGroupSetBits(g_timer_event_group, TIMER_UPDATE_BIT);
    }

    ESP_LOGI(TAG, "Total timers after update: %d", g_timer_manager.timer_count);
    return 0;
}

// 删除定时器
int app_timer_manager_remove_timer(const char* timer_id)
{
    if (timer_id == NULL) {
        return -1;
    }

    remove_timer_node(timer_id);
    update_next_alarm_time();
    
    // 触发更新事件
    if (g_timer_event_group) {
        xEventGroupSetBits(g_timer_event_group, TIMER_UPDATE_BIT);
    }

    return 0;
}

// 清除所有定时器
void app_timer_manager_clear_all(void)
{
    free_timer_list();
    g_timer_manager.timer_count = 0;
    g_timer_manager.next_alarm_time = UINT32_MAX;
    ESP_LOGI(TAG, "All timers cleared");
}

// 获取下一个即将触发的定时器时间
uint32_t app_timer_manager_get_next_alarm_time(void)
{
    return g_timer_manager.next_alarm_time;
}

// 获取所有活跃的定时器数量
size_t app_timer_manager_get_active_count(void)
{
    size_t active_count = 0;
    timer_node_t* current = g_timer_manager.head;
    
    while (current != NULL) {
        if (current->active) {
            active_count++;
        }
        current = current->next;
    }
    
    return active_count;
}

// 打印所有定时器状态
void app_timer_manager_print_status(void)
{
    ESP_LOGI(TAG, "=== Timer Manager Status ===");
    ESP_LOGI(TAG, "Total timers: %d, Active: %d, Next alarm: %lu", 
            g_timer_manager.timer_count, 
            app_timer_manager_get_active_count(),
            g_timer_manager.next_alarm_time);
    
    timer_node_t* current = g_timer_manager.head;
    int index = 0;
           ESP_LOGI(TAG, "current            timestamp=%lu,", GET_CURRENT_TIMESTAMP());   
    while (current != NULL) {
        if (current->active) {
            ESP_LOGW(TAG, "Timer[%d]: id=%s, timestamp=%lu, task=%s, active=%s",index++, current->data.id, current->data.timestamp, current->data.task, current->active ? "true" : "false");
        } else {
            ESP_LOGI(TAG, "Timer[%d]: id=%s, timestamp=%lu, task=%s, active=%s",index++, current->data.id, current->data.timestamp, current->data.task, current->active ? "true" : "false");
        }

        current = current->next;
    }
    ESP_LOGI(TAG, "=== End Status ===");
}

// 停止定时器管理器
void app_timer_manager_stop(void)
{
    if (g_timer_manager.monitor_task_handle) {
        vTaskDelete(g_timer_manager.monitor_task_handle);
        g_timer_manager.monitor_task_handle = NULL;
    }

    if (g_timer_event_group) {
        vEventGroupDelete(g_timer_event_group);
        g_timer_event_group = NULL;
    }

    free_timer_list();
    g_timer_manager.running = false;
    ESP_LOGI(TAG, "Timer manager stopped");
}

// ================== 内部函数实现 ==================

// 查找定时器节点
static timer_node_t* find_timer_node(const char* timer_id)
{
    timer_node_t* current = g_timer_manager.head;
    
    while (current != NULL) {
        if (strcmp(current->data.id, timer_id) == 0) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

// 按时间戳排序插入
static void insert_timer_sorted(timer_node_t* new_node)
{
    // 如果链表为空或新节点时间最早，插入头部
    if (g_timer_manager.head == NULL || new_node->data.timestamp < g_timer_manager.head->data.timestamp) {
        new_node->next = g_timer_manager.head;
        g_timer_manager.head = new_node;
    } else {
        // 找到插入位置
        timer_node_t* current = g_timer_manager.head;
        while (current->next != NULL && current->next->data.timestamp <= new_node->data.timestamp) {
            current = current->next;
        }
        new_node->next = current->next;
        current->next = new_node;
    }
    
    g_timer_manager.timer_count++;
}

// 移除定时器节点
static void remove_timer_node(const char* timer_id)
{
    timer_node_t* current = g_timer_manager.head;
    timer_node_t* prev = NULL;
    
    while (current != NULL) {
        if (strcmp(current->data.id, timer_id) == 0) {
            if (prev == NULL) {
                // 移除头节点
                g_timer_manager.head = current->next;
            } else {
                prev->next = current->next;
            }
            
            free(current);
            g_timer_manager.timer_count--;
            ESP_LOGI(TAG, "Timer removed: %s", timer_id);
            return;
        }
        
        prev = current;
        current = current->next;
    }
}

// 更新下一个报警时间
static void update_next_alarm_time(void)
{
    timer_node_t* current = g_timer_manager.head;
    g_timer_manager.next_alarm_time = UINT32_MAX;
    
    // 找到第一个活跃的定时器时间
    while (current != NULL) {
        if (current->active && current->data.timestamp < g_timer_manager.next_alarm_time) {
            g_timer_manager.next_alarm_time = current->data.timestamp;
        }
        current = current->next;
    }
}

// 检查并触发报警
static void check_and_trigger_alarms(void)
{
    uint32_t current_time = GET_CURRENT_TIMESTAMP(); // 转换为秒
    
    // 由于链表已排序，只需要检查前面的定时器
    timer_node_t* current = g_timer_manager.head;
    
    while (current != NULL && current->data.timestamp <= current_time) {
        if (current->active) {
            ESP_LOGI(TAG, "Timer alarm triggered: %s - %s", 
                    current->data.id, current->data.task);
            
            // 调用回调函数
            if (g_timer_manager.alarm_callback) {
                g_timer_manager.alarm_callback(current->data.id, 
                                             current->data.task, 
                                             current->data.timestamp);
            }
            
            // 触发后标记为非激活
            current->active = false;
        }
        
        current = current->next;
    }
    
    // 更新下一个报警时间
    update_next_alarm_time();
}

// 释放整个链表
static void free_timer_list(void)
{
    timer_node_t* current = g_timer_manager.head;
    timer_node_t* next;
    
    while (current != NULL) {
        next = current->next;
        free(current);
        current = next;
    }
    
    g_timer_manager.head = NULL;
    g_timer_manager.timer_count = 0;
}

// 定时器监控任务
static void timer_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Timer monitor task started");
    
    while (1) {
        uint32_t next_alarm = g_timer_manager.next_alarm_time;
        uint32_t current_time = GET_CURRENT_TIMESTAMP();
        TickType_t wait_time;
        
        if (next_alarm == UINT32_MAX) {
            // 没有定时器，等待更新事件
            wait_time = portMAX_DELAY;
        } else if (next_alarm <= current_time) {
            // 立即检查
            wait_time = 0;
        } else {
            // 计算等待时间（加上1秒缓冲）
            uint32_t time_diff = next_alarm - current_time;
            wait_time = pdMS_TO_TICKS((time_diff + 1) * 1000);
            
            // 限制最大等待时间（防止长时间阻塞）
            if (wait_time > pdMS_TO_TICKS(300000)) { // 5分钟
                wait_time = pdMS_TO_TICKS(300000);
            }
        }
        
        // 等待事件或超时
        EventBits_t bits = xEventGroupWaitBits(
            g_timer_event_group,
            TIMER_UPDATE_BIT,
            pdTRUE,  // 清除位
            pdFALSE, // 不需要所有位
            wait_time
        );
        
        // 检查定时器报警
        check_and_trigger_alarms();
        
        // 如果有定时器触发，更新下一个报警时间
        if (bits & TIMER_UPDATE_BIT || wait_time == 0) {
            update_next_alarm_time();
        }
    }
    
    vTaskDelete(NULL);
}

// 清理过期和非活跃的定时器节点
// 清理过期和非活跃的定时器节点
static void cleanup_expired_timers(void)
{
    uint32_t current_time = GET_CURRENT_TIMESTAMP();
    timer_node_t* current = g_timer_manager.head;
    timer_node_t* prev = NULL;
    
    while (current != NULL) {
        timer_node_t* next = current->next;
        
        // 删除条件：非活跃 或 已过期超过1小时
        if (!current->active || (current->data.timestamp + EXPIRED_TIMER_SEC) < current_time) {
            ESP_LOGD(TAG, "Cleaning up timer: %s (active: %s, timestamp: %lu)", 
                    current->data.id, current->active ? "true" : "false", current->data.timestamp);
            
            if (prev == NULL) {
                g_timer_manager.head = next;
            } else {
                prev->next = next;
            }
            
            free(current);
            g_timer_manager.timer_count--;
            current = next;
        } else {
            prev = current;
            current = next;
        }
    }
}

// 限制链表大小，保留时间最新的N个节点
static void limit_list_size(size_t max_size)
{
    if (g_timer_manager.timer_count <= max_size) {
        return;
    }
    
    ESP_LOGI(TAG, "Limiting list size from %d to %d", g_timer_manager.timer_count, max_size);
    
    // 由于链表是按时间戳升序排列的，最新的节点在链表尾部
    // 我们需要删除链表头部的旧节点，保留尾部的新节点
    
    size_t nodes_to_keep = max_size;
    size_t nodes_to_remove = g_timer_manager.timer_count - nodes_to_keep;
    
    // 删除链表前部的nodes_to_remove个节点（时间最早的）
    for (size_t i = 0; i < nodes_to_remove; i++) {
        if (g_timer_manager.head == NULL) {
            break;
        }
        
        timer_node_t* to_delete = g_timer_manager.head;
        g_timer_manager.head = to_delete->next;
        
        ESP_LOGD(TAG, "Removing old timer: %s (timestamp: %lu)", 
                to_delete->data.id, to_delete->data.timestamp);
        free(to_delete);
        g_timer_manager.timer_count--;
    }
}
