#!/usr/bin/env bash
# esp32_debug.sh — ESP32 Build, Flash, Serial Log Capture & Analysis
#
# Supports: macOS (bash 3.2+) and Linux (bash 4.0+)
#
# Usage:
#   ./tools/esp32_debug.sh [COMMAND] [OPTIONS]
#
# Commands:
#   all       Build → Flash → Capture (default)
#   build     Build only
#   flash     Flash only (requires prior build)
#   capture   Capture serial logs only (no build/flash)
#   filter    Filter a previously saved log file
#
# Options:
#   -p PORT       Serial port (auto-detected if omitted)
#   -b BAUD       Flash baud rate (default: 1500000)
#   -t SECONDS    Capture duration in seconds (default: 40)
#   -k KEYWORDS   Comma-separated filter keywords (default: error,Error,ERROR,panic,PANIC,assert,ASSERT,abort,ABORT,exception,EXCEPTION,backtrace,BACKTRACE,Guru,guru)
#   -o FILE       Save raw log to FILE (default: build/debug_log_YYYYMMDD_HHMMSS.txt)
#   -n            Dry run — show commands without executing
#   -h            Show this help
#
# Examples:
#   ./tools/esp32_debug.sh                          # Full cycle: build + flash + capture
#   ./tools/esp32_debug.sh capture -t 60            # Capture 60s of logs
#   ./tools/esp32_debug.sh filter -k wifi,audio     # Filter saved log for wifi/audio
#   ./tools/esp32_debug.sh all -p /dev/cu.usbmodem1101  # Specify port explicitly

set -euo pipefail

# ─── Colors ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ─── Defaults ─────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
BAUD=1500000
CAPTURE_DURATION=40
PORT=""
LOG_FILE=""
DRY_RUN=false
DEFAULT_KEYWORDS="error,Error,ERROR,panic,PANIC,assert,ASSERT,abort,ABORT,exception,EXCEPTION,backtrace,BACKTRACE,Guru,guru"
KEYWORDS="${DEFAULT_KEYWORDS}"

# ─── Helpers ──────────────────────────────────────────────────────────────────
info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
fail()  { echo -e "${RED}[FAIL]${NC}  $*"; exit 1; }

run_cmd() {
    if $DRY_RUN; then
        echo -e "${YELLOW}[DRY]${NC}   $*"
    else
        eval "$@"
    fi
}

