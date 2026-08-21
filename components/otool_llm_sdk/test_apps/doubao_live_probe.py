#!/usr/bin/env python3
"""
Doubao/Ark live protocol probe for otool_llm_sdk (WP5-style live smoke, opt-in).

- Reads the API key from OTOOL_LLM_KEY_FILE (default: %TEMP%/otool_doubao_key.txt)
  or the OTOOL_LLM_API_KEY env var. Never hardcodes a key.
- Sends ONE minimal streaming request per protocol (Responses / Chat), saves the
  raw SSE stream (sanitized: real response text replaced) to the given out file,
  and prints an event inventory (event name -> keys of the JSON payload).
- Model is fixed to doubao-seed-2-1-turbo-260628 (budget-limited testing).

Usage: python doubao_live_probe.py [out_prefix] [--chat]
"""

import json
import os
import sys
import time
import urllib.request

BASE = "https://ark.cn-beijing.volces.com/api/v3"
MODEL = "doubao-seed-2-1-turbo-260628"


def get_key():
    key = os.environ.get("OTOOL_LLM_API_KEY", "")
    if not key:
        key_file = os.environ.get("OTOOL_LLM_KEY_FILE",
                                  os.path.join(os.environ.get("TEMP", "/tmp"), "otool_doubao_key.txt"))
        if os.path.exists(key_file):
            with open(key_file, "r", encoding="ascii") as f:
                key = f.read().strip()
    if not key:
        print("no api key", file=sys.stderr)
        sys.exit(2)
    return key


def sanitize_text(text):
    """Replace user-facing content with a placeholder for the fixture file."""
    if not isinstance(text, str):
        return text
    if text.strip() in ("hi", "你好", "hello"):
        return "<input>"
    return "<generated>"


def sanitize(obj):
    if isinstance(obj, dict):
        return {k: sanitize(v) for k, v in obj.items() if k != "request_id"}
    if isinstance(obj, list):
        return [sanitize(v) for v in obj]
    if isinstance(obj, str):
        return sanitize_text(obj)
    return obj


def probe(path, body, out_prefix, label):
    key = get_key()
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode(),
                                 headers={
                                     "Content-Type": "application/json",
                                     "Authorization": "Bearer " + key,
                                     "Accept": "text/event-stream",
                                 })
    events = []
    start = time.time()
    with urllib.request.urlopen(req, timeout=60) as resp:
        status = resp.status
        ctype = resp.headers.get("Content-Type", "")
        rid = resp.headers.get("x-request-id", "")
        print(f"[{label}] status={status} content-type={ctype} x-request-id={rid}")
        if status != 200:
            raw = resp.read(2000).decode("utf-8", "replace")
            print(f"[{label}] error body: {raw}")
            return
        # read SSE lines
        buf = b""
        while True:
            chunk = resp.read(256)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode("utf-8", "replace").rstrip("\r")
                if line.startswith("data:"):
                    payload = line[5:].strip()
                    if payload == "[DONE]":
                        events.append(("DONE", None))
                        continue
                    try:
                        obj = json.loads(payload)
                    except Exception:
                        events.append(("RAW", payload))
                        continue
                    name = obj.get("type", "message")
                    events.append((name, obj))
        # leftover half line
        if buf.strip():
            events.append(("RAW_TAIL", buf.decode("utf-8", "replace")))
    elapsed = time.time() - start
    print(f"[{label}] {len(events)} SSE events in {elapsed:.2f}s")

    # event inventory
    inventory = {}
    for name, obj in events:
        keys = tuple(sorted(obj.keys())) if isinstance(obj, dict) else None
        inventory.setdefault(name, set()).add(keys)
    for name, shapes in inventory.items():
        print(f"  event '{name}': {len(shapes)} shape(s)")
        for s in sorted(shapes):
            print(f"    keys: {s}")

    # save sanitized fixture (jsonl: type + sanitized payload)
    with open(f"{out_prefix}_{label}.jsonl", "w", encoding="utf-8") as f:
        for name, obj in events:
            if obj is None:
                f.write(name + "\n")
            else:
                f.write(json.dumps({"type": name, "data": sanitize(obj)},
                                   ensure_ascii=False) + "\n")
    print(f"[{label}] sanitized fixture -> {out_prefix}_{label}.jsonl")

    # usage extraction
    for name, obj in events:
        if name in ("response.completed",) and isinstance(obj, dict):
            resp = obj.get("response", {})
            if isinstance(resp, dict) and isinstance(resp.get("usage"), dict):
                print(f"[{label}] usage: {resp['usage']}")
        if name == "message" and isinstance(obj, dict) and isinstance(obj.get("usage"), dict):
            print(f"[{label}] usage: {obj['usage']}")


def main():
    out_prefix = sys.argv[1] if len(sys.argv) > 1 else "live"
    do_chat = "--chat" in sys.argv
    do_responses = not do_chat or "--both" in sys.argv

    if do_responses:
        body = {
            "model": MODEL,
            "input": [{"role": "user", "content": "你好"}],
            "stream": True,
            "store": False,
            "max_output_tokens": 32,
        }
        probe("/responses", body, out_prefix, "responses")

    if do_chat:
        body = {
            "model": MODEL,
            "messages": [{"role": "user", "content": "你好"}],
            "stream": True,
            "max_tokens": 32,
        }
        probe("/chat/completions", body, out_prefix, "chat")


if __name__ == "__main__":
    main()
