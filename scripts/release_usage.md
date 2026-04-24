# Release.py 使用说明

## 概述

`scripts/release.py` 用于编译和打包 ESP32 固件。

- 默认不打包
- 使用 `--zip` 参数打包
- 打包时包含所有 bin 文件

## 命令行参数

| 参数 | 说明 |
|------|------|
| `board` | 板型名称或 'all' |
| `-c, --config` | 配置文件名（默认：config.json） |
| `--list-boards` | 列出所有支持的板型 |
| `--json` | 以 JSON 格式输出（配合 --list-boards） |
| `--name` | 指定要编译的变体名称 |
| `--zip` | 创建 ZIP 文件 |

## 使用示例

### 1. 列出所有板型

```bash
python scripts/release.py --list-boards
```

### 2. 编译指定板型（不打包）

```bash
python scripts/release.py m5stack-core-s3
```

### 3. 编译指定板型并打包

```bash
python scripts/release.py m5stack-core-s3 --zip
```

### 4. 编译指定板型的特定变体

```bash
python scripts/release.py waveshare/esp32-p4-nano --name esp32-p4-nano-10.1-a
```

### 5. 编译所有板型

```bash
python scripts/release.py all
```

### 6. 编译所有板型并打包

```bash
python scripts/release.py all --zip
```

### 7. 打包当前已编译的固件

```bash
python scripts/release.py --zip
```

## 输出文件

### 编译输出
- `build/merged-binary.bin` - 合并后的完整固件
- `build/*.bin` - 所有单独的 bin 文件

### 打包输出
- `releases/{板名}-v{版本}-{日期}.zip`
- ZIP 文件内容：
  - `merged-binary.bin` - 合并后的完整固件
  - `bin/` - 所有单独的 bin 文件

## 常见问题

### Q: 如何只编译不打包？

直接运行，默认不打包：
```bash
python scripts/release.py m5stack-core-s3
```

### Q: 如何打包？

添加 `--zip` 参数：
```bash
python scripts/release.py m5stack-core-s3 --zip
```

### Q: 如何查看某个板型支持哪些变体？

```bash
python scripts/release.py --list-boards
```

### Q: 打包的 ZIP 文件在哪里？

`releases/` 目录下。

### Q: 如何只打包不编译？

如果已经编译过，直接运行：
```bash
python scripts/release.py --zip
```

## 配置文件说明

### config.json 结构

```json
{
  "manufacturer": "waveshare",
  "target": "esp32s3",
  "builds": [
    {
      "name": "esp32-p4-nano-10.1-a",
      "sdkconfig_append": [
        "CONFIG_LV_DISPLAY_RESOLUTION_H=800",
        "CONFIG_LV_DISPLAY_RESOLUTION_V=480"
      ]
    }
  ]
}
```

### 字段说明

| 字段 | 说明 |
|------|------|
| `manufacturer` | 制造商名称 |
| `target` | ESP-IDF 目标芯片（esp32s3, esp32c3 等） |
| `builds` | 构建配置数组 |
| `builds[].name` | 变体名称 |
| `builds[].sdkconfig_append` | 要追加到 sdkconfig 的配置项 |