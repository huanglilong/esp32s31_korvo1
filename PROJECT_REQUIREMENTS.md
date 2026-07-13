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
| A1 | **Audio 驱动基础** | ES8389 编解码器初始化 (I2S + I2C), 双 Mic + 双 Speaker 通路建立 | ✅ 已完成 |
| A2 | **Audio 录音** | 双 Mic → ES8389 ADC → I2S RX → MP3/WAV 编码 → SD 卡存储 | ⏳ 待开发 |
| A3 | **Audio 播放** | SD 卡 → MP3/WAV 解码 → I2S TX → ES8389 DAC → NS4150B PA → 扬声器 | ⏳ 待开发 |
| A4 | **Audio 音量控制** | ES8389 硬件音量调节, 按键 (VOL+/VOL-) 和 API 控制, NVS 持久化 | ⏳ 待开发 |
| A5 | **语音唤醒 (Wake Word)** | ESP-Skainet 本地语音唤醒引擎, 低功耗后台运行 | ⏳ 待开发 |
| A6 | **语音命令识别** | ESP-Skainet 中文命令词识别, 本地离线处理 | ⏳ 待开发 |
| A7 | **TTS 语音合成** | 文字转语音输出, 支持中文 | ⏳ 待开发 |
| A8 | **Bluetooth Audio** | 蓝牙经典 A2DP Sink/Source, 蓝牙 LE Audio | ⏳ 待开发 |

### 2.2 存储功能

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| B1 | **SD 卡挂载** | SDIO 3.0 4-bit 模式, FAT 文件系统, boot 时挂载 | ✅ 已完成 |
| B2 | **SD 卡文件管理** | 文件浏览/删除/信息, Web API 或 UI 操作 | ⏳ 待开发 |
| B3 | **SD 卡录音存储** | MP3/WAV 录音文件自动命名保存到 SD 卡 | ⏳ 待开发 |

### 2.3 显示功能 (可选)

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| C1 | **LCD 显示** | ESP32-S3-LCD-EV-Board-SUB3 4.3" 子板驱动, LVGL UI | ⏳ 待开发 |
| C2 | **HMI 图形界面** | ESP-Brookesia Phone UI 桌面 + 自定义 App (音乐播放器/设置/录音) | ⏳ 待开发 |
| C3 | **触屏交互** | LCD 子板触屏支持, LVGL 触控事件 | ⏳ 待开发 |

### 2.4 按键与 LED

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| D1 | **按键驱动** | PLAY/SET/VOL-/VOL+ 四按键 GPIO 输入, 消抖, 长短按识别 | ⏳ 待开发 |
| D2 | **RGB LED 指示** | GPIO8 WS2812, 状态指示 (WiFi 连接/录音/唤醒/待机 不同颜色) | ⏳ 待开发 |
| D3 | **按键控制逻辑** | PLAY: 播放/暂停, SET: 模式切换, VOL±: 音量调节 | ⏳ 待开发 |

### 2.5 摄像头功能 (可选)

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| E1 | **DVP 摄像头驱动** | OV3660 初始化, DVP 并行接口图像采集 | ✅ 已完成 (driver only) |
| E2 | **JPEG 编码** | 硬件 JPEG Codec, 图像压缩存储 | ⏳ 待开发 |
| E3 | **人脸检测** | 可选 ESP-DL 人脸检测模型 | ⏳ 待开发 |

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
| H5 | **ULog 日志** | 二进制日志格式, SD 卡存储 (可选) | ✅ 已完成 |

---

## 3. 稳定性与性能要求

| # | 需求 | 说明 | 状态 |
|---|------|------|:----:|
| S1 | **ES8389 驱动稳定性** | I2S + I2C 通信可靠, 采样率切换无缝, 无爆音 | ⏳ 待开发 |
| S2 | **双核任务分配** | HP 核处理音频/识别, LP 核处理后台任务, 避免音频卡顿 | ⏳ 待开发 |
| S3 | **音频低延迟** | Mic→Speaker 环回延迟 <20ms, 语音唤醒延迟 <500ms | ⏳ 待开发 |
| S4 | **SD 卡可靠性** | 读写错误重试, 热插拔检测, FAT 文件系统一致性 | ⏳ 待开发 |
| S5 | **WiFi/BT 共存** | Wi-Fi 和 Bluetooth 共享天线, RF 共存策略 | ⏳ 待开发 |
| S6 | **内存管理** | PSRAM 合理分配, 避免内存碎片, OOM 保护 | ⏳ 待开发 |
| S7 | **跨核线程安全** | `std::atomic<T>` 保护跨 HP/LP 核共享变量 | ⏳ 待开发 |

---

## 4. 变更记录

| 日期 | 版本 | 变更内容 |
|------|------|----------|
| 2026-07-13 | v0.1 | 初始版本, 基于 ESP32-S31-Korvo-1 V1.1 硬件规划需求 |
| 2026-07-13 | v0.2 | 创建项目脚手架 + uORB/ULog 组件 + Logger + WiFi Service + Web Config Server + Audio/SD/Camera/SystemMonitor 驱动 + main.cpp 启动序列。Build 通过 (ESP-IDF v6.1-beta1)。 |
| 2026-07-13 | v0.3 | Code Review: 修复 esp_netif_init() 重复调用, 禁用 SPIRAM_XIP_FROM_PSRAM, 确认硬件兼容性 (ES8389/SDIO/WiFi/Camera) |
| 2026-07-14 | v0.4 | Git info 写入 log + ulog: main.cpp 启动时打印完整 git 信息 (branch/commit/author/date/message), text logger SD 卡文件头写入 git 版本信息, ulog 已有 git info 支持 (ver_sw_branch/commit/author/date/msg) |
