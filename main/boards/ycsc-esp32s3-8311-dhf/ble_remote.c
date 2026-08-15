/*
 * BLE 遥控器主机模块实现（NimBLE 版，适配 ESP-IDF v5.5+）
 *
 * 协议栈：NimBLE（替代 Bluedroid，更省内存）
 * 重连：esp_timer 一次性定时器
 */
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"

#include "ble_remote.h"

#define LOG_TAG  "BLE_REMOTE"

/* 前向声明 */
static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* ========================================================================
 *                         内部状态
 * ======================================================================== */

static ble_remote_state_t  s_state          = BLE_REMOTE_STATE_IDLE;
static uint16_t            s_conn_handle    = 0;
static ble_addr_t          s_peer_addr      = {0};
static bool                s_is_connected   = false;
static bool                s_inited         = false;

static uint16_t            s_svc_start_h    = 0;
static uint16_t            s_svc_end_h      = 0;
static uint16_t            s_char_val_h     = 0;
static uint16_t            s_cccd_handle    = 0;

static ble_remote_key_cb_t    s_key_cb    = NULL;
static ble_remote_state_cb_t  s_state_cb  = NULL;
static ble_remote_conn_cb_t   s_conn_cb   = NULL;

static esp_timer_handle_t    s_reconnect_timer = NULL;

/* UUID（16位） */
static ble_uuid16_t s_svc_uuid  = BLE_UUID16_INIT(BLE_REMOTE_SERVICE_UUID16);
static ble_uuid16_t s_char_uuid = BLE_UUID16_INIT(BLE_REMOTE_NOTIFY_CHAR_UUID16);

/* ========================================================================
 *                         weak 按键解析
 * ======================================================================== */

__attribute__((weak))
uint32_t ble_remote_parse_key(const uint8_t *data, uint16_t len)
{
    uint32_t code = 0;
    for (uint16_t i = 0; i < len && i < 4; i++) {
        code |= ((uint32_t)data[i]) << (i * 8);
    }
    return code;
}

/* ========================================================================
 *                         工具函数
 * ======================================================================== */

static void set_state(ble_remote_state_t st)
{
    if (s_state != st) {
        ESP_LOGD(LOG_TAG, "state %d -> %d", s_state, st);
        s_state = st;
        if (s_state_cb) {
            s_state_cb(st);
        }
    }
}

static const char *addr_type_str(uint8_t type)
{
    switch (type) {
    case BLE_ADDR_PUBLIC:    return "PUBLIC";
    case BLE_ADDR_RANDOM:    return "RANDOM";
    case BLE_ADDR_PUBLIC_ID: return "PUBLIC_ID";
    case BLE_ADDR_RANDOM_ID: return "RANDOM_ID";
    default:                 return "UNKNOWN";
    }
}

/* 从广播数据中查找指定类型的字段 */
static const uint8_t *adv_find_field(uint8_t type, const uint8_t *data,
                                     uint8_t len, uint8_t *out_len)
{
    uint8_t pos = 0;
    while (pos + 1 < len) {
        uint8_t f_len = data[pos];
        if (f_len == 0) break;
        if (pos + 1 + f_len > len) break;
        if (data[pos + 1] == type) {
            if (out_len) *out_len = f_len - 1;
            return &data[pos + 2];
        }
        pos += 1 + f_len;
    }
    return NULL;
}

/* ========================================================================
 *                         重连定时器
 * ======================================================================== */

static void reconnect_timer_cb(void *arg)
{
    if (!s_inited) return;
    if (s_state == BLE_REMOTE_STATE_IDLE && !s_is_connected) {
        ESP_LOGI(LOG_TAG, "reconnecting: restart scanning...");
        set_state(BLE_REMOTE_STATE_SCANNING);
        struct ble_gap_disc_params disc_params = {
            .itvl              = BLE_REMOTE_SCAN_INTERVAL,
            .window            = BLE_REMOTE_SCAN_WINDOW,
            .filter_duplicates = 1,
            .passive           = 0,
        };
        ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_REMOTE_SCAN_DURATION_SEC * 1000,
                     &disc_params, gap_event_handler, NULL);
    }
}

static void start_scan(void)
{
    set_state(BLE_REMOTE_STATE_SCANNING);
    struct ble_gap_disc_params disc_params = {
        .itvl              = BLE_REMOTE_SCAN_INTERVAL,
        .window            = BLE_REMOTE_SCAN_WINDOW,
        .filter_duplicates = 1,
        .passive           = 0,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_REMOTE_SCAN_DURATION_SEC * 1000,
                          &disc_params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(LOG_TAG, "ble_gap_disc failed, rc=%d", rc);
    }
}

/* ========================================================================
 *               GATT 服务发现（异步链式）
 * ======================================================================== */

