"""Tests for audio API endpoints — volume, record, play, list, stop.

Strategy (no firmware modification):
  - Use fixed duration sleep + explicit stop for all operations.
  - Poll record_status to observe recording progress.
  - Play for a fixed time, then stop.
"""

import time
import pytest


RECORD_SECONDS = 5
PLAY_SECONDS = 5


@pytest.fixture(autouse=True)
def _ensure_clean_start(api):
    """Before each test: stop any leftover playback/recording."""
    try:
        api.audio_stop()
    except Exception:
        pass
    try:
        st = api.record_status()
        if st.get("recording") == 1:
            api.record_stop()
    except Exception:
        pass
    yield


class TestAudioVolume:
    """GET/POST /api/audio/volume — volume control."""

    def test_volume_get_returns_number(self, api):
        """GET returns {volume: N} where N is 0-100."""
        data = api.volume_get()
        assert "volume" in data, f"Missing volume: {data}"
        assert isinstance(data["volume"], (int, float))

    def test_volume_set_middle(self, api):
        """POST sets volume to 50."""
        r = api.volume_set(50)
        assert r.get("volume") == 50, f"Volume mismatch: {r}"

    def test_volume_set_edge_low(self, api):
        """Volume 0 is accepted."""
        r = api.volume_set(0)
        assert r.get("volume") == 0, f"Volume should be 0: {r}"

    def test_volume_set_edge_high(self, api):
        """Volume 100 is accepted."""
        r = api.volume_set(100)
        assert r.get("volume") == 100, f"Volume should be 100: {r}"

    def test_volume_get_reflects_set(self, api):
        """GET returns the value set by POST."""
        api.volume_set(42)
        data = api.volume_get()
        assert data["volume"] == 42, f"Volume mismatch: {data}"

    def test_volume_restore(self, api):
        """Restore volume to a reasonable default after tests."""
        api.volume_set(60)


class TestAudioRecord:
    """Recording lifecycle: start → poll record_status → stop."""

    @pytest.fixture(autouse=True)
    def _stop_recording_after_test(self, api):
        """Ensure recording is stopped after each test, even on failure."""
        yield
        try:
            api.record_stop()
        except Exception:
            pass

    def test_record_start_returns_ok(self, api):
        """GET /api/audio/record_start returns {ok: 1}."""
        r = api.record_start()
        assert r.get("ok") in (1, True), f"Expected ok, got: {r}"
        api.record_stop()

    def test_record_status_while_active(self, api):
        """record_status shows recording=1, seconds and bytes growing."""
        api.record_start()
        time.sleep(1)
        status = api.record_status()
        assert status.get("recording") == 1, f"Not recording: {status}"
        assert "seconds" in status, f"Missing seconds: {status}"
        assert "bytes" in status, f"Missing bytes: {status}"
        assert status["seconds"] >= 0
        api.record_stop()

    def test_record_stop_returns_file_info(self, api):
        """record_stop returns {ok: 1, file: '/sdcard/...', bytes: N} with N > 0."""
        api.record_start()
        time.sleep(RECORD_SECONDS)
        # Poll status mid-recording
        mid = api.record_status()
        assert mid["recording"] == 1
        assert mid["seconds"] >= RECORD_SECONDS - 1
        assert mid["bytes"] > 0
        # Stop
        r = api.record_stop()
        assert r.get("ok") in (1, True), f"Stop failed: {r}"
        assert "file" in r, f"Missing file: {r}"
        assert r["file"].startswith("/sdcard/"), f"Bad path: {r['file']}"
        assert r.get("bytes", 0) > 0, f"Zero bytes: {r}"

    def test_record_idempotent_start(self, api):
        """Double record_start returns ok without error."""
        api.record_start()
        r2 = api.record_start()
        assert r2.get("ok") in (1, True)
        api.record_stop()

    def test_record_idempotent_stop(self, api):
        """Calling record_stop when already stopped returns ok."""
        api.record_stop()
        r = api.record_stop()
        assert r.get("ok") in (1, True)

    def test_record_status_when_idle(self, api):
        """record_status returns {recording: 0} when idle."""
        api.record_stop()
        st = api.record_status()
        assert st.get("recording") == 0

    def test_record_status_has_file_field(self, api):
        """record_status includes file path while recording."""
        api.record_start()
        time.sleep(1)
        status = api.record_status()
        assert "file" in status, f"Missing file: {status}"
        assert status["file"].startswith("/sdcard/"), \
            f"File path should start with /sdcard/: {status['file']}"
        api.record_stop()


