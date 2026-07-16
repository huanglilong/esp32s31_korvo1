# Copilot Instructions — ESP32-S31 Project

> **Framework**: ESP-IDF v6.x | **Language**: C++ (app) + C/C++ (components)
> **Board**: ESP32-S31-Korvo-1 V1.1 | **Chip**: ESP32-S31 (RISC-V dual-core, up to 320 MHz)
> **Docs**: [README.md](../README.md) (hardware) | [PROJECT.md](../PROJECT.md) (software) | [PROJECT_REQUIREMENTS.md](../PROJECT_REQUIREMENTS.md) (requirements) | [esp32s31_korvo1_hardware_info.md](doc/esp32s31_korvo1_hardware_info.md) (detailed HW)

---

## 1. Startup Procedure (CRITICAL)

**ALWAYS** at the start of every session:

1. **Read** `README.md` — hardware info, pin connections, build commands
2. **Read** `PROJECT.md` — software architecture, project structure, code conventions
3. **Read** `PROJECT_REQUIREMENTS.md` — requirements, pending items, change log
4. **Read** `doc/esp32s31_korvo1_hardware_info.md` — detailed hardware, peripherals, chip specs
5. **Plan** before starting any work
6. **After finishing all tasks**:
   - Update `README.md` / `PROJECT.md` / `PROJECT_REQUIREMENTS.md` as needed
   - Update this file if workflow/conventions change
   - Provide a summary of what was done, why, and remaining TODOs
7. **If the project crashes**: check logs → find root cause → add diagnostic logs if unclear

---

## 2. Project Structure

```
├── main/                    # Application code (C++) — EDIT FREELY
│   ├── drivers/             # Peripheral driver modules
│   ├── generated/           # ⚠️ AUTO-GENERATED — DO NOT EDIT
│   └── compat/              # Third-party compatibility shims
├── doc/                     # Hardware & project documentation
│   └── esp32s31_korvo1_hardware_info.md
├── components/              # ⚠️ Local/BSP components — edit with caution
├── managed_components/      # ⚠️ ESP-IDF managed — DO NOT EDIT
├── sdkconfig.defaults       # Default Kconfig — edit this, NOT sdkconfig
└── partitions.csv           # Partition table
```

---

## 3. Code Conventions

### 3.1 Style

- Follow **Project C/C++ Style** for formatting and naming
- **C++** for `main/`, **C/C++** for `components/`
- C++ designated initializer field order MUST match struct declaration order
- Enum types require explicit casts from int (e.g., `gpio_num_t`)
- C headers included from C++ need `extern "C"` wrapper

### 3.2 Thread Safety (ESP32-S31 Dual-Core RISC-V)

- ESP32-S31 is a **dual-core** 32-bit RISC-V SoC (HP core + LP core), up to 320 MHz
- HP RISC-V core handles application tasks; LP RISC-V core handles low-power operations
- Use `std::atomic<T>` for ALL cross-core/cross-task shared variables — **never** `volatile`
- Use `std::atomic<bool>` with `.store(release)` / `.load(acquire)` for flags
- Use `compare_exchange_strong()` for lazy one-time init of shared handles
- Use FreeRTOS Mutex for multi-step critical sections
- Use atomic counter to wait for in-flight ops before deinit
- Use atomic flag for safe task cleanup — avoid `eTaskGetState()` on self-deleting tasks
- Let tasks self-delete after releasing resources — never `vTaskDelete()` on tasks holding mutexes
- Large-stack tasks: use `xTaskCreateStatic` with PSRAM-allocated stack

### 3.3 Error Handling

- **Never** use `assert()` for runtime checks — compiled out in release builds
- Use explicit null checks + `ESP_LOGE` + graceful degradation
- **Always** check return values of ESP-IDF API calls
- **Always** validate array indices from hardware/driver responses
- **Always** validate `Content-Length` before `calloc()` in HTTP handlers
- **Always** sanitize file paths in HTTP handlers — prevent path traversal
- **Never** expose secrets in HTTP responses — use `has_*` boolean flags

### 3.4 Memory

- Use `new(std::nothrow)` — ESP-IDF disables C++ exceptions, `bad_alloc` calls `abort()`
- Use `heap_caps_free()` for `heap_caps_malloc`/`heap_caps_realloc` allocations
- Use `cJSON_free()` for `cJSON_Print*` results
- Use `esp_timer_get_time()` (monotonic) for durations — `gettimeofday()` can overflow

---

## 4. Build, Flash, Monitor & Test

### 4.1 Quick Commands

```bash
source ~/.espressif/v6.x/esp-idf/export.sh            # Setup environment (once per shell)
idf.py set-target esp32s31                            # Set target chip (once)
idf.py fullclean                                      # Full clean (only when config changed)
idf.py build                                          # Build
idf.py flash monitor                                  # Flash + monitor serial output
# In another terminal (same source), once device connects to WiFi:
pytest tests --base-url=http://esp-web.local:8080 -v  # Run tests (requires device + WiFi)
```

**Linux**: `idf.py flash -b 1500000 -p $(bash -c "ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1") monitor`
**macOS**: `idf.py flash -b 1500000 -p $(bash -c "ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -1") monitor`

