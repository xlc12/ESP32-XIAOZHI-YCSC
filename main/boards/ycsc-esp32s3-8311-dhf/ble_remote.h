/*
 * BLE 遥控器主机模块 —— 对外接口
 *
 * 用法：
 *   1. ble_remote_init()          初始化并自动扫描连接
 *   2. ble_remote_register_key_callback()  注册按键回调，在里面判断键值执行控制
 *   3. （可选）ble_remote_register_state_callback()  注册连接状态回调
 *
 * 按键解析：
 *   默认把原始数据前 4 字节小端拼成 key_code。
 *   如果你的遥控器有自定义协议，重写 ble_remote_parse_key() 即可。
 */
#ifndef _BLE_REMOTE_H_
#define _BLE_REMOTE_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 *                         可配置项（编译期）
 * ======================================================================== */

/* 目标 Service / Characteristic / CCCD —— 16 位 UUID */
#define BLE_REMOTE_SERVICE_UUID16      0xFFF0
#define BLE_REMOTE_NOTIFY_CHAR_UUID16  0xFFF1
#define BLE_REMOTE_CCCD_UUID16         0x2902

/* 设备名前缀过滤，留空 "" 表示按 Service UUID 匹配 */
#define BLE_REMOTE_NAME_PREFIX         "大黄蜂"

/* 扫描参数 */
#define BLE_REMOTE_SCAN_INTERVAL       0x0060
#define BLE_REMOTE_SCAN_WINDOW         0x0030
#define BLE_REMOTE_SCAN_DURATION_SEC   0

/* 连接参数 */
#define BLE_REMOTE_CONN_INTERVAL_MIN   0x0010
#define BLE_REMOTE_CONN_INTERVAL_MAX   0x0020
#define BLE_REMOTE_CONN_LATENCY        0
#define BLE_REMOTE_CONN_TIMEOUT        0x0100

/* 断开后重连延迟（毫秒） */
#define BLE_REMOTE_RECONNECT_DELAY_MS  2000

/* 原始数据最大长度 */
#define BLE_REMOTE_MAX_DATA_LEN        32

/* ========================================================================
 *                         数据类型
 * ======================================================================== */

/* 连接状态 */
typedef enum {
    BLE_REMOTE_STATE_IDLE = 0,     /* 未连接 */
    BLE_REMOTE_STATE_SCANNING,     /* 扫描中 */
    BLE_REMOTE_STATE_CONNECTING,   /* 连接中 */
    BLE_REMOTE_STATE_CONNECTED,    /* 已连接（正在发现服务） */
    BLE_REMOTE_STATE_DISCOVERING,  /* 发现服务中 */
    BLE_REMOTE_STATE_NOTIFY_READY, /* Notify 已使能，可接收按键 */
} ble_remote_state_t;

/* 按键事件 */
typedef struct {
    uint32_t key_code;                         /* 解析后的键值 */
    uint8_t  raw[BLE_REMOTE_MAX_DATA_LEN];     /* 原始数据 */
    uint16_t raw_len;                          /* 原始数据长度 */
} ble_remote_key_event_t;

/* 按键回调：收到按键数据时调用 */
typedef void (*ble_remote_key_cb_t)(const ble_remote_key_event_t *event);

/* 状态回调：连接状态变化时调用（细粒度，6 个状态） */
typedef void (*ble_remote_state_cb_t)(ble_remote_state_t state);

/* 连接事件：直观的连上/就绪/断开，适合做 UI 提示 */
typedef enum {
    BLE_REMOTE_CONN_CONNECTED    = 0,  /* 物理连接建立 */
    BLE_REMOTE_CONN_READY        = 1,  /* Notify 已使能，可以收按键了 */
    BLE_REMOTE_CONN_DISCONNECTED = 2,  /* 连接断开（含超时、被断开） */
} ble_remote_conn_event_t;

/* 连接事件回调：连接成功 / 就绪 / 断开时主动调用 */
typedef void (*ble_remote_conn_cb_t)(ble_remote_conn_event_t event);

/* ========================================================================
 *                         API
 * ======================================================================== */

/**
 * @brief  初始化 BLE 遥控器主机，自动开始扫描并连接目标设备
 * @return ESP_OK 成功，其他失败
 */
esp_err_t ble_remote_init(void);

/**
 * @brief  反初始化，断开连接、停止扫描、关闭蓝牙协议栈和控制器
 * @note   调用后可再次 ble_remote_init()
 */
void ble_remote_deinit(void);

/**
 * @brief  注册按键事件回调
 * @param  cb  回调函数，收到按键数据时触发
 */
void ble_remote_register_key_callback(ble_remote_key_cb_t cb);

/**
 * @brief  注册连接状态回调（细粒度，6 个状态）
 * @param  cb  回调函数，状态变化时触发
 */
void ble_remote_register_state_callback(ble_remote_state_cb_t cb);

/**
 * @brief  注册连接事件回调（直观的连上/就绪/断开）
 * @param  cb  回调函数，连接成功 / Notify 就绪 / 断开时主动触发
 *
 * 事件说明：
 *   - BLE_REMOTE_CONN_CONNECTED    物理连接建立
 *   - BLE_REMOTE_CONN_READY        Notify 使能完成，可以收按键
 *   - BLE_REMOTE_CONN_DISCONNECTED 连接断开（之后会自动重连）
 */
void ble_remote_register_conn_callback(ble_remote_conn_cb_t cb);

/**
 * @brief  获取当前连接状态
 */
ble_remote_state_t ble_remote_get_state(void);

/**
 * @brief  是否已连接且 Notify 就绪
 */
bool ble_remote_is_ready(void);

/* ========================================================================
 *               按键解析函数（可重写 / weak）
 * ======================================================================== */

/**
 * @brief  把遥控器原始数据解析成键值
 * @note   这是 weak 函数，默认实现：前 4 字节小端拼成 uint32。
 *         如果你的遥控器有自定义协议，在你的代码里重写此函数即可，
 *         链接时会自动覆盖默认实现。
 *
 * @param  data 原始数据指针
 * @param  len  原始数据长度
 * @return 解析后的键值
 *
 * 示例（重写）：
 * @code
 *   uint32_t ble_remote_parse_key(const uint8_t *data, uint16_t len)
 *   {
 *       if (len >= 2 && data[0] == 0x55) {
 *           return data[1];  // 第 2 字节是键值
 *       }
 *       return 0;
 *   }
 * @endcode
 */
uint32_t ble_remote_parse_key(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* _BLE_REMOTE_H_ */
