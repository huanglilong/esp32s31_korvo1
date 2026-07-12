# ESP32-S31 Korvo-1 开发板硬件信息

> **信息来源**: https://esp32-s31.espressif.com/en  
> **文档版本**: V1.1  
> **生成日期**: 2026-07-13

---

## 目录

1. [ESP32-S31 Korvo-1 开发板概述](#1-esp32-s31-korvo-1-开发板概述)
2. [开发板板载组件详解](#2-开发板板载组件详解)
3. [设备连接关系与系统框图](#3-设备连接关系与系统框图)
4. [电源系统](#4-电源系统)
5. [可选配件](#5-可选配件)
6. [开发框架支持](#6-开发框架支持)
7. [ESP32-S31 芯片概述](#7-esp32-s31-芯片概述)
8. [ESP32-S31 芯片外设详解](#8-esp32-s31-芯片外设详解)
9. [ESP32-S31-WROOM-3 模组](#9-esp32-s31-wroom-3-模组)
10. [参考资料](#10-参考资料)

---

## 1. ESP32-S31 Korvo-1 开发板概述

**ESP32-S31-Korvo-1 V1.1** 是一款基于 **ESP32-S31-WROOM-3** 模组的多媒体开发板，专为智能音频和人机交互 (HMI) 应用设计。

### 核心特性

- 双麦克风阵列（左右模拟麦克风）
- 双声道扬声器输出（左右各 4Ω/3W）
- microSD 卡槽（支持 4-bit SDIO 3.0）
- 支持外接 4.3 英寸 LCD 子板
- 支持外接 DVP 摄像头（OV3660）
- 语音识别和语音唤醒支持
- Wi-Fi 6 / BLE 5.4 / 802.15.4 / 蓝牙经典

---

## 2. 开发板板载组件详解

### 2.1 组件列表（顺时针方向）

| 序号 | 组件名称 | 描述 |
|------|----------|------|
| 1 | USB Type-C 端口 (Power) | 纯供电端口，无数据通信功能 |
| 2 | USB Type-C 端口 (UART) | 可供电、烧录固件、通过 USB-to-UART 桥接器通信 |
| 3 | USB-to-UART 桥接器 | 单芯片 USB 转 UART，最高支持 3 Mbps |
| 4 | 电源开关 | 拨向 ON 接通 5V 电源，反方向断开 |
| 5 | USB 2.0 Type-A 端口 | 连接 ESP32-S31 USB 2.0 OTG High-Speed 接口，作为 USB Host，最大输出 500 mA |
| 6 | Buck 转换器 | 降压型 DC-DC 转换器，提供 3.3V 系统电源 |
| 7 | 5V 电源指示灯 | USB 供电时亮红色 |
| 8 | TPS2051C 开关 | USB 电源开关，500 mA 限流 |
| 9 | 右声道扬声器输出口 | 驱动 4Ω/3W 扬声器，引脚间距 2.00 mm |
| 10 | 右麦克风 | 板载右声道模拟麦克风，连接到音频编解码器 |
| 11 | 5V 转 3.3V LDO | 为音频电路提供独立 3.3V 电源 |
| 12 | 右声道音频功放 | NS4150B 低 EMI 3W D 类单声道功放 |
| 13 | 功能按键 | 四个按键：PLAY、SET、VOL-、VOL+，连接至 ESP32-S31 GPIO |
| 14 | 音频编解码器 (Audio Codec) | **ES8389** 低功耗立体声编解码器，双 ADC/DAC，通过 I2S + I2C 连接 |
| 15 | 左声道音频功放 | NS4150B 低 EMI 3W D 类单声道功放 |
| 16 | 左麦克风 | 板载左声道模拟麦克风，连接到音频编解码器 |
| 17 | 左声道扬声器输出口 | 驱动 4Ω/3W 扬声器，引脚间距 2.00 mm |
| 18 | RGB LED | 可寻址 RGB LED，由 **GPIO8** 驱动 |
| 19 | 3.3V 转 1.8V LDO (NC) | 为 1.8V SPI NAND Flash 供电，默认不焊接 |
| 20 | SPI NAND Flash (NC) | 四线 SPI NAND Flash，与 microSD 共享信号，默认不焊接 |
| 21 | LCD 连接器 | 用于连接 LCD 子板 |
| 22 | **ESP32-S31-WROOM-3** | 核心模组，集成 ESP32-S31 芯片 + 16MB SPI Flash + 16MB PSRAM，板载 PCB 天线 |
| 23 | microSD 卡槽 | 支持 4-bit microSD，用于音频存储和播放，SDIO 3.0 |
| 24 | 3.3V 转 2.8V LDO | 为外部摄像头模块提供 2.8V |
| 25 | 3.3V 转 1.5V LDO | 为外部摄像头模块提供 1.5V |
| 26 | 摄像头连接器 | 连接外部摄像头模块（DVP 接口） |
| 27 | Reset 按钮 | 系统复位 |
| 28 | Boot 按钮 | 固件下载模式：按住 Boot 再按 Reset 进入下载模式 |

### 2.2 音频子系统详解

| 组件 | 型号/规格 | 说明 |
|------|-----------|------|
| 音频编解码器 | **ES8389** | 低功耗立体声编解码器，双 ADC/DAC，低噪声前置放大器，耳机驱动，数字音效，模拟混音，增益控制 |
| 音频功放 (左右) | **NS4150B** ×2 | 低 EMI 3W D 类单声道功放 |
| 左麦克风 | 板载模拟麦克风 | 连接到 ES8389 |
| 右麦克风 | 板载模拟麦克风 | 连接到 ES8389 |
| 扬声器输出 | 4Ω / 3W 双声道 | 引脚间距 2.00 mm |
| 接口类型 | I2S + I2C | 音频数据走 I2S，控制走 I2C |
| 独立供电 | 5V→3.3V LDO | 音频电路使用独立电源轨，减少数字噪声干扰 |

### 2.3 按键功能

| 按键 | 功能 |
|------|------|
| PLAY | 播放/暂停控制 |
| SET | 设置/模式切换 |
| VOL- | 音量减小 |
| VOL+ | 音量增大 |

### 2.4 存储接口

| 接口 | 描述 |
|------|------|
| microSD 卡槽 | 4-bit 模式，SDIO 3.0，用于音频文件存储和播放 |
| SPI NAND Flash (NC) | 默认不焊接，与 microSD 共享 ESP32-S31 信号线 |
| 模组内置 Flash | 16 MB SPI Flash（ESP32-S31-WROOM-3 模组集成） |
| 模组内置 PSRAM | 16 MB PSRAM（ESP32-S31-WROOM-3 模组集成） |

### 2.5 扩展接口

| 接口 | 描述 |
|------|------|
| LCD 连接器 | 连接 4.3 英寸 LCD 子板（ESP32-S3-LCD-EV-Board-SUB3） |
| 摄像头连接器 | DVP 接口，支持 OV3660 摄像头模块 |
| USB 2.0 Type-A | USB Host 模式，最大 500 mA 输出 |
| USB Type-C (UART) | 供电 + 烧录 + 串口通信 |
| USB Type-C (Power) | 纯供电 |

---

## 3. 设备连接关系与系统框图

### 3.1 核心连接关系

```
┌────────────────────────────────────────────────────────────┐
│                   ESP32-S31-WROOM-3 模组                     │
│  ┌──────────────────────────────────────────────────────┐  │
│  │              ESP32-S31 芯片                           │  │
│  │  • 16 MB SPI Flash                                   │  │
│  │  • 16 MB PSRAM                                       │  │
│  │  • PCB 天线                                           │  │
│  └──────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────┘
         │           │           │            │
    I2S+I2C     SDIO 3.0     DVP           GPIO
         │           │           │            │
    ┌────▼────┐ ┌───▼────┐ ┌───▼─────┐ ┌───▼──────┐
    │ ES8389  │ │microSD │ │ 摄像头   │ │ 功能按键  │
    │ Codec   │ │ 卡槽   │ │ 连接器   │ │ + RGB LED│
    └───┬─────┘ └────────┘ └─────────┘ └──────────┘
        │
   ┌────┴────┐
   │         │
┌──▼──┐  ┌──▼──┐
│NS4150B│ │NS4150B│
│ 左功放│  │ 右功放│
└──┬───┘  └──┬───┘
   │         │
┌──▼──┐  ┌──▼──┐
│左扬声器│ │右扬声器│
└─────┘  └─────┘

模拟麦克风 (左/右) → ES8389 模拟输入
```

### 3.2 通信接口连接

| 外设 | 连接总线 | 说明 |
|------|----------|------|
| ES8389 音频编解码器 | I2S (数据) + I2C (控制) | 立体声音频采集和播放 |
| microSD 卡 | SDIO 3.0 (4-bit) | 音频文件存储 |
| DVP 摄像头 | DVP 并行接口 | 图像采集 |
| USB-to-UART 桥接器 | UART | 烧录和串口调试 |
| USB 2.0 OTG | USB 2.0 HS | USB Host，连接外部 USB 设备 |
| 功能按键 (PLAY/SET/VOL-/VOL+) | GPIO | 用户交互 |
| RGB LED | GPIO8 | 状态指示 |
| LCD 子板 (可选) | 专用 LCD 接口 | 显示输出 |

---

## 4. 电源系统

### 4.1 供电方案

| 电源输入 | 说明 |
|----------|------|
| USB Type-C (Power) | 纯供电端口，无数据通信 |
| USB Type-C (UART) | 供电 + 数据通信 |

> **注意**: 当同时驱动高功率扬声器并使用 USB Type-A 为外部设备供电时，确保板卡总输入电流满足 3A 要求。

### 4.2 电源轨

| 电压 | 来源 | 用途 |
|------|------|------|
| 5V | USB 输入 | 系统输入电压 |
| 3.3V (系统) | Buck DC-DC 转换器 | ESP32-S31 模组及大部分数字电路 |
| 3.3V (音频) | 5V→3.3V LDO (独立) | ES8389 及音频电路，减少数字噪声 |
| 2.8V | 3.3V→2.8V LDO | 外部摄像头模块 |
| 1.5V | 3.3V→1.5V LDO | 外部摄像头模块 |
| 1.8V (NC) | 3.3V→1.8V LDO | SPI NAND Flash (默认不焊接) |

### 4.3 保护电路

| 组件 | 功能 |
|------|------|
| TPS2051C | USB 电源开关，500 mA 限流 |
| 电源开关 | 硬件电源通断控制 |
| Buck 转换器 | 高效 DC-DC 降压 |

---

## 5. 可选配件

| 配件 | 型号 | 说明 |
|------|------|------|
| LCD 子板 | ESP32-S3-LCD-EV-Board-SUB3 | 4.3 英寸 LCD 显示屏 |
| 摄像头模块 | OV3660 | DVP 接口摄像头 |

---

## 6. 开发框架支持

该开发板支持以下 Espressif 开发框架：

| 框架 | 说明 |
|------|------|
| **ESP-IDF** | Espressif 官方 IoT 开发框架 |
| **ESP-BLE-MESH / ESP-BLE-AUDIO** | 蓝牙 LE 生态解决方案 |
| **ESP-Brookesia** | AIoT 设备人机交互框架（图形 UI） |
| **ESP-GMF** | 通用多媒体框架（音视频处理） |
| **ESP Video Components** | 摄像头、视频流和视频处理 |
| **ESP-Matter** | Matter 和 Thread 设备开发 |
| **Bluetooth Audio APIs** | 统一蓝牙音频 API（经典 + LE Audio） |
| **esp_board_manager** | 板载外设管理组件（LCD、Codec、按键、LED 等） |

---

## 7. ESP32-S31 芯片概述

### 7.1 基本信息

| 属性 | 规格 |
|------|------|
| 芯片系列 | ESP32-S31 |
| 制造工艺 | 40 nm |
| 核心模组 | ESP32-S31-WROOM-3 |
| 开发板 | ESP32-S31-Korvo-1 (V1.1) / ESP32-S31-Function-CoreBoard-1 |

### 7.2 无线连接能力

| 协议 | 版本/标准 | 说明 |
|------|-----------|------|
| **Wi-Fi** | 2.4 GHz Wi-Fi 6 (802.11ax) | 支持 ESP-NOW, WIFI-MESH, SmartConfig, DPP, NAN |
| **蓝牙 LE** | Bluetooth 5.4 (LE) | 支持 BLE MESH, BLE AUDIO |
| **蓝牙经典** | Bluetooth (BR/EDR) | 经典蓝牙音频 |
| **IEEE 802.15.4** | Zigbee 3.0 / Thread 1.4 | 支持 Matter 协议 |

### 7.3 主要特点

- 40 nm 工艺，出色的功耗效率
- 优秀的 RF 性能
- 硬件安全特性（Flash 加密、安全启动、eFuse、HMAC、ECDSA、RSA）
- 支持多种功耗模式（睡眠模式、ULP 协处理器）
- 丰富的 GPIO 和外设接口

---

## 8. ESP32-S31 芯片外设详解

### 8.1 模拟外设

| 外设 | 说明 |
|------|------|
| **ADC** (模数转换器) | 支持多通道模拟信号采集 |
| **模拟比较器** (Analog Comparator) | 模拟电压比较 |
| **温度传感器** (Temperature Sensor) | 片内温度检测 |
| **电容触摸传感器** (Capacitive Touch) | 电容式触摸按键 |
| **LDO 稳压器** | 片内低压差稳压器 |

### 8.2 数字接口

| 外设 | 说明 |
|------|------|
| **GPIO & RTC GPIO** | 通用输入输出，支持 RTC 域 GPIO |
| **Dedicated GPIO** | 专用 GPIO（快速并行 IO） |
| **UART** | 通用异步收发器 |
| **I2C** | 内部集成电路总线 |
| **I2S** | 内部 IC 音频总线 |
| **SPI Master** | SPI 主控制器 |
| **SPI Slave** | SPI 从控制器 |
| **SPI Slave HD** | SPI 半双工从机 |
| **SDMMC Host** | SD/MMC 主机控制器 |
| **SD SPI Host** | SD SPI 模式主机 |
| **RMT** | 红外遥控收发器 |
| **TWAI** (Two-Wire Automotive Interface) | 双线汽车接口 (CAN) |
| **Parallel IO** | 并行 IO 接口 |
| **USB Device** | USB 设备模式 |
| **USB Host** | USB 主机模式 |

### 8.3 定时器与 PWM

| 外设 | 说明 |
|------|------|
| **GPTimer** (通用定时器) | 通用硬件定时器 |
| **LEDC** (LED PWM 控制器) | LED 亮度控制 / PWM 输出 |
| **MCPWM** (电机控制 PWM) | 电机控制脉宽调制 |
| **PCNT** (脉冲计数器) | 脉冲计数 |
| **SDM** (Sigma-Delta 调制) | Sigma-Delta 信号调制 |
| **ESP Timer** (高精度定时器) | 高分辨率软件定时器 |

### 8.4 多媒体与加速器

| 外设 | 说明 |
|------|------|
| **LCD** | LCD 显示控制器 |
| **JPEG Encoder/Decoder** | JPEG 图像编解码 |
| **PPA** (Pixel-Processing Accelerator) | 像素处理加速器 |
| **Asynchronous Color Conversion** | 异步色彩空间转换 |
| **Asynchronous Memory Copy** | 异步内存拷贝 (DMA) |
| **CORDIC** | 坐标旋转数字计算机（三角函数加速） |

### 8.5 安全外设

| 外设 | 说明 |
|------|------|
| **eFuse Manager** | 一次性可编程存储器管理 |
| **HMAC** | 哈希消息认证码硬件加速 |
| **ECDSA_DS** | ECDSA 数字签名硬件加速 |
| **RSA_DS** | RSA 数字签名硬件加速 |
| **Key Manager** | 密钥管理器 |
| **Flash Encryption** | Flash 加密 |
| **Secure Boot** | 安全启动 |
| **BitScrambler** | 比特加扰器 |

### 8.6 系统外设

| 外设 | 说明 |
|------|------|
| **Clock Tree** | 时钟树管理 |
| **ETM** (Event Task Matrix) | 事件任务矩阵 |
| **SPI Flash API** | SPI Flash 操作接口 |
| **Watchdogs** | 看门狗定时器 |
| **ULP Coprocessor** | 超低功耗协处理器 |
| **Power Management** | 电源管理（多种睡眠模式） |
| **Random Number Generator** | 硬件随机数生成器 |
| **Interrupt Allocation** | 中断分配管理 |
| **IPC** (Inter-Processor Call) | 核间通信 |

---

## 9. ESP32-S31-WROOM-3 模组

### 9.1 模组规格

| 属性 | 规格 |
|------|------|
| 主芯片 | ESP32-S31 |
| Flash | 16 MB SPI Flash |
| PSRAM | 16 MB |
| 天线 | PCB 板载天线 |
| Wi-Fi | 2.4 GHz Wi-Fi 6 (802.11ax) |
| 蓝牙 | Bluetooth 5.4 (LE) + Bluetooth Classic (BR/EDR) |
| 802.15.4 | Zigbee 3.0 / Thread 1.4 |

### 9.2 开发板对比

| 特性 | Korvo-1 | Function-CoreBoard-1 |
|------|---------|----------------------|
| 应用场景 | 智能音频 + HMI | 通用 AIoT 原型开发 |
| 音频 | 立体声 (ES8389) + 双麦克风 | 单声道 (ES8311) + 单麦克风 |
| 音频功放 | NS4150B ×2 (3W×2) | NS4150B ×1 (3W) |
| 以太网 | 无 | 千兆以太网 (RGMII) |
| LCD | 支持外接 4.3" LCD 子板 | 无 |
| 摄像头 | DVP 接口 (OV3660) | 无 |
| microSD | 4-bit SDIO 3.0 | 无 |
| USB | USB Type-A Host + 双 Type-C | USB Type-A Host + USB Serial/JTAG + USB-to-UART |
| GPIO 引出 | 部分（板载外设占用较多） | 全部关键 GPIO (J2 排针) |
| 按键 | PLAY/SET/VOL-/VOL+ | Boot + Reset |
| RGB LED | GPIO8 | GPIO60 |

---

## 10. 参考资料

| 资源 | 链接 |
|------|------|
| ESP32-S31 官方站点 | https://esp32-s31.espressif.com/en |
| Korvo-1 用户指南 | https://documentation.espressif.com/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/index.html |
| Function-CoreBoard-1 用户指南 | https://documentation.espressif.com/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/index.html |
| ESP-IDF 编程指南 (ESP32-S31) | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/index.html |
| 技术参考手册 (TRM PDF) | https://www.espressif.com/sites/default/files/documentation/esp32-s31_technical_reference_manual_en.pdf |
| 硬件设计指南 | https://documentation.espressif.com/esp-hardware-design-guidelines/en/latest/esp32s31/index.html |
| ESP 产品选择器 | https://products.espressif.com/ |
| 芯片系列对比 | https://products.espressif.com/#/product-comparison |
| Espressif KiCad 库 | https://github.com/espressif/kicad-libraries |
| 认证证书 | https://www.espressif.com/en/certificates?keys=ESP32-S31 |
| 硬件论坛 | https://esp32.com/viewforum.php?f=12 |
| esp_board_manager 组件 | ESP Component Registry |
| Pinout & IO 分配 | https://esp32-s31.espressif.com/en/docs/6-Pinout%20%26%20IO%20Introduction/pinout-and-io-allocation |