# ─── Auto-detect serial port ─────────────────────────────────────────────────
detect_port() {
    if [[ -n "${PORT}" ]]; then
        if ! $DRY_RUN && [[ ! -e "${PORT}" ]]; then
            fail "Specified port ${PORT} does not exist"
        fi
        $DRY_RUN && warn "Dry run: assuming port ${PORT} exists"
        return 0
    fi

    local os="$(uname -s)"
    local candidates=()

    case "${os}" in
        Darwin)
            # macOS: prefer cu.usbmodem* (USB-CDC/JTAG), fallback to cu.usbserial*
            candidates=($(ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null || true))
            ;;
        Linux)
            # Linux: try ACM first (USB-CDC/JTAG), then USB serial
            candidates=($(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true))
            ;;
        *)
            fail "Unsupported OS: ${os}"
            ;;
    esac

    if [[ ${#candidates[@]} -eq 0 ]]; then
        fail "No serial port found. Connect the ESP32 board and try again, or specify -p PORT"
    fi

    # If multiple, prefer the first and warn about others
    PORT="${candidates[0]}"
    if [[ ${#candidates[@]} -gt 1 ]]; then
        warn "Multiple serial ports found, using ${PORT}. Others: ${candidates[*]:1}"
        warn "Use -p PORT to specify a different port"
    fi

    ok "Serial port: ${PORT}"
}

# ─── Setup ESP-IDF environment ───────────────────────────────────────────────
setup_idf() {
    if [[ -z "${IDF_PATH:-}" ]]; then
        local idf_path="${HOME}/.espressif/v6.x/esp-idf"
        if [[ ! -d "${idf_path}" ]]; then
            fail "ESP-IDF not found at ${idf_path}. Install it first."
        fi
        info "Sourcing ESP-IDF environment..."
        source "${idf_path}/export.sh" >/dev/null 2>&1
    fi

    # Verify
    if ! command -v idf.py &>/dev/null; then
        fail "idf.py not found after sourcing ESP-IDF. Check your installation."
    fi
}

# ─── Step 1: Build ───────────────────────────────────────────────────────────
do_build() {
    info "Building project..."
    cd "${PROJECT_DIR}"
    if ! run_cmd "idf.py build"; then
        fail "Build failed"
    fi
    ok "Build succeeded"
}

# ─── Step 2: Flash ───────────────────────────────────────────────────────────
do_flash() {
    detect_port
    info "Flashing at ${BAUD} baud via ${PORT}..."
    cd "${PROJECT_DIR}"
    if ! run_cmd "idf.py flash -b ${BAUD} -p ${PORT}"; then
        fail "Flash failed"
    fi
    ok "Flash succeeded"
}

# ─── Step 3: Capture serial logs ─────────────────────────────────────────────
do_capture() {
    detect_port

    # Generate log filename if not specified
    if [[ -z "${LOG_FILE}" ]]; then
        local timestamp="$(date +%Y%m%d_%H%M%S)"
        LOG_FILE="${BUILD_DIR}/debug_log_${timestamp}.txt"
    fi

    # Ensure build dir exists for log storage
    mkdir -p "$(dirname "${LOG_FILE}")"

    info "Capturing serial logs for ${CAPTURE_DURATION}s from ${PORT}..."
    info "Log will be saved to: ${LOG_FILE}"

    # Python capture script with DTR reset
    # Values passed via environment variables to avoid injection risks
    if $DRY_RUN; then
        echo -e "${YELLOW}[DRY]${NC}   python3 -c \"<capture script: ${CAPTURE_DURATION}s from ${PORT}>\""
    else
        ESP_PORT="${PORT}" ESP_LOG_FILE="${LOG_FILE}" ESP_CAPTURE_SECS="${CAPTURE_DURATION}" \
        python3 -c "
import serial, time, sys, os

port = os.environ['ESP_PORT']
log_file = os.environ['ESP_LOG_FILE']
capture_secs = int(os.environ['ESP_CAPTURE_SECS'])

try:
    ser = serial.Serial(port, 115200, timeout=1)
except serial.SerialException as e:
    print(f'ERROR: Cannot open {port}: {e}', file=sys.stderr)
    sys.exit(1)

# DTR reset to capture full boot sequence
ser.setDTR(False)
time.sleep(0.2)
ser.setDTR(True)
time.sleep(0.5)

data = b''
start = time.time()
while time.time() - start < capture_secs:
    chunk = ser.read(4096)
    if chunk:
        data += chunk
    time.sleep(0.05)
ser.close()

text = data.decode('utf-8', errors='replace')

# Save raw log
with open(log_file, 'w') as f:
    f.write(text)

# Print to stdout
for line in text.split(chr(10)):
    l = line.strip()
    if l:
        print(l)
" || fail "Serial capture failed. Check port ${PORT} and pyserial installation."
    fi

    ok "Log saved to: ${LOG_FILE}"

    # Auto-analyze: show summary
    if [[ -f "${LOG_FILE}" ]] && ! $DRY_RUN; then
        analyze_log "${LOG_FILE}"
    fi
}

# ─── Step 4: Analyze / Filter log ────────────────────────────────────────────
analyze_log() {
    local log_file="$1"
    if [[ ! -f "${log_file}" ]]; then
        fail "Log file not found: ${log_file}"
    fi

    echo ""
    echo -e "${BOLD}═══ Log Analysis ═══${NC}"

    # Count log levels
    local error_count=$(grep -c 'E (' "${log_file}" 2>/dev/null) || error_count=0
    local warn_count=$(grep -c 'W (' "${log_file}" 2>/dev/null) || warn_count=0
    local info_count=$(grep -c 'I (' "${log_file}" 2>/dev/null) || info_count=0
    local total_lines=$(wc -l < "${log_file}" 2>/dev/null || echo 0)

    echo -e "  Total lines: ${total_lines}"
    echo -e "  ${GREEN}INFO${NC}:    ${info_count}"
    echo -e "  ${YELLOW}WARN${NC}:    ${warn_count}"
    echo -e "  ${RED}ERROR${NC}:   ${error_count}"

    # Show errors if any
    if [[ "${error_count}" -gt 0 ]]; then
        echo ""
        echo -e "${RED}── Errors Found ──${NC}"
        grep 'E (' "${log_file}"
    fi

    # Show warnings (excluding known harmless wifi phy rate lines)
    local real_warnings=$(grep 'W (' "${log_file}" 2>/dev/null | grep -v 'wifi:(phy)rate:' || true)
    if [[ -n "${real_warnings}" ]]; then
        echo ""
        echo -e "${YELLOW}── Warnings (excluding wifi phy rate) ──${NC}"
        echo "${real_warnings}"
    fi

    # Boot status summary
    local boot_complete=$(grep -c "Boot complete" "${log_file}" 2>/dev/null) || boot_complete=0
    local free_heap=$(grep "Free heap:" "${log_file}" 2>/dev/null | tail -1 || echo "N/A")

    echo ""
    echo -e "${BOLD}── Boot Summary ──${NC}"
    if [[ "${boot_complete}" -gt 0 ]]; then
        echo -e "  Boot: ${GREEN}COMPLETE${NC}"
    else
        echo -e "  Boot: ${RED}INCOMPLETE (no 'Boot complete' message)${NC}"
    fi
    echo -e "  ${free_heap}"

    echo ""
    ok "Analysis complete. Use 'filter' command for keyword search."
}

do_filter() {
    local log_file="${LOG_FILE}"

    # Find the most recent log file if not specified
    if [[ -z "${log_file}" ]]; then
        local latest=$(ls -t "${BUILD_DIR}"/debug_log_*.txt 2>/dev/null | head -1)
        if [[ -z "${latest}" ]]; then
            fail "No log files found in ${BUILD_DIR}/. Run 'capture' first."
        fi
        log_file="${latest}"
        info "Using latest log: ${log_file}"
    fi

    if [[ ! -f "${log_file}" ]]; then
        fail "Log file not found: ${log_file}"
    fi

    info "Filtering ${log_file} for keywords: ${KEYWORDS}"

    # Values passed via environment variables to avoid injection risks
    ESP_LOG_FILE="${log_file}" ESP_KEYWORDS="${KEYWORDS}" \
    python3 -c "
import sys, os

log_file = os.environ['ESP_LOG_FILE']
keywords = [k.strip() for k in os.environ['ESP_KEYWORDS'].split(',') if k.strip()]

with open(log_file, 'r') as f:
    for line in f:
        stripped = line.strip()
        if stripped and any(k in stripped for k in keywords):
            print(stripped)
"
}

# ─── Parse arguments ─────────────────────────────────────────────────────────
COMMAND="all"

while [[ $# -gt 0 ]]; do
    case "$1" in
        all|build|flash|capture|filter)
            COMMAND="$1"
            shift
            ;;
        -p)
            PORT="$2"
            shift 2
            ;;
        -b)
            BAUD="$2"
            shift 2
            ;;
        -t)
            CAPTURE_DURATION="$2"
            shift 2
            ;;
        -k)
            KEYWORDS="$2"
            shift 2
            ;;
        -o)
            LOG_FILE="$2"
            shift 2
            ;;
        -n)
            DRY_RUN=true
            shift
            ;;
        -h|--help)
            head -30 "${BASH_SOURCE[0]}" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        *)
            fail "Unknown option: $1. Use -h for help."
            ;;
    esac
done

# ─── Main ────────────────────────────────────────────────────────────────────
echo -e "${BOLD}ESP32 Debug Workflow${NC} — ${COMMAND}"
echo ""

setup_idf

case "${COMMAND}" in
    all)
        do_build
        do_flash
        do_capture
        ;;
    build)
        do_build
        ;;
    flash)
        do_flash
        ;;
    capture)
        do_capture
        ;;
    filter)
        do_filter
        ;;
esac

echo ""
ok "Done!"