/* 步骤3：写 CCCD 完成 */
static int on_dsc_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    if (error->status != 0) {
        ESP_LOGE(LOG_TAG, "write cccd failed, status=%d", error->status);
        return 0;
    }
    ESP_LOGI(LOG_TAG, "cccd write success, handle=0x%x", attr->handle);
    set_state(BLE_REMOTE_STATE_NOTIFY_READY);
    ESP_LOGI(LOG_TAG, "===== Notify enabled, ready =====");
    if (s_conn_cb) s_conn_cb(BLE_REMOTE_CONN_READY);
    return 0;
}

/* 步骤2：发现描述符，找到 CCCD(0x2902) */
static int on_dsc(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    if (error->status != 0) {
        if (s_cccd_handle != 0) {
            ESP_LOGI(LOG_TAG, "cccd found: handle=0x%x, write enable notify", s_cccd_handle);
            uint16_t notify_en = 0x0001;
            ble_gattc_write_flat(conn_handle, s_cccd_handle,
                                 &notify_en, sizeof(notify_en),
                                 on_dsc_write, NULL);
        } else {
            ESP_LOGE(LOG_TAG, "cccd(0x2902) not found");
        }
        return 0;
    }
    if (dsc != NULL && dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
        dsc->uuid.u16.value == BLE_REMOTE_CCCD_UUID16) {
        s_cccd_handle = dsc->handle;
    }
    return 0;
}

/* 步骤1：发现特征完成后，发现描述符 */
static int on_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status != 0) {
        if (s_char_val_h != 0) {
            ESP_LOGI(LOG_TAG, "notify char found: val_h=0x%x", s_char_val_h);
            s_cccd_handle = 0;
            ble_gattc_disc_all_dscs(conn_handle, s_char_val_h, s_svc_end_h,
                                    on_dsc, NULL);
        } else {
            ESP_LOGE(LOG_TAG, "notify char(0x%04X) not found", BLE_REMOTE_NOTIFY_CHAR_UUID16);
        }
        return 0;
    }
    if (chr != NULL && chr->uuid.u.type == BLE_UUID_TYPE_16 &&
        chr->uuid.u16.value == BLE_REMOTE_NOTIFY_CHAR_UUID16) {
        s_char_val_h = chr->val_handle;
    }
    return 0;
}

/* 步骤0：发现服务完成后，发现特征 */
static int on_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg)
{
    if (error->status != 0) {
        if (s_svc_start_h != 0) {
            ESP_LOGI(LOG_TAG, "target service found: 0x%04X, start=0x%x end=0x%x",
                     BLE_REMOTE_SERVICE_UUID16, s_svc_start_h, s_svc_end_h);
            s_char_val_h = 0;
            ble_gattc_disc_chrs_by_uuid(conn_handle, s_svc_start_h, s_svc_end_h,
                                        &s_char_uuid.u, on_chr, NULL);
        } else {
            ESP_LOGE(LOG_TAG, "target service(0x%04X) not found", BLE_REMOTE_SERVICE_UUID16);
        }
        return 0;
    }
    if (service != NULL) {
        s_svc_start_h = service->start_handle;
        s_svc_end_h   = service->end_handle;
    }
    return 0;
}

