"""Integration test: verify ULog recording contains both Audio + Camera data.

This test runs LAST (after all other tests) because it:
  1. Stops ULog to get a complete .ulg file
  2. Downloads the .ulg file via the file manager API
  3. Parses it with pyulog to verify audio_frame + camera_frame topics exist
  4. Restarts ULog to resume normal operation

It verifies:
  - ULog .ulg file contains audio_frame topic with AAC data
  - ULog .ulg file contains camera_frame topic with JPEG data (if camera connected)
  - Both topics have multiple data entries (continuous recording)
"""

import os
import subprocess
import tempfile
import time

import pytest

try:
    from pyulog.core import ULog
    HAS_PYULOG = True
except ImportError:
    HAS_PYULOG = False


def _download_ulog_file(api, filepath: str, dest: str) -> bool:
    """Download a file from the device via /api/files/download."""
    r = api.files_download(filepath)
    if r.status_code != 200:
        return False
    content_type = r.headers.get("Content-Type", "")
    if "text/html" in content_type:
        return False
    with open(dest, "wb") as f:
        f.write(r.content)
    return os.path.getsize(dest) > 0


def _parse_ulog_topics(ulg_path: str) -> dict:
    """Parse ULog file and return {topic_name: row_count}."""
    ulog = ULog(ulg_path)
    topics = {}
    for data in ulog.data_list:
        ts = data.data.get("timestamp")
        count = len(ts) if ts is not None else 0
        topics[data.name] = count
    return topics


# ── Session-scoped: download .ulg once, share across all tests ──

_ulog_cache = {}  # {local_path: str | None}


def _get_ulog_file(api):
    """Download the ULog file once per session and cache the result.

    Returns the local file path, or None if download failed.
    """
    if _ulog_cache:
        return next(iter(_ulog_cache.keys())) if any(_ulog_cache.values()) else None

    status = api.ulog_status()
    was_running = status.get("running", False)
    ulg_remote_path = status.get("filepath", "")

    if not was_running or not ulg_remote_path:
        resp = api.ulog_start()
        if resp.get("ok") not in (1, True):
            _ulog_cache[None] = False
            return None
        time.sleep(5)
        status = api.ulog_status()
        ulg_remote_path = status.get("filepath", "")
        was_running = True

    if not ulg_remote_path:
        _ulog_cache[None] = False
        return None

    # Wait for data to accumulate
    time.sleep(5)

    # Stop ULog to flush and get a complete file
    api.ulog_stop()
    time.sleep(1)

    # Download
    tmpdir = tempfile.mkdtemp(prefix="ulog_test_")
    local_path = os.path.join(tmpdir, os.path.basename(ulg_remote_path))
    success = _download_ulog_file(api, ulg_remote_path, local_path)

    # Restart ULog
    if was_running:
        api.ulog_start()
        time.sleep(1)

    if not success:
        _ulog_cache[None] = False
        return None

    _ulog_cache[local_path] = True
    return local_path


