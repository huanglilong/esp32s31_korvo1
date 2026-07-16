# ESP32-S31 Korvo-1 — 项目需求文档

> 生成日期: 2026-07-13 | 基于 ESP32-S31-Korvo-1 V1.1 硬件
>
> 本文档是**需求规划与变更记录**的唯一参考。软件/架构/实现细节见 [PROJECT.md](PROJECT.md)，硬件规格见 [README.md](README.md)。随项目迭代持续更新。

---

## 1. 项目愿景

基于 Espressif ESP32-S31-Korvo-1 V1.1 开发板构建一个智能音频 HMI 终端，集成双麦克风语音识别、双声道音乐播放、SD 卡存储、WiFi/BLE 无线连接，支持可选的 LCD 显示和摄像头采集。以单一固件覆盖智能音箱、音乐播放器、桌面助手等多种应用场景。

---

## 2. 计划需求

### 2.1 音频功能

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| A1 | **Audio 驱动基础** | ES8389 编解码器初始化 (I2S + I2C), 双 Mic + 双 Speaker 通路建立。⚠️ MCLK 不可用，采样率 16kHz | ✅ 已完成 |
| A2 | **Audio 录音** | 双 Mic → ES8389 ADC → I2S RX → MP3 编码 → SD 卡存储 | ✅ 已完成 |
| A3 | **Audio 播放** | SD 卡 → MP3 解码 → I2S TX → ES8389 DAC → NS4150B PA → 扬声器 | ✅ 已完成 |
| A4 | **Audio 音量控制** | ES8389 硬件音量调节, 按键 (VOL+/VOL-) 和 API 控制, NVS 持久化 | ✅ 已完成 |
| A5 | **语音唤醒 (Wake Word)** | ESP-Skainet 本地语音唤醒引擎, 低功耗后台运行 | ⏳ 待开发 |
| A6 | **语音命令识别** | ESP-Skainet 中文命令词识别, 本地离线处理 | ⏳ 待开发 |
| A7 | **TTS 语音合成** | 文字转语音输出, 支持中文 | ⏳ 待开发 |
| A8 | **Bluetooth Audio** | 蓝牙经典 A2DP Sink/Source, 蓝牙 LE Audio | ⏳ 待开发 |

### 2.2 存储功能

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| B1 | **SD 卡挂载** | SDIO 3.0 4-bit 模式, FAT 文件系统, boot 时挂载 | ✅ 已完成 |
| B2 | **SD 卡文件管理** | 文件浏览/删除/信息, Web API 或 UI 操作 | ✅ 已完成 |
| B3 | **SD 卡录音存储** | MP3/WAV 录音文件自动命名保存到 SD 卡 | ⏳ 待开发 |

### 2.3 显示功能 (可选)

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| C1 | **LCD 显示** | ESP32-S3-LCD-EV-Board-SUB3 4.3" 子板驱动, LVGL UI。通过 BSP `bsp_display_start()` 初始化 | ✅ 已完成 |
| C2 | **HMI 图形界面** | ESP-Brookesia Phone UI 桌面 + 自定义 App (音乐播放器/设置/录音) | ⏳ 待开发 |
| C3 | **触屏交互** | LCD 子板 GT1151 触屏支持, LVGL 触控事件。通过 BSP `bsp_display_start()` 自动初始化 | ✅ 已完成 |

### 2.4 按键与 LED

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| D1 | **按键驱动** | PLAY/SET/VOL-/VOL+ 四按键 ADC 输入, 消抖, 长短按识别。通过 BSP `bsp_iot_button_create()` 初始化 | ✅ 已完成 |
| D2 | **RGB LED 指示** | GPIO37 WS2812, 状态指示 (WiFi 连接/录音/唤醒/待机 不同颜色)。通过 BSP `bsp_led_indicator_create()` 初始化 | ✅ 已完成 |
| D3 | **按键控制逻辑** | PLAY: 播放/暂停, SET: 模式切换, VOL±: 音量调节 | ⏳ 待开发 |

