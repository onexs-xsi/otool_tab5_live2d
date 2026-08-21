#!/usr/bin/env python3
"""
Ark Responses function-calling live probe (WP2 protocol facts).

One minimal tool + one question; prints every SSE event structure with
field inventory (values masked). Key from OTOOL_LLM_KEY_FILE (default
%TEMP%/otool_doubao_key.txt). Model fixed by the project budget rule.
"""

import json
import os
import sys
import urllib.request

BASE = "https://ark.cn-beijing.volces.com/api/v3"
MODEL = "doubao-seed-2-1-turbo-260628"

TOOL = {
    "type": "function",
    "name": "get_weather",
    "description": "Get current weather for a city",
    "strict": True,
    "parameters": {
        "type": "object",
        "properties": {
            "city": {"type": "string", "description": "city name"},
        },
        "required": ["city"],
        "additionalProperties": False,
    },
}


def get_key():
    key = os.environ.get("OTOOL_LLM_API_KEY", "")
    if not key:
        kf = os.environ.get("OTOOL_LLM_KEY_FILE",
                            os.path.join(os.environ.get("TEMP", "/tmp"), "otool_doubao_key.txt"))
        if os.path.exists(kf):
            key = open(kf, encoding="ascii").read().strip()
    if not key:
        sys.exit("no key")
    return key


def mask(obj):
    if isinstance(obj, dict):
        return {k: mask(v) for k, v in obj.items() if k not in ("request_id", "id")}
    if isinstance(obj, list):
        return [mask(v) for v in obj]
    if isinstance(obj, str) and len(obj) > 24:
        return "<str:%d>" % len(obj)
    return obj


def main():
    key = get_key()
    body = {
        "model": MODEL,
        "input": [{"role": "user", "content": "北京今天天气怎么样？"}],
        "tools": [TOOL],
        "tool_choice": "auto",
        "stream": True,
        "max_output_tokens": 512,
    }
    req = urllib.request.Request(BASE + "/responses", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json",
                                          "Authorization": "Bearer " + key,
                                          "Accept": "text/event-stream"})
    events = []
    with urllib.request.urlopen(req, timeout=60) as resp:
        print("status:", resp.status, "ctype:", resp.headers.get("Content-Type"))
        buf = b""
        while True:
            chunk = resp.read(512)
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
                    obj = json.loads(payload)
                    events.append((obj.get("type", "message"), obj))

    print("event sequence:", [e[0] for e in events])
    for name, obj in events:
        if obj is None:
            continue
        if "function_call" in name or name in ("response.output_item.added", "response.output_item.done"):
            print("----", name)
            print(json.dumps(mask(obj), ensure_ascii=False)[:600])
    # usage from completed
    for name, obj in events:
        if name == "response.completed" and obj:
            r = obj.get("response", {})
            if isinstance(r.get("usage"), dict):
                print("usage:", r["usage"])


if __name__ == "__main__":
    main()
