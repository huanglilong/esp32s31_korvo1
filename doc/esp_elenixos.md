# ElenixOS 项目架构分析

> 分析日期: 2026-07-15
> 仓库地址: https://github.com/HoGC/esp_elenixos (ESP32-S31 Korvo1 移植)
> 上游仓库: https://github.com/ElenixOS/ElenixOS (核心框架)
> Stars: 22 | Language: C | License: Apache 2.0

---

## 一、整体架构

ElenixOS 是一个专为嵌入式设备设计的开源智能手表操作系统，核心架构采用**四层分层设计**：

```
┌──────────────────────────────────────┐
│  Applications & Watchfaces (JS)      │  ← JavaScript 应用层
├──────────────────────────────────────┤
│  SPM (Script Program Manager)        │  ← 程序生命周期管理
│  SNI (Script Native Interface)       │  ← JS ↔ Native 桥接
├──────────────────────────────────────┤
│  Framework (Activity/Chrome/App/    │  ← 系统框架层
│  Watchface/Package/i18n)             │
├──────────────────────────────────────┤
│  Kernel (Core/Event/Scheduler/Mem)  │  ← 内核层
│  Devices | Services | UI | Port      │
└──────────────────────────────────────┘
     ESP-IDF / 裸机硬件平台
```

### 1.1 目录结构 (上游仓库)

```
ElenixOS/
├── src/
│   ├── elenix_os.h              # 系统入口头文件
│   ├── eos_version.h            # 版本号定义
│   ├── apps/                    # 内置应用 (flash_light, settings, test)
│   ├── config/                  # 系统配置系统(支持 Kconfig)
│   ├── devices/                 # 设备抽象层 (battery, display, mic, power, sensor, speaker, time, vibrator)
│   ├── framework/               # 系统框架层
│   │   ├── activity/            # Activity 导航栈(页面生命周期管理)
│   │   ├── app/                 # 应用系统(安装/卸载/启动)
│   │   ├── chrome/              # Chrome Manager(系统叠加层 + 表冠交互)
│   │   ├── i18n/                # 多语言国际化
│   │   ├── package/             # EAPK/EWPK 包管理器
│   │   └── watchface/           # 表盘系统(内置 + JS 脚本化)
│   ├── input/                   # 输入层 (crown, side_button, touch)
│   ├── kernel/                  # 内核层
│   │   ├── core/                # 系统核心(eos_init, eos_main_loop)
│   │   ├── event/               # 事件系统
│   │   ├── memory/              # 内存管理
│   │   └── scheduler/           # 调度器
│   ├── lib/                     # 工具库(ds, sha256, utils)
│   ├── port/                    # 平台移植层(critical, file_system, memory)
│   ├── script_engine/           # 脚本引擎
│   │   ├── core/                # Script Engine Core (SEC) — JerryScript VM 生命周期
│   │   ├── spm/                 # Script Program Manager — JS 程序生命周期 + 回调门控
│   │   ├── sni/                 # Script Native Interface — JS ↔ Native 桥接
│   │   │   ├── sni_api/lv/      # LVGL API 导出到 JS
│   │   │   ├── sni_api/eos/     # ElenixOS 系统 API 导出到 JS
│   │   │   ├── sni_gen/         # 自动生成的类型绑定
│   │   │   └── sni_context.*    # 每个 Realm 独立的资源生命周期管理
│   │   └── jerry_port.c         # JerryScript 平台移植
│   ├── services/                # 后台服务(audio, battery, cache, config, display, haptic,
│   │                           #  lock, log, permission, pm, sensor, state, storage, time)
│   └── ui/                      # UI 组件(font, pages, symbol, theme, watchface_widgets, widgets)
├── resources/                   # 资源文件
├── scripts/                     # 构建脚本
└── third_party/                 # 第三方依赖
```

### 1.2 ESP32-S31 移植仓库结构

```
esp_elenixos/
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   └── main.c                  # 初始化外设、LCD、触摸、LVGL 适配器, 启动 ElenixOS
├── components/
│   ├── elenixos/                # ElenixOS 核心 + ESP 移植层
│   │   ├── ElenixOS/            # 上游核心代码(submodule)
│   │   ├── port/                # ESP32 平台移植
│   │   └── overlay/             # 覆盖层配置
│   ├── esp_lvgl_adapter/        # LVGL 的 ESP 适配器
│   └── lvgl/                    # LVGL 图形库
├── fs/
│   ├── .sys/                    # 系统配置
│   └── my_app.eapk              # 示例应用包
├── partitions.csv               # 分区表
├── sdkconfig.defaults           # 默认 Kconfig 配置
└── sdkconfig.defaults.esp32s31  # ESP32-S31 专用 Kconfig 配置
```

