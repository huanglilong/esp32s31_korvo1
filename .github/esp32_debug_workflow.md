# ESP32 Debug Workflow (Build, Flash, Serial Logs)

> Automated debug workflow for environments without `idf.py monitor` (no TTY).<br>
> Works for ESP32-S2/S3/C3/C5/C6/P4/S31 and other ESP32-series chips.<br>
> **Supports both macOS and Linux** — serial port is auto-detected per OS.

## Platform Compatibility

This workflow is tested and supported on **both macOS and Linux**:

| Feature | macOS | Linux |
|---------|-------|-------|
| Serial port auto-detect | `/dev/cu.usbmodem*`, `/dev/cu.usbserial*` | `/dev/ttyACM*`, `/dev/ttyUSB*` |
| Shell | bash 3.2+ (system default) | bash 4.0+ |
| Python | 3.x with `pyserial` | 3.x with `pyserial` |
| DTR reset | ✅ | ✅ |
| Flash baud 1500000 | ✅ | ✅ |

> **Note**: The script uses `uname -s` to detect the OS and adjusts serial port
> discovery accordingly. No platform-specific configuration is needed — just
> run `./tools/esp32_debug.sh` on either OS.

## Quick Start

```bash
# Full cycle: build → flash → capture → analyze (one command)
./tools/esp32_debug.sh

# That's it. Logs are auto-saved to build/debug_log_YYYYMMDD_HHMMSS.txt
```

## Commands

| Command | Description |
|---------|-------------|
| `all` (default) | Build → Flash → Capture → Analyze |
| `build` | Build only |
| `flash` | Flash only (requires prior build) |
| `capture` | Capture serial logs only (no build/flash) |
| `filter` | Filter a previously saved log by keywords |

## Options

| Option | Description | Default |
|--------|-------------|---------|
| `-p PORT` | Serial port (auto-detected if omitted) | auto |
| `-b BAUD` | Flash baud rate | 1500000 |
| `-t SECONDS` | Capture duration | 40 |
| `-k KEYWORDS` | Comma-separated filter keywords | error,panic,assert,abort,exception,backtrace,Guru |
| `-o FILE` | Save raw log to file | `build/debug_log_YYYYMMDD_HHMMSS.txt` |
| `-n` | Dry run — show commands without executing | — |

## Common Usage

```bash
# Full debug cycle
./tools/esp32_debug.sh

# Capture only (device already flashed)
./tools/esp32_debug.sh capture

# Longer capture for intermittent issues
./tools/esp32_debug.sh capture -t 120

# Specify port explicitly
./tools/esp32_debug.sh all -p /dev/cu.usbmodem1101

# Filter saved log for specific keywords
./tools/esp32_debug.sh filter -k wifi,audio

# Dry run (see what would be executed)
./tools/esp32_debug.sh -n
```

## Pytest Integration During Capture

**During the capture phase**, also run the test suite to verify functionality end-to-end:

```bash
# Terminal 1: start capture with extended duration (tests may take 2–5 minutes)
./tools/esp32_debug.sh capture -t 300

# Terminal 2: run pytest while capture is running
pytest tests --base-url=http://esp-web.local:8080 -v
```

**Key points:**

- The capture duration (`-t`) must be long enough to cover the full test run. Default 40s is too short — use **`-t 300`** (5 minutes) as a safe default for the full suite.
- Capture continues until the timer expires, so start pytest promptly after capture begins.
- If pytest finishes early, the remaining capture time still logs device state — useful for spotting post-test issues.
- If pytest fails, check the captured log for errors that correlate with the failure.

## How It Works

### Serial Port Auto-Detection

The script auto-detects the serial port based on OS:

| OS | Search Pattern | Priority |
|----|---------------|----------|
| macOS | `/dev/cu.usbmodem*`, `/dev/cu.usbserial*` | First match |
| Linux | `/dev/ttyACM*`, `/dev/ttyUSB*` | First match |

If multiple ports are found, the first is used and others are shown as warnings.
Override with `-p PORT` if needed.

### DTR Reset

Toggling DTR triggers a hardware reset (EN pin), ensuring the **full boot sequence** is captured — not just whatever was already printing.

### Log Analysis (Automatic)

After capture, the script automatically:
1. **Saves** raw log to `build/debug_log_YYYYMMDD_HHMMSS.txt`
2. **Counts** INFO / WARN / ERROR log levels
3. **Displays** all ERROR lines (if any)
4. **Displays** meaningful warnings (excludes noisy `wifi:(phy)rate:` lines)
5. **Checks** boot completion status and free heap

### Keyword Filtering

```bash
# Filter with custom keywords
./tools/esp32_debug.sh filter -k "wifi,timeout,SD"

# Uses the latest log file in build/ by default
# Specify a file with -o
./tools/esp32_debug.sh filter -k "audio" -o build/debug_log_20260715_024400.txt
```

## Manual Steps (if script unavailable)

If you need to run steps manually without the script:

### Prerequisites

```bash
source ~/.espressif/v6.x/esp-idf/export.sh >/dev/null 2>&1
cd <workspace>
idf.py set-target <chip>      # e.g. esp32s31, esp32s3, esp32p4
```

### Build & Flash

```bash
idf.py build
# macOS:
idf.py flash -b 1500000 -p $(bash -c "ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -1")
# Linux:
idf.py flash -b 1500000 -p $(bash -c "ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1")
```

### Capture Serial Logs

```python
python3 -c "
import serial, time
# macOS:   /dev/cu.usbmodem* or /dev/cu.usbserial*
# Linux:   /dev/ttyACM* or /dev/ttyUSB*
ser = serial.Serial('/dev/YOUR_PORT', 115200, timeout=1)
ser.setDTR(False)           # Reset (EN pin)
time.sleep(0.2)
ser.setDTR(True)
time.sleep(0.5)
data = b''
start = time.time()
while time.time() - start < 40:
    chunk = ser.read(4096)
    if chunk:
        data += chunk
    time.sleep(0.05)
ser.close()
text = data.decode('utf-8', errors='replace')
with open('debug_log.txt', 'w') as f:    # ← save to file
    f.write(text)
for line in text.split(chr(10)):
    l = line.strip()
    if l:
        print(l)
"
```

### Filter Logs

```bash
# Error/panic only
grep -E '(E \(|panic|assert|abort|Guru)' debug_log.txt

# Warnings (excluding wifi phy rate noise)
grep 'W (' debug_log.txt | grep -v 'wifi:(phy)rate:'
```
