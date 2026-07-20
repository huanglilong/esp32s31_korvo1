# ESP32-S31 Korvo-1 Project Setup

> 📋 本文档是**软件/架构/实现**的唯一参考 (含硬件平台、驱动架构、FreeRTOS 任务调度、各模块实现细节)。
> 📋 需求清单与变更记录见 [PROJECT_REQUIREMENTS.md](PROJECT_REQUIREMENTS.md)。
> 📋 硬件接线与规格见 [README.md](README.md)。

## 项目概述
基于 ESP32-S31 + Espressif ESP32-S31-Korvo-1 V1.1 开发板的智能音频 HMI 项目, 集成:
- **Audio** 双模拟麦克风采集 + 双声道扬声器输出 (ES8389 立体声编解码器, I2S + I2C)
- **microSD** 卡 (SDIO 3.0 4-bit 模式, FAT 文件系统), 音频存储和播放
- **LCD** (可选) ESP32-S3-LCD-EV-Board-SUB3 4.3 英寸显示子板
- **Camera** (可选) OV3660 DVP 摄像头, 图像采集
- **UI** ESP-Brookesia (可选) 人机交互框架 + LVGL, 或按键式交互 (PLAY/SET/VOL-/VOL+)
- **AI Voice** ESP-Skainet 语音识别 + 语音唤醒
- **Multimedia** ESP-GMF 通用多媒体框架, 音视频处理管道

### 核心应用场景

| 场景 | 说明 |
|------|------|
| 🎤 **智能音箱** | 双 Mic 语音识别 + 语音唤醒 + 双声道音乐播放 |
| 🎵 **音乐播放器** | SD 卡 MP3/WAV 播放, 按键控制, RGB LED 状态 |
| 📷 **智能门禁** | DVP 摄像头 + 人脸识别 + LCD 显示 + 语音交互 |
| 🏠 **智能家居面板** | LCD HMI + 语音控制 + Wi-Fi/BLE/Zigbee/Thread 多协议 |
| 🤖 **桌面助手** | 语音 + 显示 + USB 外设, ESP-Claw AI Agent 集成 |

## 开发环境
- **芯片**: ESP32-S31 (RISC-V Dual-Core, 320 MHz)
- **模组**: ESP32-S31-WROOM-3 (16MB Flash + 16MB PSRAM)
- **ESP-IDF 版本**: v6.x
- **Flash**: 16MB (SPI)
- **PSRAM**: 16MB (Octal SPI, 封装内)

## 项目结构

```
esp32s31_korvo1/
├── CMakeLists.txt              # 顶层项目配置 + uORB 代码生成
├── sdkconfig.defaults          # 默认 Kconfig 配置
├── partitions.csv              # 分区表 (OTA 双分区 + storage)
├── main/
│   ├── CMakeLists.txt          # 主组件编译配置 (C++)
│   ├── idf_component.yml       # 组件依赖声明 (mdns, cjson)
│   ├── Kconfig.projbuild       # 项目 Kconfig 菜单 (SD/Audio/Camera/Logger/SysMon)
│   ├── app_config.h            # 引脚/参数/NVS 键/常量 宏定义
│   ├── topics.h                # uORB topic 汇总 + ORB_TOPIC_DECLARE
│   ├── main.cpp                # 主程序: NVS→SD→Audio→WiFi→WebConfig→Logger→ULog→SysMon
│   ├── wifi_service.hpp/cpp    # WifiService — Wi-Fi STA+AP + SNTP + mDNS
│   ├── web_config_server.hpp/cpp # Web 配置服务器 (port 8080 REST API + 4-tab Web UI: WiFi/Audio/Files/System)
│   ├── app_video.h / app_video.c  # V4L2 camera helper (from BSP display_camera_video example)
│   ├── drivers/
│   │   ├── audio/
│   │   │   ├── audio_driver.hpp    # AudioDriver — ES8389 codec via esp_codec_dev (esp_board_manager API pattern)
│   │   │   └── audio_driver.cpp
│   │   ├── sdcard/
│   │   │   ├── sdcard_driver.hpp   # SDCardDriver — SDIO 3.0 4-bit 卡管理 (init + detect_mount for Brookesia)
│   │   │   └── sdcard_driver.cpp
│   │   ├── camera/
│   │   │   ├── camera_driver.hpp   # CameraDriver — OV3660 DVP 摄像头
│   │   │   ├── camera_driver.cpp
│   │   │   ├── camera_app.hpp      # CameraApp — 摄像头 LCD 预览 (V4L2 + LVGL canvas)
│   │   │   └── camera_app.cpp
│   │   └── system_monitor/
│   │       ├── system_monitor.hpp  # SystemMonitor — CPU/内存采样 + uORB
│   │       └── system_monitor.cpp
│   ├── logger/
│   │   ├── logger.hpp        # Text Logger — ESP_LOG → ring buffer → SD card (含 git info 文件头)
│   │   └── logger.cpp
│   ├── generated/            # ⚠️ 自动生成 — 由 tools/msg_gen.py 从 proto/*.msg 生成
│   │   ├── wifi_state.h/cpp, volume_state.h/cpp, ...
│   │   └── uORBTopics.hpp/cpp
│   └── compat/               # 第三方兼容 shim
├── components/
│   ├── uorb/                 # PX4 风格 pub/sub 消息总线 (FreeRTOS queue)
│   │   ├── CMakeLists.txt, idf_component.yml
│   │   ├── include/uorb.h
│   │   └── uorb.c
│   └── ulog/                 # PX4 ULog 格式二进制日志 (SD 卡, 含 git info)
│       ├── CMakeLists.txt, idf_component.yml, Kconfig.projbuild
│       ├── include/ulog_writer.h, ulog_messages.h
│       └── ulog_writer.cpp
│   └── gen_bmgr_codes/       # ⚠️ 自动生成 — 先 `pip3 install esp-bmgr-assist`，再运行 `idf.py bmgr -b esp32_s31_korvo_1`，不纳入 Git
├── proto/                    # uORB topic 定义 (.msg, PX4 兼容)
│   ├── wifi_state.msg, volume_state.msg, ...
│   └── system_stats.msg, system_alert.msg
├── tools/
│   └── msg_gen.py            # .msg → C++ 代码生成器
├── tests/                    # pytest 集成测试 (Web Config Server REST API)
│   ├── conftest.py           # 共享 fixtures (base_url, client, api helper)
│   ├── requirements.txt      # pytest + requests 依赖
│   ├── test_wifi.py          # WiFi scan/connect/status 测试
│   ├── test_audio.py         # Audio volume/record/play/list 测试
│   ├── test_files.py         # File manager list/download/delete 测试
│   ├── test_ulog.py          # ULog status/start/stop 测试
│   └── test_system.py        # System info/stats/timezone/sdcard 测试
├── managed_components/       # ⚠️ ESP-IDF 管理 (禁止编辑)
├── doc/                      # 项目文档
├── README.md                 # 硬件信息 + 构建说明
├── PROJECT.md                # 软件/架构/实现 (本文档)
├── PROJECT_REQUIREMENTS.md   # 需求与变更记录
└── CLAUDE.md                 # Copilot 指令引用
```