After code changes, rebuild. **⚠️ If `sdkconfig.defaults` changed, MUST delete `sdkconfig`, then `idf.py build` — sdkconfig is NOT auto-regenerated from defaults on incremental build!**

### 4.2 Debug Workflow (no TTY)

When `idf.py monitor` is unavailable, use the automated debug script:

```bash
./tools/esp32_debug.sh              # Build → Flash → Capture → Analyze
./tools/esp32_debug.sh capture      # Capture logs only
./tools/esp32_debug.sh filter -k wifi,audio  # Filter saved logs
```

See [esp32_debug_workflow.md](.github/esp32_debug_workflow.md) for full documentation.

---

## 5. Protected Files

| Path | Rule | Reason |
|------|------|--------|
| `managed_components/` | **DO NOT EDIT** | ESP-IDF managed, overwritten on build |
| `main/generated/` | **DO NOT EDIT** | Auto-generated code |
| `sdkconfig` | **DO NOT EDIT** | Generated from `sdkconfig.defaults` |
| `doc/` | **Read/write** | Hardware documentation, project docs |
| `components/` | **Edit with caution** | Only if necessary and approved |

---

## 6. Git Rules

1. **Every commit must be approved by user**
2. **git push is forbidden** — never push without explicit request
3. **One issue, one commit** — each commit addresses exactly ONE issue/feature/bugfix
4. **Commit messages**: clear, concise, in English, with **root cause and summary of what was done and why**
5. **Before committing**: verify build passes (`idf.py build`), and flash/monitor works (`idf.py flash monitor`) and all tests pass (`pytest tests --base-url=http://esp-web.local:8080 -v`)

---

## 7. Code Review & Fix Workflow

1. Review for issues and improvements
2. **One issue, one commit** — fix each in its own commit
3. After fixing, run another review; repeat up to **2 rounds max**
4. If issues remain after 2 rounds, report to user
5. Update all relevant `*.md` docs
6. Flash to device and monitor logs to verify
7. After all issues resolved, run **Build, Flash, Monitor & Test** to verify functionality

---

## 8. Reference Projects

| Project | Reference |
|---------|-----------|
| [ESP-IDF Examples](https://github.com/espressif/esp-idf/tree/master/examples) | Official ESP-IDF examples |
| [ESP-Skainet](https://github.com/espressif/esp-skainet) | Espressif AI voice assistant SDK |
| [ESP-Claw](https://github.com/espressif/esp-claw) | AI agent, IM, LLM, MCP |

When modifying `components/` code, follow its existing coding style and architecture.

---

## 9. Common Pitfalls

| Pitfall | Correct Approach |
|---------|-----------------|
| `volatile` for cross-core/cross-task vars | `std::atomic<T>` with explicit memory ordering |
| `TaskHandle_t` for cross-core task handles | `_Atomic TaskHandle_t` with `memory_order_release/acquire` |
| Bare `new` | `new(std::nothrow)` + null check |
| `free()` for `heap_caps_*` allocs | `heap_caps_free()` |
| `free()` for cJSON strings | `cJSON_free()` |
| `assert()` for runtime checks | Explicit null/error checks + `ESP_LOGE` |
| `eTaskGetState()` on self-deleting tasks | Atomic flag for task exit status |
| `vTaskDelete()` on mutex-holding tasks | Let task self-delete after release |
| `gettimeofday()` for durations | `esp_timer_get_time()` (monotonic) |
| Releasing mutex mid-operation (TOCTOU) | Double-check shared state after re-acquisition |
| Calling blocking ops while holding mutex | Release mutex before blocking call |
| Editing `sdkconfig` | Edit `sdkconfig.defaults` + **delete `sdkconfig` then rebuild** |
| Changing `sdkconfig.defaults` without deleting `sdkconfig` | Delete `sdkconfig`, then `idf.py fullclean && idf.py build` (sdkconfig NOT auto-regenerated from defaults on incremental build) |
| Editing `main/generated/` | Edit source files + regenerate |

## 10. Board-Specific Notes (ESP32-S31-Korvo-1 V1.1)

### 10.1 Key Peripherals

| Peripheral | Interface | Notes |
|-----------|-----------|-------|
| ES8389 Audio Codec | I2S + I2C | Stereo ADC/DAC, dual mic input, dual speaker output |
| NS4150B Audio PA (x2) | Analog from Codec | 3W Class-D, 4Ω speakers |
| microSD Card | SDIO 3.0 (4-bit) | Audio storage and playback |
| DVP Camera (OV3660) | DVP parallel | External camera connector |
| LCD Subboard | LCD connector | ESP32-S3-LCD-EV-Board-SUB3 |
| RGB LED | GPIO8 | Addressable (WS2812-compatible) |
| Function Buttons | GPIO | PLAY, SET, VOL-, VOL+ |
| USB OTG | USB 2.0 HS | Host mode, 500mA output |

### 10.2 Audio Pipeline

```
Analog Mics (L+R) → ES8389 ADC → I2S → ESP32-S31 → I2S → ES8389 DAC → NS4150B PA (L+R) → Speakers
```

### 10.3 Power Requirements

- Total input current must meet **3A** when driving high-power speakers + USB Host
- Audio circuitry uses independent LDO power rail to reduce digital noise
- Two USB Type-C ports: one power-only, one UART + power
