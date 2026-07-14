"""Tests for system API endpoints — info, stats, timezone, sdcard."""

import pytest


class TestSystemInfo:
    """GET /api/system/info — device and system information."""

    def test_system_info_returns_json(self, api):
        """Returns a JSON object with expected fields."""
        info = api.system_info()
        assert isinstance(info, dict)

    def test_chip_info(self, api):
        """Response includes chip identification fields."""
        info = api.system_info()
        assert info.get("chip") == "ESP32-S31", f"Unexpected chip: {info}"
        assert "cpu_cores" in info, f"Missing cpu_cores: {info}"
        assert info["cpu_cores"] > 0
        assert "cpu_freq" in info, f"Missing cpu_freq: {info}"

    def test_memory_info(self, api):
        """Response includes memory fields."""
        info = api.system_info()
        assert "flash_size" in info, f"Missing flash_size: {info}"
        assert "psram_size" in info, f"Missing psram_size: {info}"
        assert "free_heap" in info, f"Missing free_heap: {info}"
        assert info["free_heap"] >= 0

    def test_wifi_fields(self, api):
        """Response includes WiFi status fields."""
        info = api.system_info()
        assert "wifi_connected" in info, f"Missing wifi_connected: {info}"
        assert isinstance(info["wifi_connected"], bool)
        assert "wifi_ssid" in info, f"Missing wifi_ssid: {info}"
        assert "wifi_ip" in info, f"Missing wifi_ip: {info}"

    def test_sdcard_field(self, api):
        """Response includes SD card mount status."""
        info = api.system_info()
        assert "sdcard_mounted" in info, f"Missing sdcard_mounted: {info}"
        assert isinstance(info["sdcard_mounted"], bool)

    def test_uptime_field(self, api):
        """Response includes uptime in seconds."""
        info = api.system_info()
        assert "uptime" in info, f"Missing uptime: {info}"
        assert info["uptime"] >= 0

    def test_sdk_version(self, api):
        """Response includes SDK version string."""
        info = api.system_info()
        assert "sdk_version" in info, f"Missing sdk_version: {info}"
        assert isinstance(info["sdk_version"], str)
        assert len(info["sdk_version"]) > 0

    def test_sntp_sync_field(self, api):
        """Response includes SNTP sync status."""
        info = api.system_info()
        assert "sntp_synced" in info, f"Missing sntp_synced: {info}"
        assert isinstance(info["sntp_synced"], bool)

    def test_timezone_field(self, api):
        """Response includes timezone string."""
        info = api.system_info()
        assert "timezone" in info, f"Missing timezone: {info}"
        assert isinstance(info["timezone"], str)


class TestSystemStats:
    """GET /api/system/stats — system performance snapshot."""

    def test_stats_returns_json(self, api):
        """Returns a JSON object with CPU and memory fields."""
        stats = api.system_stats()
        assert isinstance(stats, dict)

    def test_cpu_fields(self, api):
        """CPU fields are present and within reasonable range.

        CPU% values use 0-10000 scale (0-100.00%), per system_stats.msg.
        """
        stats = api.system_stats()
        for key in ("cpu0_pct", "cpu1_pct", "total_cpu_pct"):
            assert key in stats, f"Missing {key}: {stats}"
            assert 0 <= stats[key] <= 10000, \
                f"{key} out of range [0, 10000]: {stats[key]}"

    def test_memory_fields(self, api):
        """Memory fields are present and non-negative."""
        stats = api.system_stats()
        for key in ("free_internal_kb", "free_psram_kb",
                     "min_free_internal_kb", "min_free_psram_kb"):
            assert key in stats, f"Missing {key}: {stats}"
            assert stats[key] >= 0, f"{key} is negative: {stats[key]}"


class TestTimezone:
    """GET/POST /api/system/timezone — timezone management."""

    def test_timezone_get_returns_json(self, api):
        """GET returns timezone and sntp_synced fields."""
        data = api.timezone_get()
        assert isinstance(data, dict)
        assert "timezone" in data, f"Missing timezone: {data}"
        assert "sntp_synced" in data, f"Missing sntp_synced: {data}"
        assert isinstance(data["sntp_synced"], bool)

    def test_timezone_set_and_get(self, api):
        """POST sets timezone, GET reflects the change."""
        # Save original
        original = api.timezone_get()["timezone"]
        # Set new timezone
        r = api.timezone_set("UTC0")
        assert r.get("timezone") == "UTC0", f"Timezone mismatch: {r}"
        # Verify via GET
        data = api.timezone_get()
        assert data["timezone"] == "UTC0", \
            f"Timezone not updated: {data}"
        # Restore original
        api.timezone_set(original)

    def test_timezone_set_cst(self, api):
        """Set timezone to CST-8 (China Standard Time)."""
        original = api.timezone_get()["timezone"]
        r = api.timezone_set("CST-8")
        assert r.get("timezone") == "CST-8"
        data = api.timezone_get()
        assert data["timezone"] == "CST-8"
        # Restore
        api.timezone_set(original)

    def test_timezone_current_time_when_synced(self, api):
        """If SNTP synced, response includes current_time."""
        data = api.timezone_get()
        if data.get("sntp_synced"):
            assert "current_time" in data, \
                f"Missing current_time when synced: {data}"


class TestSdCardInfo:
    """GET /api/sdcard/info — SD card information."""

    def test_sdcard_info_returns_json(self, api):
        """Returns a JSON object with mount info."""
        info = api.sdcard_info()
        assert isinstance(info, dict)
        assert "mounted" in info, f"Missing mounted: {info}"
        assert isinstance(info["mounted"], bool)
        assert "mountpoint" in info, f"Missing mountpoint: {info}"

    def test_sdcard_mountpoint(self, api):
        """Mount point is /sdcard."""
        info = api.sdcard_info()
        if info.get("mounted"):
            assert info["mountpoint"] == "/sdcard", \
                f"Unexpected mountpoint: {info['mountpoint']}"
