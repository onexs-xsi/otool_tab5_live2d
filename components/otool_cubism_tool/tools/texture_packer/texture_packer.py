#!/usr/bin/env python3
"""texture_packer — Live2D 纹理转换工具（PC 端，不进入固件）

可行性报告 §3.2/§5.3：设备端不解析 PNG；PC 端转换为设备格式
（预乘 alpha RGBA4444 / RGB565 + A8），可选 LZ4 压缩。

用法:
    python texture_packer.py <in.png> -o <out.raw> [--max 1024] [--fmt rgba4444|rgb565]

输出:
    <out.raw>        像素数据（小端）
    <out.raw.meta>   JSON：width/height/format/size 等元数据
"""

import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def premultiply(rgba: np.ndarray) -> np.ndarray:
    """预乘 alpha：rgb = rgb * alpha（RGBA4444 需要）"""
    out = rgba.astype(np.float32)
    a = out[..., 3:4] / 255.0
    out[..., 0:3] *= a
    return np.clip(out, 0, 255).astype(np.uint8)


def to_rgba4444(rgba: np.ndarray) -> bytes:
    """RGBA4444（小端 u16：R4 G4 B4 A4）"""
    r = (rgba[..., 0] >> 4).astype(np.uint16)
    g = (rgba[..., 1] >> 4).astype(np.uint16)
    b = (rgba[..., 2] >> 4).astype(np.uint16)
    a = (rgba[..., 3] >> 4).astype(np.uint16)
    u16 = (r << 12) | (g << 8) | (b << 4) | a
    return u16.astype("<u2").tobytes()


def to_rgb565(rgb: np.ndarray) -> bytes:
    r = (rgb[..., 0] >> 3).astype(np.uint16)
    g = (rgb[..., 1] >> 2).astype(np.uint16)
    b = (rgb[..., 2] >> 3).astype(np.uint16)
    u16 = (r << 11) | (g << 5) | b
    return u16.astype("<u2").tobytes()


def main(argv):
    ap = argparse.ArgumentParser(description="Live2D 纹理转换工具")
    ap.add_argument("input", help="输入 PNG")
    ap.add_argument("-o", "--output", required=True, help="输出 raw 文件")
    ap.add_argument("--max", type=int, default=1024, help="最长边上限（默认 1024）")
    ap.add_argument("--fmt", choices=["rgba4444", "rgb565"], default="rgba4444")
    args = ap.parse_args(argv)

    im = Image.open(args.input).convert("RGBA")
    w, h = im.size
    scale = min(1.0, args.max / max(w, h))
    if scale < 1.0:
        im = im.resize((max(1, int(w * scale)), max(1, int(h * scale))),
                       Image.LANCZOS)
        w, h = im.size
        print(f"resized to {w}x{h} (scale={scale:.3f})")

    rgba = np.asarray(im)

    if args.fmt == "rgba4444":
        rgba = premultiply(rgba)
        data = to_rgba4444(rgba)
    else:
        data = to_rgb565(rgba)

    out = Path(args.output)
    out.write_bytes(data)
    meta = {
        "width": w, "height": h, "format": args.fmt,
        "bytes_per_pixel": 2, "pixel_bytes": len(data),
        "source": str(Path(args.input).name),
    }
    out.with_suffix(out.suffix + ".meta").write_text(
        json.dumps(meta, indent=2), encoding="utf-8")
    print(f"wrote {out} ({len(data)} bytes, {w}x{h} {args.fmt})")
    print(f"meta: {out}.meta")


if __name__ == "__main__":
    main(sys.argv[1:])
