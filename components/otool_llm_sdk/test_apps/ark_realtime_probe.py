#!/usr/bin/env python3
"""WP10 最小 Realtime 探针：验证火山方舟 Doubao Realtime API 的 WSS 建连、
鉴权与事件往返（文本模态；不发送音频）。

用法（先按官方文档确认参数）：
  python ark_realtime_probe.py --key <ark-api-key> [--endpoint wss://...]
                               [--model <realtime-model-id>] [--text "你好"]

输出：连接/鉴权/首事件/文本往返的时间线；退出码 0 = 探针通过。

依赖：pip install websockets
"""

import argparse
import asyncio
import json
import sys
import time

DEFAULT_ENDPOINT = "wss://ark.cn-beijing.volces.com/api/v3/realtime"
DEFAULT_MODEL = "doubao-seed-realtime"  # 以官方预置模型列表为准


async def probe(endpoint, api_key, model, text, timeout_s):
    import websockets

    headers = {
        "Authorization": f"Bearer {api_key}",
        "OpenAI-Beta": "realtime=v1",
    }
    t0 = time.time()
    print(f"[probe] connecting {endpoint}")
    async with websockets.connect(endpoint, additional_headers=headers,
                                  max_size=1 << 20) as ws:
        print(f"[probe] connected +{time.time() - t0:.2f}s")

        # 1) session.update：文本模态，禁用语音（探针只验证事件往返）
        await ws.send(json.dumps({
            "type": "session.update",
            "session": {
                "model": model,
                "modalities": ["text"],
                "instructions": "你是探针测试助手，简短回答。",
            },
        }))
        print(f"[probe] session.update sent +{time.time() - t0:.2f}s")

        # 2) 等待 session.created / error
        deadline = time.time() + timeout_s
        got_session = False
        while time.time() < deadline:
            raw = await asyncio.wait_for(ws.recv(), timeout=5)
            evt = json.loads(raw)
            etype = evt.get("type")
            print(f"[probe]   <- {etype} +{time.time() - t0:.2f}s")
            if etype == "session.created":
                got_session = True
                break
            if etype == "error":
                print(f"[probe] ERROR: {evt}")
                return 1
        if not got_session:
            print("[probe] no session.created within timeout")
            return 1

        # 3) conversation.item.create + response.create（文本）
        await ws.send(json.dumps({
            "type": "conversation.item.create",
            "item": {"type": "message", "role": "user",
                     "content": [{"type": "input_text", "text": text}]},
        }))
        await ws.send(json.dumps({"type": "response.create", "response": {}}))
        print(f"[probe] item+response.create sent +{time.time() - t0:.2f}s")

        # 4) 收 response.text.delta / response.done / error
        got_text = False
        done = False
        while time.time() < deadline and not done:
            raw = await asyncio.wait_for(ws.recv(), timeout=5)
            evt = json.loads(raw)
            etype = evt.get("type")
            if etype == "response.text.delta":
                got_text = True
                print(f"[probe]   text: {evt.get('delta', '')!r}")
            elif etype in ("response.done", "error"):
                done = True
                if etype == "error":
                    print(f"[probe] ERROR: {evt}")
                    return 1
                print(f"[probe] response.done +{time.time() - t0:.2f}s")

        if not got_text:
            print("[probe] no text delta received")
            return 1
        print(f"[probe] PASS (text round-trip, total {time.time() - t0:.2f}s)")
        return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--key", required=True)
    ap.add_argument("--endpoint", default=DEFAULT_ENDPOINT)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--text", default="你好，请用一句话自我介绍")
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args()
    try:
        sys.exit(asyncio.run(probe(args.endpoint, args.key, args.model,
                                   args.text, args.timeout)))
    except ImportError:
        print("需要 websockets 库：pip install websockets")
        sys.exit(2)
    except Exception as e:  # noqa: BLE001 - probe reports any failure
        print(f"[probe] FAILED: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
