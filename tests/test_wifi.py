"""Tests for WiFi API endpoints — scan, connect, status."""

import time
import pytest


class TestWifiScan:
    """GET /api/wifi/scan — WiFi network scanning."""

    def test_scan_returns_json(self, api):
        """Scan endpoint returns a JSON object with networks array."""
        data = api.wifi_scan()
        assert isinstance(data, dict)
        # Either 'networks' on success or 'error' on failure
        assert "networks" in data or "error" in data, \
            f"Expected networks or error, got: {data}"

    def test_scan_networks_is_list(self, api):
        """On success, networks field is a list."""
        data = api.wifi_scan()
        if "networks" in data:
            assert isinstance(data["networks"], list)

    def test_scan_network_entry_fields(self, api):
        """Each network entry has ssid, rssi, authmode."""
        data = api.wifi_scan()
        if "networks" not in data or not data["networks"]:
            pytest.skip("No networks found in scan")
        for entry in data["networks"]:
            assert "ssid" in entry, f"Missing ssid: {entry}"
            assert "rssi" in entry, f"Missing rssi: {entry}"
            assert "authmode" in entry, f"Missing authmode: {entry}"

    def test_scan_count_matches(self, api):
        """Count field matches networks array length."""
        data = api.wifi_scan()
        if "networks" in data and "count" in data:
            assert data["count"] == len(data["networks"]), \
                f"Count mismatch: {data['count']} != {len(data['networks'])}"


class TestWifiStatus:
    """GET /api/wifi/status — WiFi connection status."""

    def test_status_returns_json(self, api):
        """Status endpoint returns JSON with expected fields."""
        status = api.wifi_status()
        assert isinstance(status, dict)

    def test_status_has_connection_fields(self, api):
        """Response includes connected, configured, ssid, ip, rssi."""
        status = api.wifi_status()
        assert "connected" in status, f"Missing connected: {status}"
        assert isinstance(status["connected"], bool)
        assert "configured" in status, f"Missing configured: {status}"
        assert isinstance(status["configured"], bool)
        assert "ssid" in status, f"Missing ssid: {status}"
        assert "ip" in status, f"Missing ip: {status}"
        assert "rssi" in status, f"Missing rssi: {status}"


class TestWifiConnect:
    """POST /api/wifi/connect — WiFi connection management."""

    def test_connect_empty_ssid_returns_error(self, api):
        """Connecting with empty SSID returns error."""
        r = api.wifi_connect("")
        assert "error" in r, f"Expected error for empty SSID, got: {r}"

    def test_connect_bad_credentials(self, api, client, base_url):
        """Connecting to nonexistent AP should not crash the device.

        The firmware has a connect timeout; the test uses a long timeout
        because WiFi operations block the HTTP server.
        """
        import requests as req_mod
        try:
            r = client.post(
                f"{base_url}/api/wifi/connect",
                json={"ssid": "test_integration_nonexistent_ssid",
                      "password": "wrong_password"},
                timeout=60,
            )
            r.raise_for_status()
            j = r.json()
            # Should report connection failure
            assert not j.get("connected", False), \
                "Bad credentials should not be accepted"
        except req_mod.ReadTimeout:
            pytest.xfail("WiFi connect timeout blocks httpd task")