---

## 二、关键组件详解

### 2.1 JerryScript — 轻量级 JS 引擎

ElenixOS 选择 **JerryScript** 而非 QuickJS 或 Duktape：

- **极致轻量**: JerryScript 是专为 IoT/嵌入式设计的 ECMAScript 5.1 引擎
- **内存友好**: 相比 QuickJS 更省 RAM, 适合 ESP32 级别芯片
- **成熟稳定**: Samsung 主导开发, 已广泛应用于 IoT 设备

### 2.2 SNI (Script Native Interface) — JS ↔ Native 桥接层

SNI 是 ElenixOS 最核心的创新之一，它提供的不仅仅是简单的函数绑定：

```
┌──────────────────────────────────────────────┐
│                 JavaScript                    │
│  lv.obj.create()  lv.label.create()           │
│  lv.anim.create()  eos.storage.read()         │
├──────────────────────────────────────────────┤
│              SNI API Layer                    │
│  sni_api_lv.c    sni_api_eos.c                │
│  sni_api_export.c (类/方法/属性/常量注册)      │
├──────────────────────────────────────────────┤
│          SNI Context (per-Realm)              │
│  - 自动追踪 timer/anim/style/font 等资源       │
│  - Realm 销毁时自动清理所有 JS 引用            │
│  - 支持暂停/恢复回调调度                       │
│  - 引擎恢复后检测 stale handle (gen counter)  │
├──────────────────────────────────────────────┤
│         JerryScript / LVGL / Native           │
└──────────────────────────────────────────────┘
```

**关键设计**:
- `sni_context_t` 为每个 JS Realm 独立管理托管资源, 使用类型索引链表
- `sni_context_sweep_js_refs()` 在引擎停止前释放所有 JS 回调引用, 防止 bytecode 泄漏
- 支持引擎 fatal recovery(setjmp/longjmp), 带有 generation counter 检测过时句柄

### 2.3 SPM (Script Program Manager) — JS 程序生命周期管理

SPM 是所有 JS 执行的唯一入口门控:

```
spm_call(prog, func, this_val, args)
         │
         ├─ Check: prog->state == ACTIVE
         ├─ Check: Core is IDLE or current program == prog
         ├─ Core: IDLE → RUNNING (reset timeout)
         ├─ jerry_call() via SEC
         └─ Core: RUNNING → IDLE
```

**程序状态机**:
```
TERMINATED ← STOPPING ← ACTIVE ⇄ SUSPENDED
```

- **ACTIVE**: 正常运行, 接收所有 JS 回调
- **SUSPENDED**: 表盘专用, 暂停代码执行, 保留状态
- **STOPPING**: 异步等待 Core 停止, 不允许新回调
- **TERMINATED**: 所有资源已释放

### 2.4 Activity Controller — 页面导航栈

```c
// 类型化的 Activity 系统
typedef enum {
    EOS_ACTIVITY_TYPE_APP,
    EOS_ACTIVITY_TYPE_APP_LIST,
    EOS_ACTIVITY_TYPE_WATCHFACE,
    EOS_ACTIVITY_TYPE_WATCHFACE_LIST,
    EOS_ACTIVITY_TYPE_LOCK_SCREEN,
    // ...
} eos_activity_type_t;

// 注册自定义过渡动画
eos_activity_register_anim_route(
    EOS_ACTIVITY_TYPE_WATCHFACE,
    EOS_ACTIVITY_TYPE_APP_LIST,
    my_custom_animation_cb
);
```

**核心概念**:
- **Root Activity** (表盘) — 永远在栈底, 不被 back() 销毁
- **Activity Stack** — push/pop 导航, 支持返回栈
- **`eos_activity_replace_root()`** — 切换表盘时替换 root Activity
- **类型化动画路由** — 不同页面类型间可以注册专属过渡动画
- **View 自动管理** — `eos_activity_create_root()` 自动创建和销毁 View 对象

### 2.5 Chrome Manager — 系统叠加层管理

统一管理系统级叠加层(通知列表、控制中心), 解耦输入和 UI:

