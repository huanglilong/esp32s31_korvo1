### Hardware Info

- [ESP32-S31-Korvo-1](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/index.html)
  - **ESP32-S31-WROOM-3** 模组 (ESP32-S31 + 16MB SPI Flash + 16MB PSRAM, PCB 天线)
  - 芯片版本: ESP32-S31NRV16 / ESP32-S31NRV32, CPU 主频 320 MHz
  - 详细硬件信息: [doc/esp32s31_korvo1_hardware_info.md](doc/esp32s31_korvo1_hardware_info.md)

### Official BSP Component

- [espressif/esp32_s31_korvo_1](https://components.espressif.com/components/espressif/esp32_s31_korvo_1/versions/1.0.0~1) (Board Support Package) — v1.0.0~1
  - 源码: [esp-bsp/bsp/esp32_s31_korvo_1](https://github.com/espressif/esp-bsp/tree/master/bsp/esp32_s31_korvo_1)
  - **Capabilities**: Display (LVGL) | Touch (GT1151) | Buttons (ADC array) | Audio (ES8389) | SDCARD | LED (WS2812) | Camera (OV3660)
  - **Key Dependencies**: `espressif/esp_codec_dev ~1.5`, `espressif/esp_lvgl_port ^2`, `espressif/esp_lcd_touch_gt1151`, `espressif/button ^4`, `espressif/led_indicator ^2`, `espressif/esp_video ~2.2`
  - **Official Examples**: Display, Display+Audio+Photo, Camera Video, LVGL Benchmark/Demos, SD Card, USB HID (7 examples)
  - > ⚠️ **WARNING**: MCLK 引脚在 ESP32-S31 上未连接。推荐音频采样率为 **16kHz**！

### External Peripherals (Korvo-1)

- **Audio**: ES8389 立体声编解码器 (I2S + I2C) + NS4150B ×2 (3W D 类功放), 双模拟麦克风 + 双扬声器输出 (4Ω / 3W)
  > ⚠️ **MCLK 未连接**: GPIO42 (I2S_MCLK) 在 ESP32-S31 上不可用。推荐采样率 **16kHz**，避免使用依赖 MCLK 的采样率配置。详见 [官

方 BSP 说明](https://components.espressif.com/components/espressif/esp32_s31_korvo_1/versions/1.0.0~1/readme)。
  |   Signal  |  ESP32-S31 GPIO |   Direction   |   ES8389  |
  |:----:|:----:|:----:|:----:|
  |   I2C_SDA	|   GPIO0	|   ↔	|   I2C data (AD0=L, 7-bit addr 0x10)  |
  |   I2C_SCL	|   GPIO1	|   ↔	|   I2C clk   |
  |   I2S_MCLK	|   GPIO42	|   →	|   MCLK (⚠️ 未连接)  |
  |   I2S_SCLK	|   GPIO3	|   ↔	|   BCLK      |
  |   I2S_LRCK	|   GPIO4	|   ↔	|   LRCK/WS   |
  |   I2S_SDOUT	|   GPIO6	|   ←	|   ADC data  |
  |   I2S_DSDIN	|   GPIO5	|   →	|   DAC data  |
  |   PA_CTRL	|   GPIO7    |   → NS4150B |   功放使能 (高电平有效)  |
  |   PA_L	    |   NS4150B |   ← ES8389  |   左声道功放  |
  |   PA_R	    |   NS4150B |   ← ES8389  |   右声道功放  |

- **microSD Card**: SDIO 3.0 (4-bit), 音频存储和播放
  |   Signal	  |   Interface   |   Description |
  |:----:|:----:|:----:|
  |   SD_CLK	  |   GPIO24    |   Clock       |
  |   SD_CMD	  |   GPIO25    |   Command     |
  |   SD_D0	    |   GPIO20  |   Data 0      |
  |   SD_D1	    |   GPIO21  |   Data 1      |
  |   SD_D2	    |   GPIO22  |   Data 2      |
  |   SD_D3	    |   GPIO23  |   Data 3      |

- **LCD Subboard** (可选): ESP32-S3-LCD-EV-Board-SUB3 (4.3 英寸), 通过专用 LCD 连接器连接

- **DVP Camera** (可选): OV3660, 通过 DVP 摄像头连接器连接
  |   Signal    |   Interface   |   Description     |
  |:----:|:----:|:----:|
  |   DVP_D[7:0]	|   GPIO[53:46]  |   8-bit 并行数据 (Y9:Y2)  |
  |   DVP_PCLK	  |   GPIO54    |   像素时钟        |
  |   DVP_HSYNC	|   GPIO57   |   行同步          |
  |   DVP_VSYNC	|   GPIO56   |   帧同步          |
  |   DVP_XCLK	  |   GPIO55    |   主时钟输出      |
  |   CAM_I2C_SDA	|   GPIO0 (共享) |   SCCB 数据 (与 ES8389 共享 I2C) |
  |   CAM_I2C_SCL	|   GPIO1 (共享) |   SCCB 时钟 (与 ES8389 共享 I2C) |
  |   CAM_2.8V	  |   3.3→2.8V LDO |   摄像头 2.8V 供电 |
  |   CAM_1.5V	  |   3.3→1.5V LDO |   摄像头 1.5V 供电 |

- **Buttons**:
  |   Button  |   ADC Voltage   |   Function   |
  |:----:|:----:|:----:|
  |   PLAY   |   ~2.8V (13KΩ)   |   播放/暂停   |
  |   SET    |   ~2.4V (6.8KΩ)   |   设置/模式   |
  |   VOL-   |   ~1.8V (3.3KΩ)   |   音量减小    |
  |   VOL+   |   ~1.0V (1.3KΩ)   |   音量增大    |

- **RGB LED**: 可寻址 RGB LED (WS2812B 兼容), 由 **GPIO37** 驱动 (经 LBSS138LT1G 电平转换)

- **USB**:
  |   Port   |   Interface   |   Function   |
  |:----:|:----:|:----:|
  |   USB Type-C (UART)   |   USB-to-UART Bridge (3 Mbps)   |   供电 + 烧录 + 串口通信  |
  |   USB Type-C (Power)  |   Power only   |   纯供电                  |
  |   USB 2.0 Type-A      |   USB 2.0 OTG HS   |   USB Host, 最大 500 mA  |

- **Power**:
  - 双 USB Type-C 供电; 同时驱动高功率扬声器 + USB Host 时需满足总输入 3A
  - 音频电路使用独立 5V→3.3V LDO 供电轨, 减少数字噪声
  - TPS2051C USB 电源开关 (500 mA 限流)

### ESP32-S31

- [ESP32-S31 数据手册](doc/esp32-s31_datasheet_en.pdf) (Pre-release v0.2)
  - **Dual-core** 32-bit RISC-V (HP + LP), 最高 **320 MHz**
  - SV32 MMU, 每核一个单精度 FPU, 128-bit SIMD (单核支持)
  - **QFN80** (8×8 mm), –40 ∼ 85 °C
  - 512 KB HP SRAM, 320 KB ROM, 32 KB LP SRAM
  - 外部 PSRAM 接口: 250 MHz 8-bit DDR (仅封装内)
  - 2.4 GHz **Wi-Fi 6** (802.11ax, 20 MHz-only), **Bluetooth 5.4 (LE)**, Bluetooth Classic, **802.15.4** (Zigbee 3.0, Thread 1.4)
  - TX 功率: 802.11b +20.5 dBm, 802.11ax +19.5 dBm; BLE RX 灵敏度 –105 dBm (125 Kbps)
  - 60 个可编程 GPIO, 4 个 Strapping 引脚
  - 外设: 4×UART + LP_UART, 6×SPI + LP_SPI, 2×I2C + LP_I2C, 2×I2S, USB 2.0 HS OTG, USB Serial/JTAG, 1000 Mbps EMAC, CAN FD, SDIO Host 3.0 (2 slots), 2×LEDC (8ch), 4×MCPWM, RMT, PARLIO, 2×PCNT
  - 多媒体: JPEG Codec, PPA, ASRC, CORDIC, LCD + Camera Controller
  - 模拟: Touch Sensor, Temperature Sensor, 2×12-bit SAR ADC (16ch), 2×10-bit + 2×12-bit DAC, Analog Comparator
  - 硬件安全: AES, ECC, HMAC, RSA, SHA, DSA, TRNG, Key Manager, Secure Boot, XTS_AES Flash 加密, DPA 防护, TEE
  - 功耗模式: Active, Modem-sleep, Light-sleep, Deep-sleep
- [ESP32-S31 技术参考手册](https://www.espressif.com/sites/default/files/documentation/esp32-s31_technical_reference_manual_en.pdf)
- [ESP32-S31 硬件设计指南](doc/esp-hardware-design-guidelines-en-master-esp32s31.pdf)

### ESP-IDF

- Version: **v6.x**
- Build, Flash and Monitor:
  - Linux:
    ```bash
    $ source ~/.espressif/v6.x/esp-idf/export.sh
    $ idf.py set-target esp32s31
    $ idf.py build && idf.py flash -b 1500000 -p $(bash -c "ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1") monitor
    ```
  - macOS:
    ```bash
    $ source ~/.espressif/v6.x/esp-idf/export.sh
    $ idf.py set-target esp32s31
    $ idf.py build && idf.py flash -b 1500000 -p $(bash -c "ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -1") monitor
    ```

### Software Features

- **Audio Pipeline**: 双模拟麦克风 → ES8389 ADC → I2S → ESP32-S31 处理 → I2S → ES8389 DAC → NS4150B PA → 扬声器
  - > ⚠️ MCLK 不可用，推荐采样率 **16kHz**
- **Audio Driver**: ES8389 立体声编解码器驱动, 使用 `esp_codec_dev` API, I2S 16kHz 16-bit 双工 (MCLK-less), 硬件音量控制 + PGA mic gain (40dB), AAC/ADTS 录音, uORB 音量状态发布
- **SD Card**: SDIO 3.0 4-bit 模式, FATFS 文件系统, boot 时自动挂载
- **Camera**: DVP OV3660 摄像头驱动 (optional, mutex 互斥控制), Camera App LCD 实时预览 (V4L2 + LVGL canvas)
- **WiFi**: 内置 Wi-Fi 6 STA + SoftAP 模式, NVS 凭证持久化, SNTP 时间同步
- **mDNS**: esp-web-XXXXXX.local 主机名, Web Config 服务广告
- **Web Config Server**: HTTP port 8080, WiFi 扫描/连接 REST API, 音量控制, 系统信息
- **Logger**: 文本日志 (ESP_LOG → ring buffer → SD card /logs/app_NNNNNN.log, 含 git info 文件头)
- **uORB**: PX4 风格 pub/sub 消息总线 (FreeRTOS queue), 10 个 topics
- **ULog**: PX4 ULog 格式二进制日志 (SD 卡 .ulg 文件, 含 git branch/commit/author/date/msg, SNTP 同步后自动启动)
- **System Monitor**: CPU 使用率 (per-core idle deltas), 内存抽样, uORB system_stats/alert
- **Thread Safety**: std::atomic<T> 跨核保护, FreeRTOS Mutex 互斥, CAS lazy-init
- **Build**: ESP-IDF v6.x, CMake, uORB 代码生成 (proto/*.msg → generated/*)
