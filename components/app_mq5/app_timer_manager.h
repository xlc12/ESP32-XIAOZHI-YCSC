#ifndef APP_TIMER_MANAGER_H
#define APP_TIMER_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "app_mq_data.h"
// 定时器回调函数类型定义
typedef void (*timer_alarm_callback_t)(const char* timer_id, const char* task, uint32_t timestamp);

// 统一的定时器项结构体（与 app_mq_data.h 保持一致）
// typedef struct {
//     char id[32];           // 定时器ID
//     uint32_t timestamp;    // 时间戳（秒）
//     char task[128];        // 任务描述
// } timer_item_t;

// 初始化定时器管理器
void app_timer_manager_init(void);

// 设置定时器报警回调函数
void app_timer_manager_set_callback(timer_alarm_callback_t callback);

// 添加/更新定时器
int app_timer_manager_add_timers(timer_item_t* timers, size_t count);

// 删除定时器
int app_timer_manager_remove_timer(const char* timer_id);

// 清除所有定时器
void app_timer_manager_clear_all(void);

// 获取下一个即将触发的定时器时间
uint32_t app_timer_manager_get_next_alarm_time(void);

// 获取所有活跃的定时器数量
size_t app_timer_manager_get_active_count(void);

// 打印所有定时器状态
void app_timer_manager_print_status(void);

// 停止定时器管理器
void app_timer_manager_stop(void);

#ifdef __cplusplus
}
#endif

#endif