/* ========================================================================
 *                         GAP 事件回调
 * ======================================================================== */

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    /* ---- 扫描结果 ---- */
    case BLE_GAP_EVENT_DISC: {
        struct ble_gap_disc_desc *disc = &event->disc;
        const uint8_t *adv_data = disc->data;
        uint8_t adv_len = disc->length_data;

        /* 解析设备名 */
        uint8_t name_len = 0;
        const uint8_t *name_p = adv_find_field(BLE_HS_ADV_TYPE_COMP_NAME,
                                                adv_data, adv_len, &name_len);
        if (!name_p) name_p = adv_find_field(BLE_HS_ADV_TYPE_INCOMP_NAME,
                                              adv_data, adv_len, &name_len);
        char dev_name[32] = {0};
        if (name_p && name_len) {
            memcpy(dev_name, name_p, name_len < sizeof(dev_name) - 1 ? name_len : sizeof(dev_name) - 1);
        }

        /* 解析 Service UUID */
        bool svc_match = false;
        uint8_t uuid_len = 0;
        const uint8_t *uuid_p = adv_find_field(BLE_HS_ADV_TYPE_COMP_UUIDS16,
                                               adv_data, adv_len, &uuid_len);
        if (!uuid_p) uuid_p = adv_find_field(BLE_HS_ADV_TYPE_INCOMP_UUIDS16,
                                              adv_data, adv_len, &uuid_len);
        if (uuid_p && uuid_len >= 2) {
            for (uint8_t i = 0; i + 1 < uuid_len; i += 2) {
                if ((uint16_t)(uuid_p[i] | (uuid_p[i + 1] << 8)) == BLE_REMOTE_SERVICE_UUID16) {
                    svc_match = true;
                    break;
                }
            }
        }

        bool name_match = (BLE_REMOTE_NAME_PREFIX[0] != '\0')
                          ? (name_p && strncmp(dev_name, BLE_REMOTE_NAME_PREFIX,
                                               strlen(BLE_REMOTE_NAME_PREFIX)) == 0)
                          : true;

        ESP_LOGI(LOG_TAG, "scan: rssi=%d addr=%02X:%02X:%02X:%02X:%02X:%02X "
                 "type=%s svc_match=%d name=\"%s\"",
                 disc->rssi,
                 disc->addr.val[0], disc->addr.val[1], disc->addr.val[2],
                 disc->addr.val[3], disc->addr.val[4], disc->addr.val[5],
                 addr_type_str(disc->addr.type), svc_match, dev_name);

        bool should_connect = (BLE_REMOTE_NAME_PREFIX[0] != '\0') ? name_match : svc_match;

        if (should_connect) {
            ESP_LOGI(LOG_TAG, ">>> target found!");
            ble_gap_disc_cancel();
            memcpy(&s_peer_addr, &disc->addr, sizeof(ble_addr_t));
            set_state(BLE_REMOTE_STATE_CONNECTING);

            struct ble_gap_conn_params conn_params = {
                .scan_itvl           = BLE_REMOTE_SCAN_INTERVAL,
                .scan_window         = BLE_REMOTE_SCAN_WINDOW,
                .itvl_min            = BLE_REMOTE_CONN_INTERVAL_MIN,
                .itvl_max            = BLE_REMOTE_CONN_INTERVAL_MAX,
                .latency             = BLE_REMOTE_CONN_LATENCY,
                .supervision_timeout = BLE_REMOTE_CONN_TIMEOUT,
            };
            ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &s_peer_addr, 30000,
                            &conn_params, gap_event_handler, NULL);
        }
        break;
    }

    /* ---- 扫描完成（超时） ---- */
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGW(LOG_TAG, "scan complete, reason=%d, restart...", event->disc_complete.reason);
        if (s_state == BLE_REMOTE_STATE_SCANNING) {
            start_scan();
        }
        break;

    /* ---- 连接结果 ---- */
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_is_connected = true;
            set_state(BLE_REMOTE_STATE_CONNECTED);
            ESP_LOGI(LOG_TAG, "connected, conn_handle=%d", s_conn_handle);
            if (s_conn_cb) s_conn_cb(BLE_REMOTE_CONN_CONNECTED);

            /* 更新连接参数 */
            struct ble_gap_upd_params upd_params = {
                .itvl_min            = BLE_REMOTE_CONN_INTERVAL_MIN,
                .itvl_max            = BLE_REMOTE_CONN_INTERVAL_MAX,
                .latency             = BLE_REMOTE_CONN_LATENCY,
                .supervision_timeout = BLE_REMOTE_CONN_TIMEOUT,
            };
            ble_gap_update_params(s_conn_handle, &upd_params);

            /* 开始服务发现 */
            set_state(BLE_REMOTE_STATE_DISCOVERING);
            s_svc_start_h = 0;
            s_svc_end_h = 0;
            ble_gattc_disc_svc_by_uuid(s_conn_handle, &s_svc_uuid.u, on_svc, NULL);
        } else {
            ESP_LOGE(LOG_TAG, "connect failed, status=%d", event->connect.status);
            set_state(BLE_REMOTE_STATE_IDLE);
            if (s_inited && s_reconnect_timer) {
                esp_timer_start_once(s_reconnect_timer, BLE_REMOTE_RECONNECT_DELAY_MS * 1000);
            }
        }
        break;

    /* ---- 断开 ---- */
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(LOG_TAG, "disconnected, reason=%d", event->disconnect.reason);
        s_is_connected = false;
        s_conn_handle = 0;
        s_char_val_h = 0;
        s_cccd_handle = 0;
        set_state(BLE_REMOTE_STATE_IDLE);
        if (s_conn_cb) s_conn_cb(BLE_REMOTE_CONN_DISCONNECTED);

        if (s_inited && s_reconnect_timer) {
            esp_timer_start_once(s_reconnect_timer, BLE_REMOTE_RECONNECT_DELAY_MS * 1000);
        }
        break;

    /* ---- 连接参数更新结果 ---- */
    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(LOG_TAG, "conn params updated, status=%d", event->conn_update.status);
        break;

    /* ---- 收到 Notify / Indicate ---- */
    case BLE_GAP_EVENT_NOTIFY_RX: {
        struct os_mbuf *om = event->notify_rx.om;
        uint16_t data_len = OS_MBUF_PKTLEN(om);
        if (data_len > BLE_REMOTE_MAX_DATA_LEN) data_len = BLE_REMOTE_MAX_DATA_LEN;

        uint8_t data[BLE_REMOTE_MAX_DATA_LEN];
        os_mbuf_copydata(om, 0, data_len, data);

        ESP_LOGI(LOG_TAG, ">>> %s recv, handle=0x%x, len=%d",
                 event->notify_rx.indication ? "INDICATE" : "NOTIFY",
                 event->notify_rx.attr_handle, data_len);

        char hexbuf[128] = {0};
        int pos = 0;
        for (uint16_t i = 0; i < data_len && pos < (int)sizeof(hexbuf) - 4; i++) {
            pos += snprintf(hexbuf + pos, sizeof(hexbuf) - pos, "%02X ", data[i]);
        }
        ESP_LOGI(LOG_TAG, "raw data=[%s]", hexbuf);

        if (s_key_cb) {
            ble_remote_key_event_t evt;
            evt.raw_len = data_len;
            memcpy(evt.raw, data, data_len);
            evt.key_code = ble_remote_parse_key(evt.raw, evt.raw_len);
            s_key_cb(&evt);
        }
        break;
    }

    /* ---- MTU 交换 ---- */
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(LOG_TAG, "mtu updated: conn_handle=%d, mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

/* ========================================================================
 *               NimBLE 主机同步 / 重置回调
 * ======================================================================== */

static void on_host_sync(void)
{
    ESP_LOGI(LOG_TAG, "nimble host synced, start scanning...");
    start_scan();
}

static void on_host_reset(int reason)
{
    ESP_LOGW(LOG_TAG, "nimble host reset, reason=%d", reason);
    s_is_connected = false;
    set_state(BLE_REMOTE_STATE_IDLE);
}

/* ========================================================================
 *                         NimBLE 主机任务
 * ======================================================================== */

static void host_task(void *arg)
{
    ESP_LOGI(LOG_TAG, "nimble host task started");
    nimble_port_run();
    ESP_LOGW(LOG_TAG, "nimble host task exited");
    nimble_port_freertos_deinit();
    vTaskDelete(NULL);
}

/* ========================================================================
 *                         公共 API
 * ======================================================================== */

esp_err_t ble_remote_init(void)
{
    if (s_inited) {
        ESP_LOGW(LOG_TAG, "already initialized");
        return ESP_OK;
    }

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) return ret;

    /* NimBLE 初始化（控制器 + 主机） */
    ESP_ERROR_CHECK(nimble_port_init());

    /* 设置主机同步/重置回调（GAP 回调在 disc/connect 时单独传入） */
    ble_hs_cfg.sync_cb  = on_host_sync;
    ble_hs_cfg.reset_cb = on_host_reset;

    /* 创建重连定时器 */
    esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name     = "ble_remote_reconn",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    /* 启动主机任务 */
    nimble_port_freertos_init(host_task);

    s_inited = true;

    ESP_LOGI(LOG_TAG, "init done (NimBLE), target: svc=0x%04X char=0x%04X cccd=0x%04X name=\"%s\"",
             BLE_REMOTE_SERVICE_UUID16, BLE_REMOTE_NOTIFY_CHAR_UUID16,
             BLE_REMOTE_CCCD_UUID16, BLE_REMOTE_NAME_PREFIX);
    return ESP_OK;
}

