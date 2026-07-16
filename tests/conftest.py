"""Shared fixtures and helpers for ESP32-S31 Korvo-1 web config integration tests."""

import os
import pytest
import requests


def pytest_addoption(parser):
    parser.addoption(
        "--base-url",
        default=None,
        help="ESP32-S31 base URL (e.g., http://10.0.0.42:8080). "
             "Overrides ESP_BASE_URL env var and default.",
    )


@pytest.fixture(scope="session")
def base_url(request) -> str:
    """Return the ESP32-S31 base URL.

    Precedence (highest first):
      1. --base-url CLI argument
      2. ESP_BASE_URL environment variable
      3. Built-in default (mDNS hostname)
    """
    cli = request.config.getoption("--base-url")
    if cli:
        return cli
    env = os.environ.get("ESP_BASE_URL")
    if env:
        return env
    return "http://esp-web.local:8080"


@pytest.fixture(scope="session")
def client() -> requests.Session:
    """A requests.Session pre-configured with JSON headers."""
    s = requests.Session()
    s.headers.update({"Accept": "application/json"})
    return s


@pytest.fixture(scope="session", autouse=True)
def device_info(client: requests.Session, base_url: str):
    """Fetch and print target device info at the start of the test run."""
    print(f"\n{'='*60}")
    print(f"Target device: {base_url}")
    try:
        r = client.get(f"{base_url}/api/system/info", timeout=10)
        r.raise_for_status()
        info = r.json()
        print(f"  Chip       : {info.get('chip', '?')}")
        print(f"  CPU cores  : {info.get('cpu_cores', '?')}")
        print(f"  CPU freq   : {info.get('cpu_freq', '?')} MHz")
        print(f"  Flash      : {info.get('flash_size', '?')} MB")
        print(f"  PSRAM      : {info.get('psram_size', '?')} MB")
        print(f"  SDK        : {info.get('sdk_version', '?')}")
        wifi = "✓" if info.get("wifi_connected") else "✗"
        print(f"  WiFi       : {wifi}  SSID={info.get('wifi_ssid', '(none)')}")
        sd = "✓" if info.get("sdcard_mounted") else "✗"
        print(f"  SD card    : {sd}")
        print(f"  Free heap  : {info.get('free_heap', '?')} KB")
        print(f"  Uptime     : {info.get('uptime', '?')} s")
        sntp = "✓" if info.get("sntp_synced") else "✗"
        print(f"  SNTP sync  : {sntp}")
        print(f"  Timezone   : {info.get('timezone', '?')}")
    except requests.RequestException as e:
        print(f"  ⚠️  Failed to fetch device info: {e}")
    print(f"{'='*60}\n")
    yield


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
