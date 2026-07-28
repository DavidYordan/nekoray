#!/usr/bin/env python3
"""Loopback-only HTTP proxy stub that identifies one logical test line."""

from __future__ import annotations

import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    response_status = 200
    line_name = ""

    def _record(self, method: str, target: str) -> None:
        print(
            json.dumps(
                {
                    "line": self.line_name,
                    "method": method,
                    "target": target,
                },
                sort_keys=True,
            ),
            flush=True,
        )

    def _respond(self) -> None:
        self._record(self.command, self.path)
        self.send_response(self.response_status)
        self.send_header("X-NekoRay-Test-Line", self.line_name)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()

    do_GET = _respond
    do_HEAD = _respond

    def do_CONNECT(self) -> None:
        self.send_response(200, "Connection Established")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.flush()

        self.connection.settimeout(5)
        request_line = self.rfile.readline(65537)
        if not request_line or len(request_line) > 65536:
            self.close_connection = True
            return
        for _ in range(200):
            header_line = self.rfile.readline(65537)
            if not header_line or header_line in (b"\r\n", b"\n"):
                break
        decoded_request_line = request_line.decode("iso-8859-1", errors="replace").strip()
        parts = decoded_request_line.split(" ", 2)
        method = parts[0] if parts else ""
        target = parts[1] if len(parts) > 1 else ""
        self._record(method, target)

        response = (
            f"HTTP/1.1 {self.response_status} Line Response\r\n"
            f"X-NekoRay-Test-Line: {self.line_name}\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n"
        )
        self.wfile.write(response.encode("ascii"))
        self.wfile.flush()
        self.close_connection = True

    def log_message(self, _format: str, *_args: object) -> None:
        return


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--status", type=int, required=True)
    parser.add_argument("--line", required=True)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if not 200 <= args.status <= 599:
        parser.error("--status must be between 200 and 599")
    if not args.line or any(character in args.line for character in "\r\n"):
        parser.error("--line must be a non-empty single-line value")

    Handler.response_status = args.status
    Handler.line_name = args.line
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.serve_forever(poll_interval=0.1)


if __name__ == "__main__":
    main()