## 硬件接线参考

### Audio (ES8389)

> ⚠️ **MCLK 未连接**: GPIO42 (I2S_MCLK) 在 ESP32-S31 上不可用。推荐采样率 **16kHz**。使用 BCLK 作为 ES8389 的主时钟源。

| Signal | Direction | ESP32-S31 GPIO | Description |
|:---|:---|:---|:---|
| I2C_SDA | ↔ | GPIO0 (strapping) | 配置数据 |
| I2C_SCL | ↔ | GPIO1 (strapping) | 配置时钟 |
| I2S_MCLK | ESP32-S31 → ES8389 | GPIO42 | 主时钟 (⚠️ 未连接) |
| I2S_SCLK | ↔ | GPIO3 | 位时钟 (BCLK) |
| I2S_LRCK | ↔ | GPIO4 | 左右声道时钟 (WS) |
| I2S_SDOUT | ← ES8389 | GPIO6 | ADC 数据 (麦克风) |
| I2S_DSDIN | ESP32-S31 → | GPIO5 | DAC 数据 (扬声器) |
| PA_CTRL | ESP32-S31 → NS4150B | GPIO7 | 功放使能 |
| MIC_L/R | → ES8389 analog in | — | 模拟麦克风输入 |
| PA_L/R | ← ES8389 → NS4150B | — | 功放输入 → 扬声器 |

### Buttons

| Button | ADC 分压 | Function |
|:---|:---|:---|
| PLAY | ~2.8V (13KΩ) | 播放/暂停 |
| SET | ~2.4V (6.8KΩ) | 设置/模式 |
| VOL- | ~1.8V (3.3KΩ) | 音量减小 |
| VOL+ | ~1.0V (1.3KΩ) | 音量增大 |

### RGB LED

| LED | GPIO | Description |
|:---|:---|:---|
| WS2812B RGB | GPIO37 → Q2 (电平转换) | 可寻址 RGB LED |

### microSD Card

| Signal | Interface | ESP32-S31 GPIO | Description |
|:---|:---|:---|:---|
| SD_CLK | SDIO CLK | GPIO24 | 时钟 |
| SD_CMD | SDIO CMD | GPIO25 | 命令 |
| SD_D0 | SDIO DATA0 | GPIO20 | 数据线 0 |
| SD_D1 | SDIO DATA1 | GPIO21 | 数据线 1 |
| SD_D2 | SDIO DATA2 | GPIO22 | 数据线 2 |
| SD_D3 | SDIO DATA3 | GPIO23 | 数据线 3 |

## 代码规范

- **C++** 用于 `main/` 应用层代码，**C/C++** 用于 `components/`
- 遵循 ESP-IDF 错误处理模式: 检查返回值 + `ESP_LOGE` + 优雅降级
- **Dual-Core** 线程安全: `std::atomic<T>` 用于跨核/跨任务共享变量
- 内存管理: `new(std::nothrow)` + null check (无 C++ 异常)
- 驱动架构: 每个外设独立 Driver 类, 通过 `PeripheralManager` facade 统一管理

## 开发框架

| 框架 | 用途 |
|------|------|
- **ESP-IDF v6.2** | 基础开发框架（ESP32-S31 preview target） |
| **esp32_s31_korvo_1 BSP** (v1.0.0~1) | [官方板级支持包](https://components.espressif.com/components/espressif/esp32_s31_korvo_1/versions/1.0.0~1), 提供 Display/LVGL/Touch/Buttons/Audio/SDCARD/LED/Camera 驱动 |
| **ESP-Skainet** | AI 语音助手 SDK (语音唤醒 + 命令词识别) |
| **ESP-Brookesia master (v0.8)** | 组件化 HMI/Service/GUI 基线；ESP32-S31 要求 ESP-IDF 6.2，当前先编译服务与 LVGL 公共层，硬件所有权暂由现有驱动保持 |
| **ESP-GMF** | 通用多媒体框架 (音视频管道) |
| **ESP-Matter** | Matter + Thread 智能家居协议 |
| **esp_board_manager** | 板载外设管理组件 |
| **gen_bmgr_codes** | 由 `idf.py bmgr -b esp32_s31_korvo_1` 自动生成的板级定义代码；运行前需安装 `esp-bmgr-assist`，目录已忽略，不应手工编辑 |
