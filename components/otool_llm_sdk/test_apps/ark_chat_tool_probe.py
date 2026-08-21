#!/usr/bin/env python3
"""
Ark Chat Completions tool-calling probe (WP8): verify delta.tool_calls event
structure with a minimal tool, then a two-turn loop (tool result message).
"""

import json
import os
import sys
import urllib.request

BASE = "https://ark.cn-beijing.volces.com/api/v3"
MODEL = "doubao-seed-2-1-turbo-260628"

TOOL = {
    "type": "function",
    "function": {
        "name": "get_device_status",
        "description": "Get device status",
        "parameters": {
            "type": "object",
            "properties": {},
            "required": [],
            "additionalProperties": False,
        },
    },
}


def get_key():
    k = os.environ.get("OTOOL_LLM_API_KEY", "")
    if not k:
        kf = os.environ.get("OTOOL_LLM_KEY_FILE",
                            os.path.join(os.environ.get("TEMP", "/tmp"), "otool_doubao_key.txt"))
        if os.path.exists(kf):
            k = open(kf, encoding="ascii").read().strip()
    return k


def stream(messages, tools=None, label=""):
    key = get_key()
    body = {"model": MODEL, "messages": messages, "stream": True, "max_tokens": 512}
    if tools:
        body["tools"] = tools
    req = urllib.request.Request(BASE + "/chat/completions", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json",
                                          "Authorization": "Bearer " + key,
                                          "Accept": "text/event-stream"})
    print(f"---- {label} ----")
    chunks = []
    with urllib.request.urlopen(req, timeout=60) as resp:
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
                        chunks.append(("DONE", None))
                        continue
                    obj = json.loads(payload)
                    chunks.append((obj.get("choices", [{}])[0].get("finish_reason"),
                                   obj.get("choices", [{}])[0].get("delta", {})))
    for fr, delta in chunks:
        if delta is None:
            print("  [DONE]")
            continue
        out = {}
        if delta.get("content"):
            out["content"] = delta["content"]
        if delta.get("tool_calls"):
            out["tool_calls"] = delta["tool_calls"]
        if fr:
            out["finish_reason"] = fr
        print("  ", json.dumps(out, ensure_ascii=False))
    return chunks


def main():
    # Turn 1: ask with tools -> expect tool_calls
    chunks = stream([{"role": "user", "content": "查询设备状态"}], [TOOL], "turn1: tool call")

    # Extract the tool call
    call_id = None
    name = None
    args = ""
    for fr, delta in chunks:
        for tc in (delta or {}).get("tool_calls", []) or []:
            if tc.get("id"):
                call_id = tc["id"]
            if tc.get("function", {}).get("name"):
                name = tc["function"]["name"]
            args += tc.get("function", {}).get("arguments", "")
    print(f"  extracted: id={call_id} name={name} args={args}")

    # Turn 2: assistant tool_calls + tool result
    history = [
        {"role": "user", "content": "查询设备状态"},
        {"role": "assistant", "content": None,
         "tool_calls": [{"id": call_id, "type": "function",
                         "function": {"name": name, "arguments": args}}]},
        {"role": "tool", "tool_call_id": call_id,
         "content": '{"ok":true,"result":{"uptime_s":123}}'},
    ]
    stream(history, [TOOL], "turn2: final answer")


if __name__ == "__main__":
    main()
