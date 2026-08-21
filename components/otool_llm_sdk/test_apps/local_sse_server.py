#!/usr/bin/env python3
"""
Local controllable SSE server for otool_llm_sdk transport tests (WP4).

Serves protocol fixtures with arbitrary chunk boundaries, delays and failure
shapes. Run on the host, point a test client at http://<host>:18080.

Usage:
    python local_sse_server.py [port]
"""

import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080


def responses_fixture():
    return [
        ("response.created",
         {"type": "response.created",
          "response": {"id": "resp_local_001", "model": "local-model",
                       "status": "in_progress"}}),
        ("response.output_text.delta",
         {"type": "response.output_text.delta", "item_id": "i1", "output_index": 0,
          "response_id": "resp_local_001", "delta": "你好，"}),
        ("response.output_text.delta",
         {"type": "response.output_text.delta", "item_id": "i1", "output_index": 0,
          "response_id": "resp_local_001", "delta": "world 🌍"}),
        ("response.output_text.delta",
         {"type": "response.output_text.delta", "item_id": "i1", "output_index": 0,
          "response_id": "resp_local_001", "delta": " (3rd chunk)"}),
        ("response.output_text.done",
         {"type": "response.output_text.done", "item_id": "i1", "output_index": 0,
          "response_id": "resp_local_001", "text": "你好，world 🌍 (3rd chunk)"}),
        ("response.completed",
         {"type": "response.completed",
          "response": {"id": "resp_local_001", "status": "completed",
                       "usage": {"input_tokens": 9, "output_tokens": 21,
                                 "total_tokens": 30}}}),
    ]


def chat_fixture(with_usage=False):
    chunks = [
        {"id": "chatcmpl-local", "object": "chat.completion.chunk",
         "choices": [{"index": 0, "delta": {"role": "assistant"}, "finish_reason": None}]},
        {"id": "chatcmpl-local", "object": "chat.completion.chunk",
         "choices": [{"index": 0, "delta": {"content": "Hi"}, "finish_reason": None}]},
        {"id": "chatcmpl-local", "object": "chat.completion.chunk",
         "choices": [{"index": 0, "delta": {"content": " from chat"}, "finish_reason": None}]},
        {"id": "chatcmpl-local", "object": "chat.completion.chunk",
         "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]},
    ]
    if with_usage:
        chunks[-1]["usage"] = {"prompt_tokens": 4, "completion_tokens": 6, "total_tokens": 10}
    return chunks


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _log_request(self, path):
        auth = self.headers.get("Authorization", "")
        has_auth = "Bearer" in auth
        print(f"[req] {self.command} {path} auth={'yes' if has_auth else 'NO'} "
              f"authlen={len(auth)} ctype={self.headers.get('Content-Type')} "
              f"bodylen={int(self.headers.get('Content-Length', 0))}")

    def _sse_headers(self, extra=None, status=200):
        self.send_response(status)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        if extra:
            for k, v in extra.items():
                self.send_header(k, v)
        self.end_headers()

    def _send_event(self, name, obj, flush=True):
        payload = json.dumps(obj, ensure_ascii=False)
        if name:
            self.wfile.write(f"event: {name}\n".encode())
        self.wfile.write(f"data: {payload}\n\n".encode())
        if flush:
            self.wfile.flush()

    def _send_raw(self, data):
        self.wfile.write(data)
        self.wfile.flush()

    def do_POST(self):
        self._log_request(self.path)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))

        if self.path == "/ok":
            self._sse_headers()
            for name, obj in responses_fixture():
                # stream in small odd-sized pieces to exercise chunk boundaries
                payload = json.dumps(obj, ensure_ascii=False)
                event_bytes = (f"event: {name}\ndata: {payload}\n\n").encode()
                i = 0
                sizes = [1, 3, 5, 7, 11]
                while i < len(event_bytes):
                    n = sizes[(i // 13) % len(sizes)]
                    self._send_raw(event_bytes[i:i + n])
                    i += n
                    time.sleep(0.005)
            self._send_raw(b"")
            return

        if self.path == "/chat":
            self._sse_headers()
            for chunk in chat_fixture(with_usage=True):
                self._send_event(None, chunk)
            self._send_raw(b"data: [DONE]\n\n")
            return

        if self.path == "/slow":
            self._sse_headers()
            for name, obj in responses_fixture():
                self._send_event(name, obj)
                time.sleep(0.8)
            return

        if self.path == "/401":
            self._sse_headers({"x-request-id": "req-401"}, status=401)
            self._send_raw(json.dumps(
                {"error": {"message": "Invalid API key provided",
                           "code": "invalid_api_key", "request_id": "req-401"}}).encode())
            return

        if self.path == "/429":
            self._sse_headers({"Retry-After": "5", "x-request-id": "req-429"}, status=429)
            self._send_raw(json.dumps(
                {"error": {"message": "Rate limit exceeded", "code": "rate_limit",
                           "request_id": "req-429"}}).encode())
            return

        if self.path == "/500":
            self._sse_headers({"x-request-id": "req-500"}, status=500)
            self._send_raw(json.dumps(
                {"error": {"message": "internal server error", "code": "server_error",
                           "request_id": "req-500"}}).encode())
            return

        if self.path == "/badtype":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(b"<html>not sse</html>")
            return

        if self.path == "/half":
            self._sse_headers()
            self._send_raw(b'event: response.created\ndata: {"type":"response.created",')
            # connection closes mid-event without a blank line
            return

        if self.path == "/oversize":
            self._sse_headers()
            big = "x" * 20000
            self._send_raw(f"data: {big}\n\n".encode())
            return

        if self.path == "/errorjson":
            self._sse_headers()
            self._send_raw(b"data: {not valid json}\n\n")
            return

        if self.path == "/eof-no-terminal":
            self._sse_headers()
            self._send_raw(b'data: {"type":"response.created","response":{"id":"r1"}}\n\n')
            # clean EOF without any terminal event
            return

        if self.path == "/multi-choice":
            self._sse_headers()
            self._send_raw(json.dumps({
                "id": "chatcmpl-mc", "choices": [
                    {"index": 0, "delta": {"content": "a"}, "finish_reason": None},
                    {"index": 1, "delta": {"content": "b"}, "finish_reason": None}]}).encode() + b"\n\n")
            return

        self.send_response(404)
        self.end_headers()

    def do_GET(self):
        if self.path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"ok":true}')
            return
        self.send_response(404)
        self.end_headers()


if __name__ == "__main__":
    print(f"local SSE server on 0.0.0.0:{PORT}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
