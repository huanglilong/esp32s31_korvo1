# WiFi connection state
uint64  timestamp
char[32] ssid           # Connected SSID
int8    rssi            # Current RSSI (dBm)
bool    connected       # STA connected to AP
bool    scanning        # Scan in progress
