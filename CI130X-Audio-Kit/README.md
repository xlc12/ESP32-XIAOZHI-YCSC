# CI130X 音频移植套件 · 使用预览

> 一句话：把 CI1302/CI1306 当成 I2S 从机 + UART 控制的「智能 Codec」接进小智，并能拆成组件复用到其他工程。

## 1. 怎么使用这个 Skill

**方式 A：直接对话（推荐）**

在 Trae 里描述任务即可，Skill 会按描述自动触发，例如：

- “把 CI1302 接到一块新的 ESP32-S3 板子上”
- “CI130X 没有声音 / 唤醒词重复 / 播报后重启，帮我排查”
- “把 ci130x 音频方案拆成 ESP-IDF 组件”

也可以显式点名：`用 ci130x-audio-port 技能，帮我把这块板子接上 CI130X`。

**方式 B：装到其他工程**

1. 把 `skill/ci130x-audio-port/` 整个目录复制到目标工程的 `.trae/skills/` 下；
2. 重启/刷新 Trae，确认 Skill 列表里出现 `ci130x-audio-port`；
3. 之后在该工程里按方式 A 使用。

**Skill 会做什么**：读代码 → 按顺序改 5 处（引脚、协议、Codec、状态同步、唤醒桥接）→ 按验证顺序排查，不会一上来大改。

## 2. 怎么查看这些文档

| 文档 | 什么时候看 |
|---|---|
| [报告-CI130X音频移植与调试.md](./报告-CI130X音频移植与调试.md) | 第一次接入：接线、I2S 参数、协议、问题时间线、日志速查 |
| [报告-CI130X组件化与可移植性.md](./报告-CI130X组件化与可移植性.md) | 想复用/打包：耦合度分析、三层架构、组件代码、重构步骤 |
| [skill/.../SKILL.md](./skill/ci130x-audio-port/SKILL.md) | 操作流程总纲：移植步骤、I2S 参数、必踩的坑 |
| [references/protocol.md](./skill/ci130x-audio-port/references/protocol.md) | 查命令字、帧格式、校验、应答 |
| [references/troubleshooting.md](./skill/ci130x-audio-port/references/troubleshooting.md) | 出问题时对照现象与日志 |
| [references/packaging.md](./skill/ci130x-audio-port/references/packaging.md) | 组件目录、CMake、API、适配器与胶水实现 |
| [references/porting.md](./skill/ci130x-audio-port/references/porting.md) | 新板/新工程的逐项 Checklist |

**建议阅读顺序**：`SKILL.md` → `references/porting.md` → 调试报告 →（需要复用时）组件化报告。

## 3. 开工前要准备什么

**硬件**

- [ ] CI1302/CI1306 板与目标 ESP32-S3 板；
- [ ] I2S 四根线走向：CI SCLK/LRCLK/SDO/SDI ↔ ESP32 BCLK/WS/DIN/DOUT（MCLK 不接）；
- [ ] UART 两组线（TX/RX 交叉）+ 波特率（默认 921600）；
- [ ] 功放控制方式：CI 的 PC4 + `0x0119`，还是 ESP32 GPIO + 有效电平；
- [ ] 扬声器/麦克风结构，尽量避开强反馈。

**CI 侧固件**

- [ ] `IIS_UPLOAD_IS_WAKEUP`、`IIS_DOWNLOAD_BY_CMD` 已开启；
- [ ] 命令表与 `ci130x_protocol.h` 一致；
- [ ] 唤醒词/提示音配置确认。

**工程环境**

- [ ] ESP-IDF 5.x，当前工程能正常 `idf.py build`；
- [ ] 备份现有 `sdkconfig`；
- [ ] 明确 AEC 策略：`USE_SERVER_AEC` 或 `USE_DEVICE_AEC` 二选一；
- [ ] 明确唤醒策略：CI 唤醒 / AFE 唤醒，避免重复；
- [ ] 能看串口日志（建议全局 ERROR，只放行应用、Codec、协议三个 TAG）。

**信息清单**

- [ ] CI 与 ESP32 的引脚对照表；
- [ ] 采样率（本方案固定 16 kHz，输入输出必须相等）；
- [ ] 目标板的 UART 占用情况（避开 UART0 控制台、Flash/PSRAM 引脚）。

准备齐后，直接对 Trae 说：“按 ci130x-audio-port 把这块板子接上 CI130X”，或先看 `references/porting.md` 自己走一遍。