### 2.5 摄像头功能 (可选)

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| E1 | **DVP 摄像头驱动** | OV3660 初始化, DVP 并行接口图像采集 | ✅ 已完成 (driver only) |
| E2 | **Camera App (LCD 预览)** | Camera 图像实时在 LCD 上显示 (V4L2 + LVGL canvas) | ✅ 已完成 |
| E3 | **JPEG 编码** | 硬件 JPEG Codec, 图像压缩存储 | ⏳ 待开发 |
| E4 | **人脸检测** | 可选 ESP-DL 人脸检测模型 | ⏳ 待开发 |

### 2.6 网络功能

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| F1 | **Wi-Fi 连接** | Wi-Fi 6 Station 模式, Web 配网/NVS 持久化 | ✅ 已完成 |
| F2 | **BLE 通信** | BLE MESH / BLE AUDIO，手机 APP 控制 | ⏳ 待开发 |
| F3 | **Web 配置服务器** | HTTP 服务器, WiFi/音量/设备设置, 文件管理 | ✅ 已完成 |
| F4 | **OTA 固件升级** | Wi-Fi OTA 远程升级, HTTP/HTTPS | ⏳ 待开发 |
| F5 | **802.15.4 组网** | Thread/Zigbee/Matter 智能家居协议 | ⏳ 待开发 |

### 2.7 USB 功能

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| G1 | **USB Host** | USB Type-A 端口, 支持 U 盘/外设 | ⏳ 待开发 |
| G2 | **USB 音频** | USB Audio Class, USB 麦克风/耳机 | ⏳ 待开发 |
| G3 | **USB Serial/JTAG** | USB 串口调试和 JTAG 在线调试 | ⏳ 待开发 |

### 2.8 系统 & AI

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| H1 | **AI Agent** | ESP-Claw AI Agent 集成 (LLM/MCP 工具调用/IM) | ⏳ 待开发 |
| H2 | **系统监控** | CPU/内存使用率, FreeRTOS 任务监控, 资源告警 | ✅ 已完成 |
| H3 | **电源管理** | Deep-sleep 语音唤醒, 低功耗模式切换 | ⏳ 待开发 |
| H4 | **uORB 消息总线** | PX4 风格 pub/sub 进程间通信 (可选) | ✅ 已完成 |
| H5 | **ULog 日志** | 二进制日志格式, SD 卡存储, Web API 启停控制 | ✅ 已完成 |

---

## 3. 稳定性与性能要求

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| S1 | **ES8389 驱动稳定性** | I2S + I2C 通信可靠, 采样率切换无缝, 无爆音。MCLK-less 模式 (BCLK 作为主时钟) | ⏳ 待开发 |
| S2 | **双核任务分配** | HP 核处理音频/识别, LP 核处理后台任务, 避免音频卡顿 | ⏳ 待开发 |
| S3 | **音频低延迟** | Mic→Speaker 环回延迟 <20ms, 语音唤醒延迟 <500ms | ⏳ 待开发 |
| S4 | **SD 卡可靠性** | 读写错误重试, 热插拔检测, FAT 文件系统一致性 | ⏳ 待开发 |
| S5 | **WiFi/BT 共存** | Wi-Fi 和 Bluetooth 共享天线, RF 共存策略 | ⏳ 待开发 |
| S6 | **内存管理** | PSRAM 合理分配, 避免内存碎片, OOM 保护 | ⏳ 待开发 |
| S7 | **跨核线程安全** | `std::atomic<T>` 保护跨 HP/LP 核共享变量 | ⏳ 待开发 |
| S8 | **Web API 自动化测试** | pytest 集成测试, 覆盖 WiFi/Audio/Files/ULog/System 全部 REST API 端点 | ✅ 已完成 |

---

## 4. 变更记录

