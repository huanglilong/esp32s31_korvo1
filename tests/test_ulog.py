"""Tests for ULog API endpoints — status, start, stop.

ULog is the PX4-compatible binary logging subsystem on the ESP32-S31.
"""

import time
import pytest


class TestUlogStatus:
    """GET /api/ulog/status — check logger state."""

    def test_ulog_status_returns_json(self, api):
        """Status endpoint returns a JSON object with expected fields."""
        status = api.ulog_status()
        assert isinstance(status, dict)
        assert "running" in status, f"Missing running field: {status}"
        assert isinstance(status["running"], bool)
        assert "filepath" in status, f"Missing filepath: {status}"
        assert "bytes_written" in status, f"Missing bytes_written: {status}"
        assert isinstance(status["bytes_written"], (int, float))


class TestUlogLifecycle:
    """ULog start → status → stop cycle."""

    @pytest.fixture(autouse=True)
    def _ensure_stopped(self, api):
        """Ensure ULog is stopped before each test."""
        try:
            api.ulog_stop()
        except Exception:
            pass
        time.sleep(0.5)
        yield

    @pytest.fixture(autouse=True)
    def _cleanup(self, api):
        """Stop ULog after test if it's still running."""
        yield
        try:
            status = api.ulog_status()
            if status.get("running"):
                api.ulog_stop()
        except Exception:
            pass

    def test_ulog_start(self, api):
        """POST /api/ulog/start returns {ok: 1} and sets running=true."""
        resp = api.ulog_start()
        assert resp.get("ok") in (1, True), f"Start failed: {resp}"
        time.sleep(1)
        status = api.ulog_status()
        assert status.get("running") is True, f"ULog not running: {status}"

    def test_ulog_writes_data(self, api):
        """After starting, bytes_written should increase over time."""
        api.ulog_start()
        time.sleep(2)
        status = api.ulog_status()
        assert status.get("bytes_written", 0) > 0, \
            f"No bytes written: {status}"
        assert status["filepath"], f"Filepath should not be empty: {status}"

    def test_ulog_stop(self, api):
        """POST /api/ulog/stop returns {ok: 1} and sets running=false."""
        api.ulog_start()
        time.sleep(1)
        resp = api.ulog_stop()
        assert resp.get("ok") in (1, True), f"Stop failed: {resp}"
        time.sleep(0.5)
        status = api.ulog_status()
        assert status.get("running") is False, f"ULog still running: {status}"

    def test_ulog_start_when_no_sdcard(self, api, client, base_url):
        """ULog start without SD card returns {ok: 0, error: ...}.

        This test only verifies the response format; actual behavior
        depends on whether an SD card is mounted.
        """
        # We can't unmount the SD card remotely, so just verify the
        # endpoint returns valid JSON with 'ok' field
        resp = api.ulog_start()
        assert "ok" in resp, f"Missing ok field: {resp}"
        # Clean up
        if resp.get("ok") in (1, True):
            api.ulog_stop()
