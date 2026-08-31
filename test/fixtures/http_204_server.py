#!/usr/bin/env python3
"""Loopback-only deterministic HTTP 204 origin for Mixed probe fixtures."""

from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _respond(self) -> None:
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()

    do_GET = _respond
    do_HEAD = _respond

    def log_message(self, _format: str, *_args: object) -> None:
        return


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    server.serve_forever(poll_interval=0.1)


if __name__ == "__main__":
    main()
