#!/usr/bin/env python3
"""Tests of downloads.fetch against a server on the loopback: an address that sends another file is tried again and then left for the
next one, a missing file is left at once, a copy in place is kept or refused by its checksum, and nothing partial stays behind."""

import hashlib
import http.server
from pathlib import Path
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import downloads

GOOD = b"the pinned file\n" * 64
CHECKSUM = hashlib.sha256(GOOD).hexdigest()


class Files(http.server.BaseHTTPRequestHandler):
    asked = []

    def do_GET(self):
        Files.asked.append(self.path)
        if self.path == "/missing":
            self.send_error(404)
            return
        if self.path == "/broken":
            self.send_error(502)
            return
        body = GOOD if self.path == "/good" else b"<html>a page in place of the file</html>"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *arguments):
        pass


class Fetch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Files)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"
        cls.sleep = downloads.time.sleep
        downloads.time.sleep = lambda seconds: None

    @classmethod
    def tearDownClass(cls):
        downloads.time.sleep = cls.sleep
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        Files.asked.clear()
        self.folder = Path(tempfile.mkdtemp())
        self.archive = self.folder / "file.tar.gz"

    def tearDown(self):
        for path in self.folder.iterdir():
            path.unlink()
        self.folder.rmdir()

    def test_a_wrong_copy_is_tried_again_then_the_next_address(self):
        downloads.fetch([self.base + "/page", self.base + "/good"], self.archive, CHECKSUM, "file")
        self.assertEqual(self.archive.read_bytes(), GOOD)
        self.assertEqual(Files.asked, ["/page"] * downloads.ATTEMPTS + ["/good"])
        self.assertEqual(list(self.folder.iterdir()), [self.archive])

    def test_a_missing_file_leaves_its_address_at_once(self):
        downloads.fetch([self.base + "/missing", self.base + "/good"], self.archive, CHECKSUM, "file")
        self.assertEqual(Files.asked, ["/missing", "/good"])

    def test_a_server_error_is_tried_again(self):
        with self.assertRaisesRegex(RuntimeError, "no address sent file"):
            downloads.fetch(self.base + "/broken", self.archive, CHECKSUM, "file")
        self.assertEqual(Files.asked, ["/broken"] * downloads.ATTEMPTS)
        self.assertEqual(list(self.folder.iterdir()), [])

    def test_a_file_past_its_size_is_refused(self):
        with self.assertRaises(RuntimeError):
            downloads.fetch(self.base + "/good", self.archive, CHECKSUM, "file", size=len(GOOD) - 1)
        self.assertFalse(self.archive.exists())

    def test_a_copy_in_place_is_kept_or_refused(self):
        self.archive.write_bytes(GOOD)
        downloads.fetch(self.base + "/page", self.archive, CHECKSUM, "file")
        self.assertEqual(Files.asked, [])
        self.archive.write_bytes(b"another version")
        with self.assertRaisesRegex(RuntimeError, "delete it to download it again"):
            downloads.fetch(self.base + "/good", self.archive, CHECKSUM, "file")
        downloads.fetch(self.base + "/good", self.archive, CHECKSUM, "file", replace=True)
        self.assertEqual(self.archive.read_bytes(), GOOD)

    def test_another_digest(self):
        downloads.fetch(self.base + "/good", self.archive, hashlib.sha3_256(GOOD).hexdigest(), "file", algorithm="sha3_256")
        self.assertEqual(self.archive.read_bytes(), GOOD)


if __name__ == "__main__":
    unittest.main()
