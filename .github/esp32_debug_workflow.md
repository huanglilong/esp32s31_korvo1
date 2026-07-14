# ESP32 Debug Workflow (Build, Flash, Serial Logs)

> For environments where `idf.py monitor` is unavailable (no TTY).<br>
> Works for ESP32-S2/S3/C3/C5/C6/P4/S31 and other ESP32-series chips.

## Prerequisites

```bash
# One-time environment setup
source ~/.espressif/v6.x/esp-idf/export.sh >/dev/null 2>&1
cd <workspace>
idf.py set-target <chip>      # e.g. esp32s31, esp32s3, esp32p4
```

## Step 1: Build

```bash
cd <workspace>
source ~/.espressif/v6.x/esp-idf/export.sh >/dev/null 2>&1
idf.py build
```

> If `sdkconfig.defaults` changed, run `idf.py fullclean` first.

## Step 2: Flash

**Linux**: `idf.py flash -b 1500000 -p $(bash -c "ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1")`
**macOS**: `idf.py flash -b 1500000 -p $(bash -c "ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -1")`

## Step 3: Capture Serial Logs (Python + DTR Reset)

Toggling DTR triggers a hardware reset (EN pin), capturing the full boot sequence.

**Linux** (`/dev/ttyUSB0` or `/dev/ttyACM0`):

```python
python3 -c "
import serial, time
ser = serial.Serial('/dev/ttyUSB0', 115200, timeout=1)
ser.setDTR(False)           # Pull DTR low to reset (EN pin)
time.sleep(0.2)
ser.setDTR(True)            # Release reset
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
for line in text.split(chr(10)):
    l = line.strip()
    if l:
        print(l)
"
```

**macOS** (`/dev/cu.usbmodem*` or `/dev/cu.usbserial*`): replace the port path.

## Step 4: Filter Logs by Keyword

Replace the final `for` loop with targeted filtering:

```python
for line in text.split(chr(10)):
    l = line.strip()
    if l and any(k in l for k in ['error', 'Error', 'panic', 'keyword']):
        print(l)
```

Adjust the keyword list to match what you're debugging.
