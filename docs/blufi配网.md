BLUFI配网过程详解

1. 配网流程（从wifi_board.cc的EnterWifiConfigMode函数）

第一阶段：初始化准备

ble_active_ = true;  // 设置蓝牙为活跃状态
esp_read_mac(mac, ESP_MAC_WIFI_STA);  // 读取MAC地址
snprintf(blufi_device_name, sizeof(blufi_device_name), "BLUFI_DEVICE");  // 设置设备名

第二阶段：设置回调函数

sta_config_cb: 接收WiFi SSID和密码，保存到SsidManager
custom_data_cb: 接收自定义数据（如OTA URL）

第三阶段：WiFi状态检查和重启


esp_wifi_get_mode(&mode);  // 检查WiFi是否已初始化
if(wifi_initialized) {
    esp_wifi_stop();  // 停止WiFi
    vTaskDelay(pdMS_TO_TICKS(1000));  // 等待1秒
}

第四阶段：启动BLUFI

blufi_wificfg_start(!wifi_initialized, blufi_device_name, cbs, this);


第五阶段：等待连接和版本检查

等待获得IP地址
检查新版本
发送激活码
重启设备





使用ESP的APP，需要配置blufi_device_name为"BLUFI_DEVICE"，否则无法配网
snprintf(blufi_device_name, sizeof(blufi_device_name), "BLUFI_DEVICE");

使用思博则需要配置blufi_device_name为
snprintf(blufi_device_name, sizeof(blufi_device_name), "DTXZ_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