class TestAudioList:
    """Audio file listing via GET /api/audio/list."""

    def test_audio_list_returns_files_array(self, api):
        """audio/list returns {files: [...]}."""
        data = api.audio_list()
        assert isinstance(data, dict)
        assert "files" in data
        assert isinstance(data["files"], list)

    def test_audio_list_has_recording_after_record(self, api):
        """After recording, list contains audio file entry."""
        api.record_start()
        time.sleep(RECORD_SECONDS)
        api.record_stop()
        time.sleep(0.5)
        data = api.audio_list()
        audio = [f for f in data["files"] if f.lower().endswith((".aac", ".wav", ".mp3"))]
        assert len(audio) > 0, f"No audio files in list: {data['files']}"
        aac = [f for f in data["files"] if f.lower().endswith(".aac")]
        assert len(aac) > 0, f"No .aac in list: {data['files']}"
        pytest.latest_recording = aac[-1]


class TestAudioPlay:
    """Playback: start → fixed duration → stop."""

    @pytest.fixture(autouse=True)
    def _ensure_recording(self, api):
        """Fixture: create a short recording for play tests if none exists."""
        try:
            data = api.audio_list()
        except Exception:
            data = {"files": []}
        audio = [f for f in data["files"] if f.lower().endswith((".wav", ".mp3", ".aac"))]
        if not audio:
            api.record_start()
            time.sleep(RECORD_SECONDS)
            api.record_stop()
            time.sleep(0.5)
            data = api.audio_list()
            audio = [f for f in data["files"] if f.lower().endswith((".wav", ".mp3", ".aac"))]
        pytest.latest_recording = audio[-1] if audio else None

    def test_play_returns_ok(self, api):
        """play with valid file returns {ok: 1}."""
        if not pytest.latest_recording:
            pytest.skip("No recording available")
        r = api.audio_play(pytest.latest_recording)
        assert r.get("ok") in (1, True), f"Play failed: {r}"
        time.sleep(PLAY_SECONDS)
        api.audio_stop()

    def test_play_and_stop(self, api):
        """Play, then stop cleanly."""
        if not pytest.latest_recording:
            pytest.skip("No recording available")
        r = api.audio_play(pytest.latest_recording)
        assert r.get("ok") in (1, True)
        time.sleep(1)
        # Verify playing state is true
        status = api.play_status()
        assert status.get("playing") is True, f"Expected playing=true: {status}"
        r = api.audio_stop()
        assert r.get("ok") in (1, True)
        time.sleep(0.5)
        # Verify playing state is false after stop
        status = api.play_status()
        assert status.get("playing") is False, f"Expected playing=false after stop: {status}"

    def test_play_status_when_idle(self, api):
        """play_status returns {playing: false} when idle."""
        api.audio_stop()  # Ensure stopped
        status = api.play_status()
        assert "playing" in status, f"Missing playing field: {status}"
        assert status["playing"] is False, f"Expected playing=false: {status}"

    def test_play_without_file_returns_ok_0(self, api, client, base_url):
        """play without ?file= param returns {ok: 0}."""
        resp = client.get(f"{base_url}/api/audio/play", timeout=10)
        resp.raise_for_status()
        j = resp.json()
        assert j.get("ok") == 0

    def test_audio_stop_returns_ok(self, api):
        """audio/stop returns {ok: 1}."""
        r = api.audio_stop()
        assert r.get("ok") in (1, True)

    def test_play_nonexistent_file(self, api):
        """play with nonexistent file returns valid JSON with ok key.

        The firmware's esp_audio_simple_player_run is asynchronous;
        it may return ok:0 or ok:1 (failure is async via callback).
        """
        r = api.audio_play("nonexistent_abc123.aac")
        assert "ok" in r, f"Response missing 'ok' key: {r}"
        # Clean up: stop any leftover player handle
        time.sleep(1)
        api.audio_stop()


class TestAudioCleanup:
    """Final cleanup — stop any active operation."""

    def test_stop_all(self, api):
        """Ensure playback and recording are both stopped."""
        api.audio_stop()
        try:
            st = api.record_status()
            if st.get("recording") == 1:
                api.record_stop()
        except Exception:
            pass