void ble_remote_deinit(void)
{
    if (!s_inited) return;
    s_inited = false;

    ESP_LOGI(LOG_TAG, "deinit...");

    /* 停止重连定时器 */
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }

    /* 断开连接 */
    if (s_is_connected) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_is_connected = false;
    }

    /* 停止扫描 */
    ble_gap_disc_cancel();

    /* 停止 NimBLE */
    nimble_port_stop();
    nimble_port_deinit();

    /* 重置状态 */
    s_state = BLE_REMOTE_STATE_IDLE;
    s_conn_handle = 0;
    s_svc_start_h = 0;
    s_svc_end_h = 0;
    s_char_val_h = 0;
    s_cccd_handle = 0;
    memset(&s_peer_addr, 0, sizeof(s_peer_addr));

    ESP_LOGI(LOG_TAG, "deinit done");
}

void ble_remote_register_key_callback(ble_remote_key_cb_t cb)
{
    s_key_cb = cb;
}

void ble_remote_register_state_callback(ble_remote_state_cb_t cb)
{
    s_state_cb = cb;
}

void ble_remote_register_conn_callback(ble_remote_conn_cb_t cb)
{
    s_conn_cb = cb;
}

ble_remote_state_t ble_remote_get_state(void)
{
    return s_state;
}

bool ble_remote_is_ready(void)
{
    return (s_state == BLE_REMOTE_STATE_NOTIFY_READY);
}
