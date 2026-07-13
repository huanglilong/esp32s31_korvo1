### Hardware Info

- [ESP32-S31-Korvo-1](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32s31/esp32-s31-korvo-1/index.html)
  - **ESP32-S31-WROOM-3** 模组 (ESP32-S31 + 16MB SPI Flash + 16MB PSRAM, PCB 天线)
  - 芯片版本: ESP32-S31NRV16 / ESP32-S31NRV32, CPU 主频 320 MHz
  - 详细硬件信息: [doc/esp32s31_korvo1_hardware_info.md](doc/esp32s31_korvo1_hardware_info.md)

### External Peripherals (Korvo-1)

- **Audio**: ES8389 立体声编解码器 (I2S + I2C) + NS4150B ×2 (3W D 类功放), 双模拟麦克风 + 双扬声器输出 (4Ω / 3W)
  |   Signal  |  ESP32-S31 GPIO |   Direction   |   ES8389  |
  |:----:|:----:|:----:|:----:|
  |   I2C_SDA	|   TBD	|   ↔	|   I2C data  |
  |   I2C_SCL	|   TBD	|   ↔	|   I2C clk   |
  |   I2S_MCLK	|   TBD	|   →	|   MCLK      |
  |   I2S_SCLK	|   TBD	|   ↔	|   BCLK      |
  |   I2S_LRCK	|   TBD	|   ↔	|   LRCK/WS   |
  |   I2S_SDOUT	|   TBD	|   ←	|   ADC data  |
  |   I2S_SDIN	|   TBD	|   →	|   DAC data  |
  |   PA_L	    |   NS4150B |   ← ES8389  |   左声道功放  |
  |   PA_R	    |   NS4150B |   ← ES8389  |   右声道功放  |

- **microSD Card**: SDIO 3.0 (4-bit), 音频存储和播放
  |   Signal	  |   Interface   |   Description |
  |:----:|:----:|:----:|
  |   SD_CLK	  |   SDIO CLK    |   Clock       |
  |   SD_CMD	  |   SDIO CMD    |   Command     |
  |   SD_D0	    |   SDIO DATA0  |   Data 0      |
  |   SD_D1	    |   SDIO DATA1  |   Data 1      |
  |   SD_D2	    |   SDIO DATA2  |   Data 2      |
  |   SD_D3	    |   SDIO DATA3  |   Data 3      |

- **LCD Subboard** (可选): ESP32-S3-LCD-EV-Board-SUB3 (4.3 英寸), 通过专用 LCD 连接器连接

- **DVP Camera** (可选): OV3660, 通过 DVP 摄像头连接器连接
  |   Signal    |   Interface   |   Description     |
  |:----:|:----:|:----:|
  |   DVP_D[7:0]	|   DVP Data    |   8-bit 并行数据  |
  |   DVP_PCLK	  |   DVP PCLK    |   像素时钟        |
  |   DVP_HSYNC	|   DVP HSYNC   |   行同步          |
  |   DVP_VSYNC	|   DVP VSYNC   |   帧同步          |
  |   DVP_XCLK	  |   DVP XCLK    |   主时钟输出      |
  |   CAM_2.8V	  |   3.3→2.8V LDO |   摄像头 2.8V 供电 |
  |   CAM_1.5V	  |   3.3→1.5V LDO |   摄像头 1.5V 供电 |

- **Buttons**:
  |   Button  |   GPIO   |   Function   |
  |:----:|:----:|:----:|
  |   PLAY   |   TBD   |   播放/暂停   |
  |   SET    |   TBD   |   设置/模式   |
  |   VOL-   |   TBD   |   音量减小    |
  |   VOL+   |   TBD   |   音量增大    |

- **RGB LED**: 可寻址 RGB LED (WS2812 兼容), 由 **GPIO8** 驱动

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
- **Audio Driver**: ES8389 立体声编解码器驱动, I2S 48kHz 16-bit 双工, 硬件音量控制, uORB 音量状态发布
- **SD Card**: SDIO 3.0 4-bit 模式, FATFS 文件系统, boot 时自动挂载
- **Camera**: DVP OV3660 摄像头驱动 (optional, mutex 互斥控制)
- **WiFi**: 内置 Wi-Fi 6 STA + SoftAP 模式, NVS 凭证持久化, SNTP 时间同步
- **mDNS**: esp-web-XXXXXX.local 主机名, Web Config 服务广告
- **Web Config Server**: HTTP port 8080, WiFi 扫描/连接 REST API, 音量控制, 系统信息
- **Logger**: 文本日志 (ESP_LOG → ring buffer → SD card /logs/app_NNNNNN.log, 含 git info 文件头)
- **uORB**: PX4 风格 pub/sub 消息总线 (FreeRTOS queue), 10 个 topics
- **ULog**: PX4 ULog 格式二进制日志 (SD 卡 .ulg 文件, 含 git branch/commit/author/date/msg, SNTP 同步后自动启动)
- **System Monitor**: CPU 使用率 (per-core idle deltas), 内存抽样, uORB system_stats/alert
- **Thread Safety**: std::atomic<T> 跨核保护, FreeRTOS Mutex 互斥, CAS lazy-init
- **Build**: ESP-IDF v6.x, CMake, uORB 代码生成 (proto/*.msg → generated/*)