class TestUlogAudioCamera:
    """Verify ULog recording file contains both Audio and Camera data.

    These tests download the actual .ulg file and parse it with pyulog
    to confirm audio_frame and camera_frame topics are present with data.
    """

    @pytest.fixture(autouse=True)
    def _ulog_path(self, api):
        """Function-scoped fixture: download .ulg once, share path with all tests."""
        if not HAS_PYULOG:
            pytest.skip("pyulog not installed (pip install pyulog)")

        path = _get_ulog_file(api)
        if path is None:
            pytest.skip("Could not download ULog file")
        yield path

    def test_ulog_file_has_audio_topic(self, _ulog_path):
        """ULog file must contain audio_frame topic with AAC data."""
        topics = _parse_ulog_topics(_ulog_path)
        assert "audio_frame" in topics, \
            f"audio_frame topic not found in ULog. Topics: {list(topics.keys())}"
        assert topics["audio_frame"] > 0, \
            f"audio_frame has 0 data entries. Topics: {topics}"

    def test_ulog_file_audio_has_multiple_frames(self, _ulog_path):
        """audio_frame should have many entries (continuous recording at ~15fps AAC)."""
        topics = _parse_ulog_topics(_ulog_path)
        if "audio_frame" not in topics:
            pytest.skip("audio_frame topic not in ULog file")
        # At ~15 AAC frames/sec, 5 seconds should yield ~75 frames
        frame_count = topics["audio_frame"]
        assert frame_count >= 10, \
            f"Too few audio frames: {frame_count} (expected >= 10 for 5s recording)"

    def test_ulog_file_has_camera_topic(self, _ulog_path, api):
        """ULog file must contain camera_frame topic with JPEG data (if camera connected)."""
        status = api.ulog_status()
        if not status.get("camera_streaming"):
            pytest.skip("Camera not streaming (no camera connected?)")
        topics = _parse_ulog_topics(_ulog_path)
        assert "camera_frame" in topics, \
            f"camera_frame topic not found in ULog. Topics: {list(topics.keys())}"
        assert topics["camera_frame"] > 0, \
            f"camera_frame has 0 data entries. Topics: {topics}"

    def test_ulog_file_camera_has_multiple_frames(self, _ulog_path, api):
        """camera_frame should have multiple entries (5fps, 5s = ~25 frames)."""
        status = api.ulog_status()
        if not status.get("camera_streaming"):
            pytest.skip("Camera not streaming")
        topics = _parse_ulog_topics(_ulog_path)
        if "camera_frame" not in topics:
            pytest.skip("camera_frame topic not in ULog file")
        frame_count = topics["camera_frame"]
        assert frame_count >= 5, \
            f"Too few camera frames: {frame_count} (expected >= 5 for 5s at 5fps)"

    def test_ulog_file_audio_frame_fields(self, _ulog_path):
        """audio_frame should contain expected fields: aac_data, aac_size, sample_rate, etc."""
        ulog = ULog(_ulog_path)
        audio_data = None
        for data in ulog.data_list:
            if data.name == "audio_frame":
                audio_data = data
                break
        if audio_data is None:
            pytest.skip("audio_frame not in ULog file")
        required_fields = ["timestamp", "frame_index", "aac_size", "sample_rate", "channel"]
        for field in required_fields:
            assert field in audio_data.data, \
                f"audio_frame missing field '{field}'. Fields: {list(audio_data.data.keys())}"
        aac_sizes = audio_data.data.get("aac_size")
        if aac_sizes is not None and len(aac_sizes) > 0:
            assert aac_sizes[0] > 0, f"aac_size should be > 0, got {aac_sizes[0]}"

    def test_ulog_file_camera_frame_fields(self, _ulog_path, api):
        """camera_frame should contain expected fields: jpeg_data, jpeg_size, width, height, etc."""
        status = api.ulog_status()
        if not status.get("camera_streaming"):
            pytest.skip("Camera not streaming")
        ulog = ULog(_ulog_path)
        cam_data = None
        for data in ulog.data_list:
            if data.name == "camera_frame":
                cam_data = data
                break
        if cam_data is None:
            pytest.skip("camera_frame not in ULog file")
        required_fields = ["timestamp", "frame_index", "jpeg_size", "width", "height"]
        for field in required_fields:
            assert field in cam_data.data, \
                f"camera_frame missing field '{field}'. Fields: {list(cam_data.data.keys())}"
        jpeg_sizes = cam_data.data.get("jpeg_size")
        if jpeg_sizes is not None and len(jpeg_sizes) > 0:
            assert jpeg_sizes[0] > 0, f"jpeg_size should be > 0, got {jpeg_sizes[0]}"
        widths = cam_data.data.get("width")
        heights = cam_data.data.get("height")
        if widths is not None and len(widths) > 0:
            assert widths[0] == 320, f"Expected width=320, got {widths[0]}"
            assert heights[0] == 240, f"Expected height=240, got {heights[0]}"

    def test_ulog_info_tool_output(self, _ulog_path):
        """ulog_info CLI tool should report both audio_frame and camera_frame topics."""
        result = subprocess.run(
            ["ulog_info", _ulog_path],
            capture_output=True, text=True, timeout=10
        )
        assert result.returncode == 0, f"ulog_info failed: {result.stderr}"
        output = result.stdout
        assert len(output) > 0, "ulog_info produced no output"
        topics = _parse_ulog_topics(_ulog_path)
        if "audio_frame" in topics:
            assert "audio_frame" in output, "ulog_info output doesn't mention audio_frame"
        if "camera_frame" in topics:
            assert "camera_frame" in output, "ulog_info output doesn't mention camera_frame"