```c
typedef struct eos_chrome_overlay_t {
    void (*pull_back)(void);          // 带动画关闭
    void (*hide)(void);               // 立即隐藏
    void (*on_focus)(void);           // 成为栈顶时回调
    bool (*is_open)(void);            // 查询状态
    lv_obj_t *(*get_scrollable)(void); // 获取可滚动对象(表冠输入)
    lv_obj_t *(*get_foreground_obj)(void); // Z-order 管理
} eos_chrome_overlay_t;
```

**表冠点击决策逻辑**:
1. 如果 PM 休眠 → 唤醒
2. 如果有叠加层打开 → 关闭栈顶叠加层
3. 如果在表盘 → 进入应用列表
4. 否则 → 返回上一页

### 2.6 EAPK/EWPK 包管理系统

类似 Android APK 的包格式:

```c
typedef struct {
    char magic[4];              // "EAPK" (应用) / "EWPK" (表盘)
    char pkg_name[256];         // 包名
    char pkg_id[256];           // 软件 ID
    char pkg_version[256];      // 版本
    uint16_t min_api_level;     // 最低 API level
    uint16_t target_api_level;  // 目标 API level
    uint32_t file_count;        // 文件数量
    uint32_t reserved;          // 预留
} eos_pkg_header_t;
```

包内包含:
- `manifest.json` — 元数据、权限声明
- `main.js` — 入口脚本
- `icon.bin` — 图标资源
- 其他资源文件

### 2.7 多语言国际化 (i18n)

内置完整的多语言系统, 支持 EN/ZH 等语言, 所有 UI 字符串通过 `lang_string_id_t` 枚举引用, 支持运行时语言切换。

### 2.8 配置系统

分层的配置系统, 支持:
- `eos_platform_config.h` — 平台手动覆盖
- `eos_config_gen.h` — Kconfig menuconfig 生成
- `eos_config_defaults.h` — 内核默认值

---

## 三、核心优势

### 3.1 脚本化的应用模型 — 最大的差异化优势

大多数手表 UI 方案(LVGL 裸写、AWTK、LittlevGL 直接 API)都是**纯 C 代码开发 UI**。ElenixOS 采用 **JerryScript + SNI 桥接**, 表盘和应用全部用 JS 编写:

- 在 JS 中直接调用 `lv.obj.create()`, `lv.label.create()`, `lv.anim.create()` 等 LVGL API
- 调用 `eos.*` 系统 API(传感器、存储、时间等)
- 表盘就是一个独立的 JS 程序, 可热安装/卸载
- 改表盘无需重新编译固件

### 3.2 清晰的分层与关注点分离

- **SEC (Script Engine Core)** 只关心 JerryScript VM 生命周期, 完全不知道 Activity/View/Watchface
- **SPM** 是 JS 入口的唯一门控(`spm_call`), 所有 JS 回调必须经过 SPM 状态验证
- **SNI Context** 按 Realm 独立管理资源(timer/anim/style 等), 销毁时自动清理, 防止泄漏
- **Chrome Manager** 专门处理系统叠加层的 Z-order 和表冠交互, 解耦了输入和 UI

### 3.3 Activity 栈 + 类型化动画路由

不同页面类型间切换可以注册专属过渡动画, 比简单的 push/pop 更灵活。

### 3.4 EAPK/EWPK 包管理

类似 Android APK 的设计, 应用和表盘都打包成包文件, 支持版本号、API level 兼容性检查、权限声明。使应用分发和生态扩展成为可能。

### 3.5 WebAssembly 在线模拟器

无需硬件即可在浏览器体验完整系统, 降低上手门槛。

---

## 四、与其他手表 UI 方案对比

