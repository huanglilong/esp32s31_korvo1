"""Tests for file manager API endpoints — list, download, delete, delete_batch.

These tests require an SD card to be mounted on the ESP32-S31.
"""

import pytest


class TestFilesList:
    """File listing via GET /api/files/list."""

    def test_files_list_root(self, api):
        """GET /api/files/list?dir=/ returns {ok: 1, files: [...], current, total_kb, free_kb}."""
        data = api.files_list("/")
        assert isinstance(data, dict)
        assert data.get("ok") == 1, f"Expected ok:1, got: {data}"
        assert "files" in data, f"Missing files: {data}"
        assert isinstance(data["files"], list)
        assert "current" in data, f"Missing current: {data}"
        assert "total_kb" in data, f"Missing total_kb: {data}"
        assert "free_kb" in data, f"Missing free_kb: {data}"

    def test_files_list_items_have_expected_keys(self, api):
        """Each file entry has name, is_dir, size, mtime."""
        data = api.files_list("/")
        for entry in data["files"]:
            assert "name" in entry, f"Missing name: {entry}"
            assert "is_dir" in entry, f"Missing is_dir: {entry}"
            assert isinstance(entry["is_dir"], bool)
            assert "size" in entry, f"Missing size: {entry}"
            assert "mtime" in entry, f"Missing mtime: {entry}"

    def test_files_list_invalid_path(self, api):
        """Querying a path outside /sdcard returns {ok: 0}."""
        data = api.files_list("/etc")
        assert data.get("ok") == 0, f"Expected ok:0, got: {data}"

    def test_files_list_path_traversal(self, api):
        """Path with '..' is rejected."""
        data = api.files_list("/../etc")
        assert data.get("ok") == 0, f"Expected ok:0 for path traversal, got: {data}"

    def test_files_list_subdirectory(self, api):
        """Listing a subdirectory works or returns error if not found."""
        data = api.files_list("/")
        dirs = [f for f in data["files"] if f["is_dir"]]
        if dirs:
            sub = api.files_list(f"/{dirs[0]['name']}")
            assert "files" in sub
            assert isinstance(sub["files"], list)


class TestFilesDownload:
    """File download via GET /api/files/download."""

    def test_download_existing_file(self, api):
        """Download an existing file and verify content."""
        listing = api.files_list("/")
        # Find a non-directory file to download
        files = [f for f in listing["files"]
                 if not f["is_dir"] and f["size"] > 0]
        if not files:
            pytest.skip("No files found on device to download")
        target = files[0]
        filepath = f"/sdcard/{target['name']}"
        resp = api.files_download(filepath)
        assert resp.status_code == 200
        assert len(resp.content) > 0, "Downloaded file is empty"

    def test_download_nonexistent_file(self, api):
        """Downloading a non-existent file returns error text."""
        resp = api.files_download("/sdcard/nonexistent_file_xyz.mp3")
        assert resp.status_code == 200
        text = resp.text
        assert "Not a file" in text or "Cannot open file" in text.lower() \
            or "not" in text.lower(), \
            f"Expected error message, got: {text}"


class TestFilesDelete:
    """File delete via POST /api/files/delete."""

    def test_delete_nonexistent_file(self, api):
        """Deleting a non-existent file returns {ok: 0, error: 'File not found'}."""
        r = api.files_delete("/sdcard/nonexistent_delete_test.mp3")
        assert r.get("ok") == 0, f"Expected ok:0, got: {r}"
        assert "error" in r, f"Missing error: {r}"

    def test_delete_invalid_path(self, api):
        """Deleting a path outside /sdcard returns {ok: 0}."""
        r = api.files_delete("/etc/passwd")
        assert r.get("ok") == 0, f"Expected ok:0, got: {r}"


class TestFilesDeleteBatch:
    """Batch delete via POST /api/files/delete_batch."""

    def test_delete_batch_nonexistent(self, api):
        """delete_batch with nonexistent paths returns {ok: 1, deleted: 0, failed: N}."""
        r = api.files_delete_batch([
            "/sdcard/nonexistent_a.mp3",
            "/sdcard/nonexistent_b.mp3",
        ])
        assert r.get("ok") == 1, f"Expected ok:1, got: {r}"
        assert "deleted" in r, f"Missing deleted: {r}"
        assert "failed" in r, f"Missing failed: {r}"
        assert r["failed"] > 0, f"Expected failures: {r}"

    def test_delete_batch_invalid_paths(self, api):
        """delete_batch with paths outside /sdcard reports failures."""
        r = api.files_delete_batch(["/etc/passwd", "/tmp/test"])
        assert r.get("ok") == 1, f"Expected ok:1, got: {r}"
        assert r["failed"] > 0
