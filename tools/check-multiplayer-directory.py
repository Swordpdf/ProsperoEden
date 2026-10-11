#!/usr/bin/env python3
"""Bounded directory parsing and real HTTP option regression checks; no public room joins."""
import http.server
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import threading

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("check_remote", root / "tools/check-remote.py")
remote = importlib.util.module_from_spec(spec)
spec.loader.exec_module(remote)
includes = ["-I" + str(root / "headless")]
if folder := remote.find_json():
    includes += ["-I" + str(folder)]

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(self.headers.get("api-version", "missing").encode())
    def log_message(self, *args):
        pass

with tempfile.TemporaryDirectory(prefix="directory-check-") as temp:
    http_object = str(Path(temp) / "http.o")
    binary = str(Path(temp) / "check")
    flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Werror"]
    subprocess.run(["cc", *flags, "-DREMOTE_HTTP_HOST=1", "-c", str(root / "headless/remote/http.c"), "-o", http_object], check=True)
    subprocess.run(["c++", "-std=c++20", *flags, *includes,
                    str(root / "headless/multiplayer_directory.cpp"),
                    str(root / "headless/multiplayer_directory_check.cpp"), http_object,
                    "-lcurl", "-pthread", "-o", binary], check=True)
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            subprocess.run([binary, f"http://127.0.0.1:{server.server_port}/"], check=True)
        finally:
            server.shutdown()
            worker.join()
