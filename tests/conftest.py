"""Shared fixtures and helpers for ESP32-S31 Korvo-1 web config integration tests."""

import os
import sys
import time
import pytest
import requests


def _log(msg: str = "", *, color: str | None = None):
    """Write a line directly to the terminal, bypassing pytest output capture.

    Opens ``/dev/tty`` (the controlling terminal) so output is visible
    even when pytest redirects stdout/stderr (e.g. ``-v`` without ``-s``).
    Falls back to ``sys.stderr`` if ``/dev/tty`` is unavailable.
    """
    colors = {
        "green":  "\033[32m",
        "yellow": "\033[33m",
        "red":    "\033[31m",
        "bold":   "\033[1m",
    }
    reset = "\033[0m"
    line = f"{msg}\n"
    if color:
        line = f"{colors.get(color, '')}{msg}{reset}\n"
    try:
        with open("/dev/tty", "w", encoding="utf-8", errors="replace") as tty:
            tty.write(line)
            tty.flush()
    except OSError:
        sys.stderr.write(line)
        sys.stderr.flush()


# ── CLI options ────────────────────────────────────────────────────────


def pytest_addoption(parser):
    parser.addoption(
        "--base-url",
        default=None,
        help="ESP32-S31 base URL (e.g., http://10.0.0.42:8080). "
             "Overrides ESP_BASE_URL env var and default.",
    )
    parser.addoption(
        "--wait-timeout",
        default=None,
        help="Max seconds to wait for device WiFi connection before tests "
             "(default: 120). Overrides ESP_WAIT_TIMEOUT env var.",
    )
    parser.addoption(
        "--no-wait",
        action="store_true",
        default=False,
        help="Skip the device WiFi readiness check entirely.",
    )


# ── Resolve base URL ──────────────────────────────────────────────────


def _resolve_base_url(config) -> str:
    """Resolve base URL from CLI / env / default."""
    cli = config.getoption("--base-url")
    if cli:
        return cli
    env = os.environ.get("ESP_BASE_URL")
    if env:
        return env
    return "http://esp-web.local:8080"


# ── Hook: WiFi readiness check (runs before any test) ─────────────────


def pytest_collection_modifyitems(config, items):
    """Hook: runs after collection, before any test execution.

    Wait for device WiFi connection here so the readiness check
    appears *before* the first test name in the output.
    """
    if config.getoption("--no-wait"):
        _log("⏩  Skipping device readiness check (--no-wait)", color="yellow")
        return

    base_url = _resolve_base_url(config)
    max_wait = (
        config.getoption("--wait-timeout")
        or os.environ.get("ESP_WAIT_TIMEOUT")
        or 120
    )
    max_wait = int(max_wait)
    interval = 3
    deadline = time.time() + max_wait
    last_error = None
    elapsed = 0
    attempt = 0

    s = requests.Session()
    s.headers.update({"Accept": "application/json"})

    _log()
    _log("─" * 60, color="bold")
    _log("  Device Readiness Check", color="bold")
    _log("─" * 60, color="bold")
    _log(f"  Target  : {base_url}")
    _log(f"  Timeout : {max_wait}s  |  Interval : {interval}s")
    _log(f"  Polling : GET /api/wifi/status until connected=true")
    _log()

    while time.time() < deadline:
        attempt += 1
        try:
            r = s.get(f"{base_url}/api/wifi/status", timeout=5)
            r.raise_for_status()
            status = r.json()
            if status.get("connected"):
                ssid = status.get("ssid", "?")
                ip = status.get("ip", "?")
                rssi = status.get("rssi", "?")
                _log(f"  ✓ WiFi connected! "
                     f"SSID={ssid}, IP={ip}, RSSI={rssi} "
                     f"(attempt #{attempt}, {elapsed}s elapsed)",
                     color="green")
                _log()
                break
            else:
                configured = status.get("configured", False)
                ssid = status.get("ssid", "")
                _log(f"  ⏳ [{attempt}] Device reachable but WiFi not connected "
                     f"(configured={configured}, ssid={ssid}), retrying ...")
        except requests.RequestException as e:
            last_error = e
            remaining = int(deadline - time.time())
            _log(f"  ⏳ [{attempt}] Device not reachable ({type(e).__name__}), "
                 f"retrying ... ({remaining}s left)")
        time.sleep(interval)
        elapsed = int(time.time() - (deadline - max_wait))
    else:
        pytest.exit(
            f"❌  Device not ready after {max_wait}s at {base_url} "
            f"({attempt} attempts)"
            + (f" — last error: {last_error}" if last_error else "")
        )

    # ── Print device info ────────────────────────────────────────────
    _log("─" * 60, color="bold")
    _log("  Device Info", color="bold")
    _log("─" * 60, color="bold")
    _log(f"  Target : {base_url}")
    try:
        r = s.get(f"{base_url}/api/system/info", timeout=10)
        r.raise_for_status()
        info = r.json()
        _log(f"  Chip       : {info.get('chip', '?')}")
        _log(f"  CPU cores  : {info.get('cpu_cores', '?')}")
        _log(f"  CPU freq   : {info.get('cpu_freq', '?')} MHz")
        _log(f"  Flash      : {info.get('flash_size', '?')} MB")
        _log(f"  PSRAM      : {info.get('psram_size', '?')} MB")
        _log(f"  SDK        : {info.get('sdk_version', '?')}")
        wifi = "✓" if info.get("wifi_connected") else "✗"
        _log(f"  WiFi       : {wifi}  SSID={info.get('wifi_ssid', '(none)')}")
        sd = "✓" if info.get("sdcard_mounted") else "✗"
        _log(f"  SD card    : {sd}")
        _log(f"  Free heap  : {info.get('free_heap', '?')} KB")
        _log(f"  Uptime     : {info.get('uptime', '?')} s")
        sntp = "✓" if info.get("sntp_synced") else "✗"
        _log(f"  SNTP sync  : {sntp}")
        _log(f"  Timezone   : {info.get('timezone', '?')}")
    except requests.RequestException as e:
        _log(f"  ⚠️  Failed to fetch device info: {e}", color="red")
    _log()