| 维度 | **ElenixOS** | **LVGL 裸写** | **AWTK (ZLG)** | **PineTime (InfiniTime)** | **Wear OS (Android)** | **watchOS (Apple)** |
|------|:-----------:|:----------:|:------------:|:----------------------:|:-------------------:|:-----------------:|
| **开发语言** | **JS (应用层)** | C | C/XML | C++ | Kotlin/Java | Swift |
| **脚本引擎** | JerryScript | 无 | 无(可选Lua) | 无 | ART (JVM) | Swift Runtime |
| **UI 框架** | LVGL | LVGL | AWTK | LVGL | Jetpack Compose | SwiftUI |
| **应用生态** | EAPK 包管理 | 无标准 | 无标准 | 编译进固件 | Google Play | App Store |
| **表盘脚本化** | ✅ JS 热更 | ❌ 编译 | ❌ 编译 | ❌ 编译 | ✅(WF格式) | ✅ 有限API |
| **RAM 需求** | ~512KB+ | ~64KB+ | ~256KB+ | ~256KB | ~512MB+ | ~1GB+ |
| **Flash 需求** | ~4MB+ | ~256KB+ | ~2MB+ | ~512KB | ~8GB+ | ~32GB+ |
| **MCU 支持** | ✅ ESP32/ARM | ✅ 全平台 | ✅ 全平台 | ✅ nRF52 | ❌ 需AP级 | ❌ 需AP级 |
| **在线模拟器** | ✅ WASM | ❌ | 桌面版 | ❌ | 模拟器 | 模拟器 |
| **国际化(i18n)** | ✅ 内置 | 需自建 | 需自建 | 需自建 | ✅ | ✅ |
| **包权限系统** | ✅ manifest | ❌ | ❌ | ❌ | ✅ | ✅ |
| **活动栈导航** | ✅ Activity栈 | 需自建 | ✅ Navigator | 需自建 | ✅ | ✅ |
| **代码复杂度** | 中(多层抽象) | 低(直接API) | 中高 | 中低 | 高 | 高 |

---

## 五、与其他方案的详细对比

### 5.1 vs LVGL 裸写

LVGL 裸写最轻量, 但所有 UI 都是 C 代码, 改一次 UI 就要重新编译烧录。ElenixOS 通过 JS 脚本化, 改表盘无需重新编译固件。代价是多了一层 JerryScript 开销(~200KB RAM)。

### 5.2 vs AWTK

AWTK 功能更全面(有 Designer 工具、完整控件库), 但架构更重, 且缺少 JS 脚本化的应用/表盘模型。AWTK 适合工业嵌入式 GUI, ElenixOS 更专注手表场景。

### 5.3 vs InfiniTime (PineTime)

InfiniTime 是 C++ + LVGL, 所有功能编译进固件。ElenixOS 的应用生态概念(安装/卸载 EAPK)明显更先进, 但 InfiniTime 系统占用更小。

### 5.4 vs Wear OS / watchOS

这些是完整 OS, 硬件成本高出两个数量级。ElenixOS 目标是在 ESP32 级别的芯片上提供接近 Apple Watch 的交互体验, 是**最具性价比**的方案。

### 5.5 vs esp-brookesia (Espressif 官方手表 UI 框架)

| 维度 | ElenixOS | esp-brookesia |
|------|:--------:|:-------------:|
| 方案类型 | 完整 OS | UI 框架库 |
| 应用模型 | JS 脚本化 + EAPK 包管理 | C 代码编译到固件 |
| 表盘开发 | JS 编写, 热安装 | C 代码, 编译进固件 |
| 上游维护 | 开源社区 | Espressif 官方 |
| 生态完整性 | 完整(Activity栈, 包管理, i18n, 权限) | 轻量(专注 UI 渲染) |
| 适用场景 | 需要应用生态的手表产品 | 快速原型, 固定功能手表 |

---

## 六、总结

ElenixOS 的**核心差异化优势**是:

1. **JS 脚本化应用/表盘模型** — 在 MCU 级平台上实现了接近智能手表 OS 的开发体验
2. **EAPK 包管理 + API level 兼容性** — 使第三方应用分发在嵌入式设备上成为可能
3. **SNI 桥接层的生命周期管理** — 自动化的 JS↔Native 资源清理, 避免嵌入式开发中最头疼的内存泄漏
4. **清晰的分层架构** — SEC/SPM/SNI/Activity/Chrome 职责分明, 可移植性强
5. **WebAssembly 在线模拟器** — 显著降低开发和体验门槛

**最大的权衡**: 额外的 RAM/Flash 开销(JerryScript + SNI Context), 不适合极致低功耗/低资源场景(如 nRF52832 级别), 但在 ESP32 及以上级别芯片上是很好的选择。

---

## 七、在 ESP32-S31 上用 ElenixOS 开发 UI

ElenixOS 提供**两种 UI 开发模式**，根据是做系统内置应用还是可分发应用来选择。

### 7.1 模式一: C 语言开发 (系统内置应用/表盘)

直接调用 ElenixOS 框架和 LVGL API，适合系统核心应用(设置、手电筒等)。

**核心步骤**:

```c
// 1. 定义 Activity 生命周期回调
static const eos_activity_lifecycle_t my_app_lifecycle = {
    .on_enter = my_app_on_enter,
    .on_pause = my_app_on_pause,
    .on_resume = my_app_on_resume,
    .on_destroy = my_app_on_destroy,
};

// 2. 创建 Activity (系统会自动创建 View)
eos_activity_t *a = eos_activity_create(&my_app_lifecycle);
eos_activity_set_type(a, EOS_ACTIVITY_TYPE_APP);
eos_activity_set_title_id(a, STR_ID_MY_APP);
eos_activity_set_app_header_visible(a, true);  // 显示顶部标题栏

// 3. 在 on_enter 中获取 View 并构建 UI
static void my_app_on_enter(eos_activity_t *activity) {
    lv_obj_t *view = eos_activity_get_view(activity);  // 框架自动创建的 View
    // 用 LVGL API 或 ElenixOS 封装控件构建 UI
    lv_obj_t *list = eos_list_create(view);
    lv_obj_t *sw = eos_list_add_switch(list, "开关选项");
}

// 4. 启动 Activity (框架处理动画和导航栈)
eos_activity_enter(a);

// 5. 返回上一页 (框架自动销毁 Activity)
eos_activity_back();
```

**系统已有的 UI 控件** (在 `src/ui/widgets/` 下):
- `eos_list` / `eos_accordion` — 列表控件
- `eos_swipe_panel` / `eos_slide_widget` — 滑动面板
- `eos_card_pager` — 卡片分页器
- `eos_panel` / `eos_toast` / `eos_numpad` — 面板/提示/数字键盘
- `eos_app_header` — 应用标题栏(支持时间显示模式)
- `eos_basic_widgets` / `eos_standard_widgets` — 基础控件封装
- `eos_anim` — 动画工具

**系统服务 API** (可在 UI 中直接调用):
- `eos_display_set_brightness()` — 屏幕亮度控制
- `eos_service_haptic_*()` — 触觉反馈
- `eos_service_audio_*()` — 音频控制
- `eos_service_storage_*()` — 文件存储
- `eos_config_get_bool/set_bool()` — 配置存取

### 7.2 模式二: JavaScript 开发 (可分发应用/表盘)

这是 ElenixOS **最大的差异化优势**。JS 应用打包成 `.eapk` 文件，可安装、卸载、热更新，无需重新编译固件。

**JS 应用结构**:

```
my_app/
├── manifest.json    # 应用元数据和权限声明
├── main.js          # 入口脚本
└── icon.bin         # 应用图标
```

**manifest.json**:
```json
{
  "id": "com.example.myapp",
  "name": "My App",
  "version": "1.0.0",
  "author": "开发者",
  "description": "示例应用",
  "permissions": ["storage", "haptic"]
}
```

**main.js** — JS 中直接使用 LVGL API:
```javascript
// 创建标签
var label = new lv.label(lv.layer_top());
label.set_text("Hello ElenixOS!");
label.align(lv.ALIGN.CENTER, 0, 0);

// 创建按钮
var btn = new lv.btn(lv.layer_top());
btn.set_size(100, 50);
btn.align(lv.ALIGN.CENTER, 0, 50);

var btn_label = new lv.label(btn);
btn_label.set_text("Click");
btn_label.center();

// 注册事件回调
btn.add_event_cb(function(e) {
    eos.haptic.play("click");  // 触觉反馈
    label.set_text("Clicked!");
}, lv.EVENT.CLICKED, null);

// 系统 API 调用
var brightness = eos.display.get_brightness();
eos.config.set_bool("my_setting", true);
```

**JS 表盘结构**:

```
my_watchface/
├── manifest.json    # 表盘元数据
├── main.js          # 表盘脚本
└── snapshot.bin     # 表盘预览截图
```

表盘 JS 被框架自动管理生命周期:
- 进入表盘 → `spm_watchface_start()` → JS 的全局作用域执行
- 切换到应用 → `spm_watchface_pause()` → JS 暂停
- 返回表盘 → `spm_watchface_resume()` → JS 恢复

**打包与部署**:
```bash
# 打包成 EAPK
eos-pkg pack my_app/ -o my_app.eapk

# 部署方式1: 放入 fs/ 目录，编译时打入固件
cp my_app.eapk fs/

# 部署方式2: 拷贝到设备的 /flash 分区
cp my_app.eapk /flash/apps/
```

### 7.3 两种模式对比

