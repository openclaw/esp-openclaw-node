#!/usr/bin/env python3
"""Exercise Talk through the real ESP-IDF native HTTP client on loopback."""

import argparse
import http.server
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading


def verify_network(binary):
    records = []

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def do_GET(self):
            self.respond()

        def do_POST(self):
            self.respond()

        def respond(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            records.append((self.path, self.command,
                            self.headers.get("Authorization") == "Bearer synthetic-broker-token",
                            body.startswith(b"v=0")))
            if self.path.startswith("/redirect/"):
                _, _, code, destination = self.path.split("/")
                port = hop.server_port if destination == "cross" else offer.server_port
                self.send_response(int(code))
                self.send_header("Location", f"http://127.0.0.1:{port}/answer")
                self.send_header("Content-Length", "0")
                self.end_headers()
            else:
                response = b"v=0\r\no=synthetic-answer 0 0 IN IP4 127.0.0.1\r\n"
                self.send_response(201)
                self.send_header("Content-Type", "application/sdp")
                self.send_header("Content-Length", str(len(response)))
                self.end_headers()
                self.wfile.write(response)

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as offer, \
         http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as hop:
        threads = [threading.Thread(target=server.serve_forever, daemon=True)
                   for server in (offer, hop)]
        for thread in threads:
            thread.start()
        try:
            cases = ["/offer"] + [
                f"/redirect/{code}/{destination}" for code in (301, 302, 303, 307, 308)
                for destination in ("cross", "same")
            ] + ["/offer"]
            for path in cases:
                records.clear()
                result = subprocess.run(
                    [str(binary)], capture_output=True, text=True, timeout=20,
                    env={**os.environ, "PROOF_ORIGIN": f"http://127.0.0.1:{offer.server_port}",
                         "PROOF_PATH": path},
                )
                match = re.search(r"PROOF_RESULT result=(-?\d+) answers=(\d+)", result.stdout)
                if result.returncode != 0 or match is None:
                    raise RuntimeError(f"Native client failed: {result.stdout}\n{result.stderr}")
                code, answers = map(int, match.groups())
                hops = [record for record in records if record[0] == "/answer"]
                print(f"{path}: result={code} answers={answers} requests={len(records)} "
                      f"redirected_requests={len(hops)} authorized_hops={sum(r[2] for r in hops)}",
                      flush=True)
                if not records or records[0][1:] != ("POST", True, True):
                    raise AssertionError(f"Missing authenticated SDP POST: {records}")
                if path.startswith("/redirect/"):
                    if code == 0 or answers != 0 or len(records) != 1:
                        raise AssertionError("Redirect must fail without a second request or SDP answer")
                elif code != 0 or answers != 1 or len(records) != 1:
                    raise AssertionError("Direct SDP exchange must succeed exactly once")
        finally:
            for server in (offer, hop):
                server.shutdown()
            for thread in threads:
                thread.join()
    print("12 real ESP-IDF HTTP exchanges passed")


def main():
    tests = Path(__file__).resolve().parent
    repo = tests.parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", type=Path, default=repo / "components/esp-openclaw-node/test_apps/esp_openclaw_node_unity_tests/managed_components/espressif__cjson/cJSON")
    parser.add_argument("--webrtc-dir", type=Path, default=repo / "third_party/esp-webrtc-solution")
    parser.add_argument("--talk-source", type=Path, default=tests.parent / "src/esp_openclaw_talk.c",
                        help="Production source override for baseline regression reproduction")
    parser.add_argument("--build-dir", type=Path, help="Retain a native build outside the repository")
    args = parser.parse_args()
    if "IDF_PATH" not in os.environ:
        parser.error("Activate an ESP-IDF installation with export.sh first")
    idf = Path(os.environ["IDF_PATH"])
    if not (args.cjson_dir / "cJSON.c").is_file():
        parser.error("Provide configured cJSON sources with --cjson-dir")
    with tempfile.TemporaryDirectory(prefix="talk-http-host-") as temporary:
        build = (args.build_dir or Path(temporary) / "build").resolve()
        build.mkdir(parents=True, exist_ok=True)
        subprocess.run([
            sys.executable, str(idf / "tools/idf.py"), "--preview",
            "-C", str(tests / "http_host"), "-B", str(build), "-DIDF_TARGET=linux",
            f"-DSDKCONFIG={build / 'sdkconfig'}", f"-DCJSON_DIR={args.cjson_dir.resolve()}",
            f"-DWEBRTC_DIR={args.webrtc_dir.resolve()}", f"-DTALK_SOURCE={args.talk_source.resolve()}",
            "reconfigure",
        ], check=True, timeout=180)
        subprocess.run(["ninja", "-C", str(build), "-j2"], check=True, timeout=600)
        verify_network(build / "talk_http_proof.elf")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
