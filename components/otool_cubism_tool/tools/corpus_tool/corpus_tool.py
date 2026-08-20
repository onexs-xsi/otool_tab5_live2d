#!/usr/bin/env python3
"""corpus_tool — 模型语料清单工具（PC 端，不进入固件）

可行性报告 §12 任务 2：记录生产/参考模型的 SHA-256、moc 版本字节、
model3.json 统计（纹理/参数/部件/动作/表情），输出 corpus manifest。

用法:
    python corpus_tool.py <模型目录...> [-o manifest.yml]

说明:
    - 仅读取文件头部与 JSON 元数据，不实现 moc3 布局解析；
      deformer/drawable/keyform/mask 统计待 C0 parser 冻结后扩展。
    - 不修改、不复制模型文件；输出清单供研究审批与向量登记使用。
"""

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

MOC3_MAGIC = b"MOC3"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def moc3_header(path: Path):
    """返回 (magic, version_byte) 或 (None, None)。版本字节取小端 u32。"""
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
            if magic != MOC3_MAGIC:
                return None, None
            raw = f.read(4)
            if len(raw) < 4:
                return magic, None
            return magic, struct.unpack("<I", raw)[0]
    except OSError:
        return None, None


def scan_model3(model_json: Path) -> dict:
    """解析 model3.json 与同名 cdi3.json 的顶层统计。失败时返回错误信息。"""
    try:
        data = json.loads(model_json.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return {"error": f"{type(e).__name__}: {e}"}

    def count_list(v):
        return len(v) if isinstance(v, list) else None

    file_refs = data.get("FileReferences") or {}

    # Parameters/Parts 在 <name>.cdi3.json（Cubism Display Info）
    cdi = {}
    cdi_name = model_json.name.replace(".model3.json", ".cdi3.json")
    cdi_path = model_json.with_name(cdi_name)
    try:
        cdi = json.loads(cdi_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        cdi = {"error": f"{type(e).__name__}: {e}"}

    motions = {}
    for k, v in (file_refs.get("Motions") or {}).items():
        motions[k] = len(v) if isinstance(v, list) else None

    return {
        "textures": count_list(file_refs.get("Textures")),
        "parameters": count_list(cdi.get("Parameters")),
        "parts": count_list(cdi.get("Parts")),
        "parameter_groups": count_list(data.get("Groups")),
        "motions": motions,
        "expressions": count_list(file_refs.get("Expressions")),
        "physics_file": (file_refs.get("Physics") or {}).get("File")
            if isinstance(file_refs.get("Physics"), dict) else None,
        "pose_file": (file_refs.get("Pose") or {}).get("File")
            if isinstance(file_refs.get("Pose"), dict) else None,
        "moc_ref": (file_refs.get("Moc3") or {}).get("File")
            if isinstance(file_refs.get("Moc3"), dict) else None,
        "cdi_error": cdi.get("error"),
    }


def find_model3_json(root: Path):
    """查找 <name>.model3.json 或 model3.json。"""
    for candidate in (root / "model3.json",):
        if candidate.is_file():
            return candidate
    for candidate in root.glob("*.model3.json"):
        if candidate.is_file():
            return candidate
    return None


def scan_dir(root: Path) -> dict:
    model_json = find_model3_json(root)
    if model_json is None:
        return {"path": str(root), "error": "no model3.json"}

    entry = {
        "path": str(root),
        "model3_sha256": sha256_file(model_json),
        "model3_stats": scan_model3(model_json),
        "files": [],
    }

    for f in sorted(root.rglob("*")):
        if not f.is_file():
            continue
        rel = f.relative_to(root)
        rel_s = rel.as_posix()
        if rel_s == model_json.name:
            continue
        info = {"name": rel_s, "sha256": sha256_file(f), "size": f.stat().st_size}
        if f.suffix.lower() == ".moc3":
            magic, ver = moc3_header(f)
            info["moc3_magic"] = magic.decode("ascii", "replace") if magic else None
            info["moc3_version_byte"] = ver
        entry["files"].append(info)
    return entry


def main(argv):
    ap = argparse.ArgumentParser(description="Live2D 模型语料清单工具")
    ap.add_argument("dirs", nargs="+", help="模型目录（含 model3.json）")
    ap.add_argument("-o", "--output", default="corpus_manifest.yml",
                    help="输出 YAML 路径")
    args = ap.parse_args(argv)

    entries = []
    for d in args.dirs:
        root = Path(d)
        if not root.is_dir():
            print(f"skip: not a directory: {d}", file=sys.stderr)
            continue
        entries.append(scan_dir(root))

    lines = ["# corpus manifest（corpus_tool 生成）",
             "# 字段语义见 spec/test_vector_schema.md 与 research/reference_manifest.yml",
             "schema_version: 1",
             f"models:"]
    for e in entries:
        lines.append(f"  - path: {e.get('path')}")
        if "error" in e:
            lines.append(f"    error: {e['error']}")
            continue
        lines.append(f"    model3_sha256: {e['model3_sha256']}")
        st = e["model3_stats"]
        lines.append("    model3_stats:")
        if "error" in st:
            lines.append(f"      error: {st['error']}")
        else:
            lines.append(f"      textures: {st['textures']}")
            lines.append(f"      parameters: {st['parameters']}")
            lines.append(f"      parts: {st['parts']}")
            lines.append(f"      parameter_groups: {st['parameter_groups']}")
            lines.append(f"      motions: {st['motions']}")
            lines.append(f"      expressions: {st['expressions']}")
            lines.append(f"      physics: {st['physics_file']}")
            lines.append(f"      pose: {st['pose_file']}")
            lines.append(f"      moc_ref: {st['moc_ref']}")
            if st.get("cdi_error"):
                lines.append(f"      cdi_error: {st['cdi_error']}")
        lines.append("    files:")
        for f in e["files"]:
            extra = ""
            if f.get("moc3_version_byte") is not None:
                extra = f" moc3_version={f['moc3_version_byte']}"
            lines.append(f"      - name: {f['name']} size: {f['size']}"
                         f" sha256: {f['sha256']}{extra}")

    out = Path(args.output)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {len(entries)} model(s) -> {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