| 日期 | 版本 | 变更内容 |
|------|------|----------|
| 2026-07-13 | v0.1 | 初始版本, 基于 ESP32-S31-Korvo-1 V1.1 硬件规划需求 |
| 2026-07-13 | v0.2 | 创建项目脚手架 + uORB/ULog 组件 + Logger + WiFi Service + Web Config Server + Audio/SD/Camera/SystemMonitor 驱动 + main.cpp 启动序列。Build 通过 (ESP-IDF v6.1-beta1)。 |
| 2026-07-13 | v0.3 | Code Review: 修复 esp_netif_init() 重复调用, 禁用 SPIRAM_XIP_FROM_PSRAM, 确认硬件兼容性 (ES8389/SDIO/WiFi/Camera) |
| 2026-07-13 | v0.4 | SNTP 时间同步: 实现 WiFi STA 连接后自动 NTP 对时 (pool.ntp.org), 时区支持 (NVS 持久化 + Web API GET/POST /api/system/timezone), ULog 在 SNTP 同步后自动启动, 系统信息 API 增加 timezone/current_time 字段 |
| 2026-07-14 | v0.4.1 | Git info 写入 log + ulog: main.cpp 启动时打印完整 git 信息 (branch/commit/author/date/message), text logger SD 卡文件头写入 git 版本信息, ulog 已有 git info 支持 (ver_sw_branch/commit/author/date/msg) |
| 2026-07-14 | v0.5 | SNTP 修复: 修复 s_timezone 数据竞争 (SNTP 回调/HTTP handler 并发读取), 修复 Kconfig 默认时区不一致 (UTC0→CST-8), 添加 STA 关联/DHCP/IP 获取诊断日志, 添加 IP_EVENT_STA_LOST_IP 处理, 系统信息显示 NTP 同步状态和当前时间 |
| 2026-07-14 | v0.5.1 | ULog 延迟启动: ULog 不再在 boot 时立即 start, 改为 SNTP 时间同步完成后由 web_config_task 自动 start (参考 esp32p4_monitor 实现)。确保 ULog 文件获得正确的 wall-clock 时间戳和日期命名。main.cpp 只做 init + add_topic, 不调用 ulog_writer_start()。 |
| 2026-07-14 | v0.6 | **Driver refactoring: esp_board_manager API pattern**. AudioDriver now uses `esp_codec_dev` (ES8389 via `es8389_codec_new`, `esp_codec_dev_new`, etc.) instead of raw I2S+I2C+register access. SDCardDriver and CameraDriver refactored to config-driven `init(cfg, cfg_size, handle)` API matching esp_board_manager device pattern (`dev_fs_fat_config_t`, `dev_camera_config_t` with DVP sub-type). Added `espressif/esp_codec_dev` component dependency. Build passes (IDF v6.1-beta1). |
| 2026-07-14 | v0.6.1 | **Fix GPIO 41 invalid**: GPIO 41 excluded on ESP32-S31 (BIT41 in SOC_GPIO_VALID_GPIO_MASK). Changed I2C SDA from GPIO 41→39. Removed duplicate macros in app_config.h. Added thread-safety protocol to AudioDriver (_codec_mutex + _codec_ops_in_flight, from P4-Monitor pattern). |
| 2026-07-14 | v0.7.1 | **Display + Touch 驱动适配**: 新增 DisplayDriver, 基于 BSP `bsp_display_start()` 集成 LCD (RGB 800x480) + LVGL + GT1151 Touch。`bsp_display_lock/unlock` 提供线程安全 LVGL 访问。Display 为可选外设, 未连接时优雅跳过。Build 通过。 |
| 2026-07-14 | v0.8 | **Web Config Server 功能扩展**: 参考 esp32p4_monitor, 新增 Audio Recording API (/api/audio/record_start|stop|status, I2S RX → shine MP3 → SD), Music Playback API (/api/audio/list|play|stop, esp_audio_simple_player), File Manager API (/api/files/list|download|delete|delete_batch), ULog Control API (/api/ulog/status|start|stop)。Web UI 重构为 4 标签页 (WiFi/Audio/Files/System), 双模式 (Audio → 录音+播放, Files → 文件管理)。新增 shine_encoder 本地组件 (+extern "C" 头文件修复), esp_audio_simple_player 依赖。Build 通过。 |
| 2026-07-14 | v0.8.1 | **Bug 修复**: (1) File Manager: 修复文件夹无法打开的问题 — JS onclick handler 区分目录(导航)和文件(切换选择) (2) Audio Recording 0-byte 文件: 修复 audio_task 使用 I2S 直接读取导致无数据 — 改为通过 AudioDriver::codec_read() (esp_codec_dev_read) 读取 BSP 管理的 I2S RX 通道, 避免 BSP 私有 I2S handle 无法获取的问题。Build 通过。 |
| 2026-07-15 | v0.9 | **Web UI 交互优化**: (1) WiFi: 自动低频 Scan (10s), Select→Connect 绿色按钮, 密码弹窗 (Enter 提交, 过滤功能键), 移除 Scan 按钮和 WiFi Connect 子页面 (2) Audio: Record+▶/■ 同一行, Recording/Stopped 状态+文件路径+大小分行显示, Music Player 自动刷新 (5s), ▶/■ 图标按钮 (3) Files: 删除/下载更新状态栏 (4) System: System Info 自动刷新 (5s), Volume 实时滑块+NVS 防抖 (?save=false), ULog Record+▶/■ 同一行, Running→Recording, 状态+文件路径+大小分行显示, 自动刷新 (3s) (5) 默认按钮蓝色, 状态文字蓝色, Start/Stop 用 ▶/■ 图标。record_status API 新增 file 字段。Build 通过。 |
| 2026-07-15 | v0.9.1 | **pytest 集成测试**: 参考 esp32p4_monitor/tests/ 方案, 新增 tests/ 目录, 覆盖 Web Config Server 全部 REST API 端点。conftest.py 提供 base_url/client/api fixture (--base-url CLI / ESP_BASE_URL env / mDNS 默认), device_info 自动打印设备信息。test_wifi.py (scan/connect/status), test_audio.py (volume/record/play/list/stop), test_files.py (list/download/delete/delete_batch + path traversal 防护), test_ulog.py (status/start/stop lifecycle), test_system.py (info/stats/timezone/sdcard)。 |
| 2026-07-15 | v0.10.1 | **Camera App 修复 + PPA 加速**: (1) 修复 OV3660 传感器未检测: 添加 `CONFIG_CAMERA_OV3660=y` 到 sdkconfig.defaults, 否则 esp_cam_sensor 不编译驱动。(2) 修复 RGB565 颜色错误: 检测 V4L2 RGB565X 格式并映射到 LV_COLOR_FORMAT_RGB565_SWAPPED。(3) **PPA 硬件加速**: 使用 PPA SRM (scale-rotate-mirror) 将 640x480 缩放+居中到 800x480 全屏, 替代 CPU for-loop 像素拷贝。(4) P2 DMA2D: LVGL DMA2D 仅支持 STM32; ESP32-S31 使用 SW 渲染。(5) P3 JPEG: CONFIG_ESP_VIDEO_ENABLE_HW_JPEG 因 esp_lvgl_port CONFIGDEP bug 暂不可用。相机稳定运行: ~11 fps, heap 10798 KB。 |
| 2026-07-16 | v0.10.3 | **Migrate to official AAC/ADTS recording**: (1) Replace WAV/shine_encoder with official `esp_audio_enc` AAC encoder (ADTS format, .aac files). (2) AAC config: 16kHz stereo 64kbps, adts_used=true for raw playable files. (3) Encoder frame buffer management: `esp_audio_enc_get_frame_size` + accumulation loop (4096B in → 1736B out). (4) `esp_audio_simple_player` natively supports .aac playback. (5) Remove shine_encoder component (~2900 lines). Recording test: 6s→43KB AAC (~57kbps)。Build + Flash 通过。 |
| 2026-07-16 | v0.10.4 | **Web Server 状态更新修复**: (1) 7 个 JSON API handler 缺少 `Content-Type: application/json` header (record_start/stop/status, play, stop, files_delete, files_delete_batch), ESP-IDF 默认 text/html 导致客户端解析异常。(2) `_api_stop` 未 destroy `s_asp` handle, 导致资源泄漏。(3) 新增 `GET /api/audio/play_status` 端点, 返回 `{playing: bool}`, 供 pytest 和 Web UI 权威状态查询。(4) Web UI Music Player 自动刷新时轮询 play_status, 播放自然结束后 UI 自动更新为 Stopped。(5) pytest test_audio.py: .mp3→.aac/.wav/.mp3 适配 (v0.10.3 格式变更), 新增 play_status 测试, play+stop 后验证状态。Build 通过。 |
| 2026-07-16 | v0.10.5 | **Fix mic gain not applied**: `AudioDriver::init()` 中 `adc_init_gain=40` 被配置但从未通过 `set_mic_gain()` 应用到硬件，导致录音音量很小。在 `init()` 中添加 `set_mic_gain(cfg->adc_init_gain)` 调用修复。Build 通过。 |
| 2026-07-16 | v0.10.6 | **Web UI 按钮响应优化 — 消除点击延迟**: (1) 命令 API 返回完整状态: record_start→{ok,recording,file}, record_stop→{ok,recording,file,bytes}, play→{ok,playing,file}, stop→{ok,playing}, ulog/start→{ok,running,filepath,bytes_written}, ulog/stop→{ok,running,filepath,bytes_written}。客户端无需二次轮询即可更新 UI。(2) 乐观 UI 更新: 点击按钮时立即更新 UI 状态 (按钮图标/文字/颜色), 命令返回后从响应同步精确状态, 失败时回退。提取 applyRecStatus/applyPlayStatus/applyUlogStatus 统一状态渲染函数。(3) 消除冗余 refreshRecStatus/refreshUlogStatus 二次调用, 减少网络往返。Build 通过。 |
| 2026-07-16 | v0.11.0 | **Code Review Round 1+2 — 16 fixes**: (1) Fix malformed JSON in _api_rec_start error responses (missing closing brace). (2) Fix TOCTOU race in _api_rec_start playback stop (remove double re-check). (3) Fix JSON injection in play_status/record_status (use cJSON instead of snprintf). (4) Fix race condition on s_timezone_mutex creation (use std::atomic CAS). (5) Fix s_enc_in_count not atomic (std::atomic<int>). (6) Fix misleading EXAMPLE_AUDIO_SAMPLE_RATE=48000 (→16000) and MCLK macros. (7) Fix hardcoded PSRAM=16 (→heap_caps_get_total_size) and broken sdcard info API (→f_getfree). (8) Fix s_timezone read without mutex in _web_config_task (snapshot under mutex). (9) Fix WiFi password length logged (info leak, removed). (10) Fix HTTP header injection via filename in Content-Disposition (strip quotes/CR/LF). (11) Fix missing Content-Length validation on 3 POST handlers (timezone_set, volume_set, files_delete). (12) Fix s_timezone read after mutex unlock in _api_timezone_set (snapshot before unlock). (13) Fix s_rec_path read after mutex unlock in _api_rec_start (snapshot before unlock). (14) Fix s_mdns_mutex creation TOCTOU race (use std::atomic CAS). (15) Fix s_audio_tcb never freed in web_config_server_stop (~340 bytes internal RAM leak). (16) Remove redundant s_playing/s_playing_file reset in _api_play. Build 通过。 |
