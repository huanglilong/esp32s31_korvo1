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
├── CMakeLists.txt              # 顶层项目配置
├── sdkconfig.defaults          # 默认 Kconfig 配置
├── partitions.csv              # 分区表
├── main/
│   ├── CMakeLists.txt          # 主组件编译配置 (C++)
│   ├── idf_component.yml       # 组件依赖声明
│   ├── Kconfig.projbuild       # 项目 Kconfig 菜单
│   ├── example_config.h        # 引脚/参数/NVS 键/常量 宏定义
│   ├── main.cpp                # 主程序 (C++)
│   ├── drivers/                # Driver 模块
│   │   ├── audio/
│   │   │   ├── audio_driver.hpp    # AudioDriver — ES8389 I2S+I2C + volume
│   │   │   └── audio_driver.cpp
│   │   ├── sdcard/
│   │   │   ├── sdcard_driver.hpp   # SDCardDriver — SDIO 3.0 卡管理
│   │   │   └── sdcard_driver.cpp
│   │   ├── camera/
│   │   │   ├── camera_driver.hpp   # CameraDriver — OV3660 DVP 摄像头
│   │   │   └── camera_driver.cpp
│   │   └── buttons/
│   │       ├── buttons_driver.hpp  # ButtonsDriver — PLAY/SET/VOL-/VOL+
│   │       └── buttons_driver.cpp
│   ├── audio_recorder.hpp     # 音频录音模块 (麦克风 → ES8389 → I2S → MP3/WAV)
│   ├── audio_recorder.cpp
│   ├── audio_player.hpp       # 音频播放模块 (SD 卡 → MP3/WAV → I2S → ES8389 → 扬声器)
│   ├── audio_player.cpp
│   ├── voice_wakeup.hpp       # 语音唤醒模块 (ESP-Skainet)
│   ├── voice_wakeup.cpp
│   ├── speech_recognition.hpp # 语音识别模块
│   ├── speech_recognition.cpp
│   ├── wifi_service.hpp       # WifiService — Wi-Fi 管理 + SNTP
│   ├── wifi_service.cpp
│   └── rgb_led.hpp            # RGB LED 控制 (GPIO8, WS2812)
│   └── rgb_led.cpp
├── components/                 # 本地/BSP 组件 (谨慎编辑)
├── managed_components/         # ⚠️ ESP-IDF 管理 (禁止编辑)
├── doc/                        # 项目文档
│   ├── esp32s31_korvo1_hardware_info.md
│   └── esp32-s31_datasheet_en.pdf
├── README.md                   # 硬件信息 + 构建说明
├── PROJECT_REQUIREMENTS.md     # 需求与变更记录
└── CLAUDE.md                   # Copilot 指令引用
```

## 硬件接线参考

### Audio (ES8389)

| Signal | Direction | Description |
|:---|:---|:---|
| I2S_MCLK | ESP32-S31 → ES8389 | 主时钟 |
| I2S_SCLK | ↔ | 位时钟 (BCLK) |
| I2S_LRCK | ↔ | 左右声道时钟 (WS) |
| I2S_SDOUT | ← ES8389 | ADC 数据 (麦克风) |
| I2S_SDIN | ESP32-S31 → | DAC 数据 (扬声器) |
| I2C_SDA | ↔ | 配置与控制 |
| I2C_SCL | ↔ | I2C 时钟 |
| MIC_L/R | → ES8389 analog in | 模拟麦克风输入 |
| PA_L/R | ← ES8389 → NS4150B | 功放输入 → 扬声器 |

### Buttons

| Button | GPIO | Function |
|:---|:---|:---|
| PLAY | TBD | 播放/暂停 |
| SET | TBD | 设置/模式 |
| VOL- | TBD | 音量减小 |
| VOL+ | TBD | 音量增大 |

### RGB LED

| LED | GPIO | Description |
|:---|:---|:---|
| WS2812 RGB | GPIO8 | 可寻址 RGB LED |

### microSD Card

| Signal | Interface | Description |
|:---|:---|:---|
| SD_CLK | SDIO CLK | 时钟 |
| SD_CMD | SDIO CMD | 命令 |
| SD_D0 | SDIO DATA0 | 数据线 0 |
| SD_D1 | SDIO DATA1 | 数据线 1 |
| SD_D2 | SDIO DATA2 | 数据线 2 |
| SD_D3 | SDIO DATA3 | 数据线 3 |

## 代码规范

- **C++** 用于 `main/` 应用层代码，**C/C++** 用于 `components/`
- 遵循 ESP-IDF 错误处理模式: 检查返回值 + `ESP_LOGE` + 优雅降级
- **Dual-Core** 线程安全: `std::atomic<T>` 用于跨核/跨任务共享变量
- 内存管理: `new(std::nothrow)` + null check (无 C++ 异常)
- 驱动架构: 每个外设独立 Driver 类, 通过 `PeripheralManager` facade 统一管理

## 开发框架

| 框架 | 用途 |
|------|------|
| **ESP-IDF v6.x** | 基础开发框架 |
| **ESP-Skainet** | AI 语音助手 SDK (语音唤醒 + 命令词识别) |
| **ESP-Brookesia** | HMI 人机交互框架 (图形 UI, LVGL) |
| **ESP-GMF** | 通用多媒体框架 (音视频管道) |
| **ESP-Matter** | Matter + Thread 智能家居协议 |
| **esp_board_manager** | 板载外设管理组件 |