| 维度 | C 语言模式 | JavaScript 模式 |
|------|:---------:|:-------------:|
| **开发效率** | 需编译烧录，改一次等几分钟 | 修改 JS 后只需重新安装 EAPK |
| **分发方式** | 编译进固件 | EAPK 包文件，独立分发 |
| **权限控制** | 无限制 | manifest 声明权限 |
| **性能** | 最优(无脚本开销) | 略有 JerryScript 开销 |
| **可用的 UI 控件** | 全部(C 原生 + 框架封装) | LVGL 核心控件(通过 SNI 桥接) |
| **适用场景** | 系统核心应用(设置、手电筒) | 第三方应用、表盘 |
| **学习成本** | 需要 C 和 LVGL 知识 | 只需要 JavaScript |

### 7.4 实际开发建议

1. **从 C 模式开始** — 先熟悉 ElenixOS 框架的 Activity/View 生命周期，把手电筒和设置的源码读一遍
2. **了解 ElenixOS 封装的 UI 控件** — `eos_list`、`eos_swipe_panel`、`eos_card_pager` 等，它们是手表 UX 的核心组件
3. **用 JS 做表盘** — 表盘最适合 JS 脚本化，因为表盘最需要快速迭代和视觉调整
4. **系统应用用 C** — 需要深度硬件交互的应用(如设置)用 C 写更合适

---

## 八、vs SquareLine Studio 对比

SquareLine Studio 和 ElenixOS 的 UI 开发方式本质上是**两种不同的范式**: 可视化拖拽 vs 代码驱动。

### 8.1 核心差异: 拖拽 vs 代码

| 维度 | **SquareLine Studio** | **ElenixOS (C模式)** | **ElenixOS (JS模式)** |
|------|:---------------------:|:--------------------:|:--------------------:|
| **开发方式** | 🖱️ 可视化拖拽 + 少量代码 | ⌨️ 纯 C 代码 | ⌨️ 纯 JS 代码 |
| **上手门槛** | 极低(非程序员也能用) | 高(需要 C + LVGL) | 中(需要 JS) |
| **灵活性** | 受限(拖拽能力有限) | 完全自由 | 完全自由 |
| **改 UI 后** | 导出代码 → 编译 → 烧录 | 编译 → 烧录 | 重新打包 EAPK → 安装 |
| **动态换肤/换表盘** | ❌ 不支持 | ❌ 不支持 | ✅ 支持(EAPK 安装/卸载) |
| **学习 LVGL** | 不需要 | 需要深入理解 | 需要理解 LVGL 概念 |
| **适合谁** | 设计师、快速原型 | 嵌入式工程师 | 前端开发者 |

### 8.2 SquareLine 的"简单"是有限的

SquareLine Studio 拖几个控件、设个颜色确实简单，但存在以下限制:

1. **稍微复杂的交互逻辑**就得写 C 回调代码，而 SquareLine 生成的代码结构很死板，改起来比从头写还痛苦
2. **屏幕适配是噩梦** — 换个分辨率所有布局都得重新拖一遍
3. **改一次 UI 就得重新编译烧录**，改个颜色都要等 2-3 分钟
4. **不能动态加载 UI** — 所有界面编译死在固件里，没法像 ElenixOS 的 JS 表盘那样热更新
5. **版本管理困难** — SquareLine 的二进制工程文件没法 diff

### 8.3 结论: 不同场景不同答案

```
简单 = 上手快 + 改得快 + 不踩坑
```

| 场景 | 哪个更简单 |
|------|:--------:|
| 做固定 UI 的原型验证 | **SquareLine** ✅ (拖几下就出来了) |
| 做需要频繁调整的表盘 UI | **ElenixOS JS** ✅ (改 JS 不重编译) |
| 做复杂交互的应用 | **ElenixOS** ✅ (代码全控，没 SquareLine 的限制) |
| 非程序员想快速上手 | **SquareLine** ✅ (不用写代码) |
| 做了要分发/更新的产品 | **ElenixOS** ✅ (EAPK 包管理) |
| 跨分辨率适配 | **ElenixOS** ✅ (代码比拖拽好适配) |

**简单来说**: SquareLine 是"前 10 分钟简单"，ElenixOS 是"第 2 天开始简单"。如果只是画个静态界面截图，SquareLine 更快；如果要做一个实际的可持续迭代的手表产品，ElenixOS 的 JS 模式长期更简单。
