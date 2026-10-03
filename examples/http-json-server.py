#!/usr/bin/env python3
"""Loopback-only demo for 93-http-json-client.gti; no third-party packages."""

import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def reply(self, status, body):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/user":
            self.reply(200, b'{"name":"local demo"}')
        elif self.path == "/invalid-json":
            self.reply(200, b'{')
        elif self.path == "/missing-name":
            self.reply(200, b'{}')
        elif self.path == "/oversize":
            self.reply(200, b'x' * 65537)
        else:
            self.reply(404, b'{"error":"not found"}')

    def do_POST(self):
        if self.path != "/user":
            self.reply(404, b'{}')
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 4096:
                raise ValueError("body limit")
            if self.headers.get("Content-Type") != "application/json":
                raise ValueError("content type")
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict) or not isinstance(body.get("name"), str):
                raise ValueError("name must be a string")
        except (ValueError, UnicodeError):
            self.reply(400, b'{"error":"expected a JSON object with name"}')
            return
        self.reply(201, json.dumps(body).encode())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    with ThreadingHTTPServer(("127.0.0.1", args.port), Handler) as server:
        print(f"Demo URL: http://127.0.0.1:{server.server_port}/user", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