# ── Fixtures ──────────────────────────────────────────────────────────


@pytest.fixture(scope="session")
def base_url(request) -> str:
    """Return the ESP32-S31 base URL.

    Precedence (highest first):
      1. --base-url CLI argument
      2. ESP_BASE_URL environment variable
      3. Built-in default (mDNS hostname)
    """
    return _resolve_base_url(request.config)


@pytest.fixture(scope="session")
def client() -> requests.Session:
    """A requests.Session pre-configured with JSON headers."""
    s = requests.Session()
    s.headers.update({"Accept": "application/json"})
    return s


@pytest.fixture
def api(client: requests.Session, base_url: str):
    """Namespace-like fixture with helper methods for common API calls.

    Usage:
        def test_something(api):
            resp = api.wifi_status()
            assert resp["connected"]
    """
    class _API:
        # ── WiFi ──

        @staticmethod
        def wifi_scan():
            r = client.get(f"{base_url}/api/wifi/scan", timeout=15)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def wifi_connect(ssid: str, password: str = ""):
            r = client.post(
                f"{base_url}/api/wifi/connect",
                json={"ssid": ssid, "password": password},
                timeout=60,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def wifi_status():
            r = client.get(f"{base_url}/api/wifi/status", timeout=10)
            r.raise_for_status()
            return r.json()

        # ── Audio Volume ──

        @staticmethod
        def volume_get():
            r = client.get(f"{base_url}/api/audio/volume", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def volume_set(volume: int, save: bool = True):
            url = f"{base_url}/api/audio/volume"
            if not save:
                url += "?save=false"
            r = client.post(url, json={"volume": volume}, timeout=5)
            r.raise_for_status()
            return r.json()

        # ── Audio Recording ──

        @staticmethod
        def record_start():
            r = client.get(f"{base_url}/api/audio/record_start", timeout=10)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def record_stop():
            r = client.get(f"{base_url}/api/audio/record_stop", timeout=10)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def record_status():
            r = client.get(f"{base_url}/api/audio/record_status", timeout=5)
            r.raise_for_status()
            return r.json()

        # ── Audio Playback ──

        @staticmethod
        def audio_list():
            r = client.get(f"{base_url}/api/audio/list", timeout=10)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def audio_play(file: str):
            r = client.get(
                f"{base_url}/api/audio/play",
                params={"file": file},
                timeout=30,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def play_status():
            r = client.get(f"{base_url}/api/audio/play_status", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def audio_stop():
            r = client.get(f"{base_url}/api/audio/stop", timeout=5)
            r.raise_for_status()
            return r.json()

        # ── Files ──

        @staticmethod
        def files_list(dir: str = "/"):
            r = client.get(
                f"{base_url}/api/files/list",
                params={"dir": dir},
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def files_download(path: str):
            r = client.get(
                f"{base_url}/api/files/download",
                params={"path": path},
                timeout=30,
            )
            return r  # return raw response (may be binary)

        @staticmethod
        def files_delete(path: str):
            r = client.post(
                f"{base_url}/api/files/delete",
                json={"path": path},
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        @staticmethod
        def files_delete_batch(paths: list):
            r = client.post(
                f"{base_url}/api/files/delete_batch",
                json={"paths": paths},
                timeout=10,
            )
            r.raise_for_status()
            return r.json()

        # ── ULog ──

        @staticmethod
        def ulog_status():
            r = client.get(f"{base_url}/api/ulog/status", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def ulog_start():
            r = client.post(f"{base_url}/api/ulog/start", timeout=10)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def ulog_stop():
            r = client.post(f"{base_url}/api/ulog/stop", timeout=5)
            r.raise_for_status()
            return r.json()

        # ── System ──

        @staticmethod
        def system_info():
            r = client.get(f"{base_url}/api/system/info", timeout=10)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def system_stats():
            r = client.get(f"{base_url}/api/system/stats", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def timezone_get():
            r = client.get(f"{base_url}/api/system/timezone", timeout=5)
            r.raise_for_status()
            return r.json()

        @staticmethod
        def timezone_set(timezone: str):
            r = client.post(
                f"{base_url}/api/system/timezone",
                json={"timezone": timezone},
                timeout=5,
            )
            r.raise_for_status()
            return r.json()

        # ── SD Card ──

        @staticmethod
        def sdcard_info():
            r = client.get(f"{base_url}/api/sdcard/info", timeout=5)
            r.raise_for_status()
            return r.json()

    return _API
