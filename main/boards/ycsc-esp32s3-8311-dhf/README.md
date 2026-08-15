# ycsc-esp32s3-8311-dhf 编译说明

本板卡实现「BLE 蓝牙遥控」功能：作为 BLE 从机广播设备名 **大黄蜂**，接收遥控/手机写入的命令并打印日志。

## 一、板级配置

所有编译配置都在本目录的 [config.json](config.json) 里，由 `sdkconfig_append` 字段描述。编译时会自动追加到 `sdkconfig`，包括：

- 板型选择：`CONFIG_BOARD_TYPE_YCSC_ESP32S3_8311_DHF=y`
- BLE（NimBLE）配置：GATT 服务端、外设/广播角色等
- 其它板级硬件配置（LCD、SPIRAM、唤醒词等）

BLE 代码在 [ble_remote_control.cc](ble_remote_control.cc)，服务 UUID `0xFFE0`、写特征 UUID `0xFFE1`。

## 二、每次编译命令

在**项目根目录**（`xiaozhi-esp32-DHF`）执行：

```powershell
python scripts/release.py ycsc-esp32s3-8311-dhf --name ycsc-esp32s3-8311-dhf
```

脚本会自动完成：

1. `idf.py set-target esp32s3`
2. 把 `config.json` 的 `sdkconfig_append` 追加到 `sdkconfig`
3. `idf.py build`
4. `merge-bin` 并打包 zip

> 前提：`idf.py` 环境已激活（ESP-IDF 终端或执行过 `export.ps1`）。

## 三、编译产物

- 固件 zip：`releases/v{版本}_ycsc-esp32s3-8311-dhf.zip`（内含 `merged-binary.bin`）
- 也可直接用 `build/` 目录下的固件烧录

## 四、重新编译（重要）

`release.py` 会**跳过已存在的产物 zip**。要重新编译，先删除旧 zip：

```powershell
Remove-Item releases/v*_ycsc-esp32s3-8311-dhf.zip
python scripts/release.py ycsc-esp32s3-8311-dhf --name ycsc-esp32s3-8311-dhf
```

## 五、彻底干净重编（切过板型 / 构建异常时）

因为 `sdkconfig_append` 是**追加**到 `sdkconfig`（不清理旧值），切换过其它板型或构建异常时先 fullclean：

```powershell
idf.py fullclean
python scripts/release.py ycsc-esp32s3-8311-dhf --name ycsc-esp32s3-8311-dhf
```

## 六、烧录与查看日志

```powershell
idf.py -p COMx flash monitor
```

把 `COMx` 换成实际串口号。

## 七、BLE 功能验证

1. 上电后广播设备名「大黄蜂」
2. 用手机（如 nRF Connect）或遥控连接
3. 向特征 `0xFFE1`（服务 `0xFFE0`）写入数据
4. 串口日志会打印收到的命令（HEX + 可打印文本）

> 如果遥控器用的是其它服务/特征 UUID，需要修改 [ble_remote_control.cc](ble_remote_control.cc) 里的 `GATTS_SERVICE_UUID` / `GATTS_CHAR_UUID`。
