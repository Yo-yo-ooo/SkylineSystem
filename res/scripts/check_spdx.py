#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Yo-yo-ooo
# SPDX-License-Identifier: MIT
"""SPDX / REUSE coverage gate for SkylineSystem.

Every tracked file must either:
  * carry inline ``SPDX-FileCopyrightText`` + ``SPDX-License-Identifier``
    tags (no filename, author only), or
  * be covered by an annotation in ``REUSE.toml`` (data/binary/license-text
    files that cannot take an inline comment).

Every SPDX license identifier in use must have its text under ``LICENSES/``.

Usage:
  python3 res/scripts/check_spdx.py            # gate: exit 1 on gaps
  python3 res/scripts/check_spdx.py --list     # print per-file status
"""
import fnmatch
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)

LICENSE_DIR = "LICENSES"
REUSE_TOML = "REUSE.toml"
# 只认"行首是注释标记"的 SPDX 标签；生成器源码里的字符串字面量不算。
LICENSE_RE = re.compile(
    r"^[ \t]*(?:\*|//|#|;|--|/\*|<!--|@REM)[ \t]*"
    r"SPDX-License-Identifier:[ \t]*([^\r\n]+?)[ \t]*$", re.MULTILINE)
COPYRIGHT_RE = re.compile(r"SPDX-FileCopyrightText:")
ID_RE = re.compile(r"^[A-Za-z0-9.+-]+(?: WITH [A-Za-z0-9.+-]+)?"
                   r"(?: (?:OR|AND) (?:[A-Za-z0-9.+-]+(?: WITH [A-Za-z0-9.+-]+)?))*$")


def tracked_files():
    """已跟踪 + 未跟踪但未被 .gitignore 忽略的源文件。

    这样新增文件在提交前就会被门禁覆盖，避免"先提交再补头"。构建产物
    （iso/img/obj/bin 等）都在 .gitignore 里，不会进入列表。
    """
    out = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        capture_output=True, text=True, check=True).stdout
    return [f for f in out.splitlines() if f]


def load_annotations():
    """读取 REUSE.toml；返回 [(patterns, has_copy, has_lic)]。"""
    path = REUSE_TOML
    if not os.path.exists(path):
        return []
    try:
        import tomllib
        with open(path, "rb") as fh:
            data = tomllib.load(fh)
    except ImportError:  # Python < 3.11: 只做最小解析
        data = {}
        text = open(path, "r", encoding="utf-8").read()
        for block in text.split("[[annotations]]")[1:]:
            paths = re.findall(r"\"([^\"]*)\"", block.split("SPDX-")[0])
            data.setdefault("annotations", []).append({
                "path": paths,
                "SPDX-FileCopyrightText": "SPDX-FileCopyrightText" in block,
                "SPDX-License-Identifier": "SPDX-License-Identifier" in block,
            })
    out = []
    for ann in data.get("annotations", []):
        pats = ann.get("path", [])
        if isinstance(pats, str):
            pats = [pats]
        out.append((pats,
                    bool(ann.get("SPDX-FileCopyrightText")),
                    bool(ann.get("SPDX-License-Identifier"))))
    return out


def match_annotation(path, annotations):
    for pats, has_copy, has_lic in annotations:
        for pat in pats:
            # REUSE.toml 用 ** 表示任意深度；fnmatch 的 * 本身跨 "/"，
            # 把 ** 折叠为 * 后语义一致。
            glob = pat.replace("**", "*")
            if fnmatch.fnmatchcase(path, glob):
                return has_copy, has_lic
    return None


def license_ids_from(text):
    ids = set()
    for m in LICENSE_RE.finditer(text):
        expr = m.group(1).strip()
        # 去掉注释收尾 / 引号 / 逗号等非标识符字符。
        for suffix in ("*/", "-->", "\"", ",", "\\"):
            if expr.endswith(suffix):
                expr = expr[: -len(suffix)].rstrip()
        if not expr or not ID_RE.match(expr):
            continue
        for part in re.split(r"\s+(?:OR|AND)\s+", expr):
            part = part.strip()
            if " WITH " in part:
                base, exc = part.split(" WITH ", 1)
                ids.add(base.strip())
                ids.add(exc.strip())
            else:
                ids.add(part)
    return ids


def annotations_ids(annotations):
    # 注解里的标识符也要有文本；annotation 元组只有布尔，重新读一次原文件。
    try:
        import tomllib
        with open(REUSE_TOML, "rb") as fh:
            data = tomllib.load(fh)
    except ImportError:
        return set()
    ids = set()
    for ann in data.get("annotations", []):
        expr = ann.get("SPDX-License-Identifier", "")
        for part in re.split(r"\s+(?:OR|AND)\s+", expr):
            part = part.strip()
            if " WITH " in part:
                base, exc = part.split(" WITH ", 1)
                ids.add(base.strip())
                ids.add(exc.strip())
            elif part:
                ids.add(part)
    return ids


def main():
    show_list = "--list" in sys.argv
    files = tracked_files()
    annotations = load_annotations()

    covered_inline = 0
    covered_annot = 0
    uncovered = []
    used_ids = set()
    for path in files:
        if path.startswith(LICENSE_DIR + "/"):
            continue
        try:
            with open(path, "rb") as fh:
                blob = fh.read()
        except OSError:
            continue
        text = blob.decode("utf-8", "replace")
        has_lic = "SPDX-License-Identifier" in text
        has_cp = COPYRIGHT_RE.search(text) is not None
        if has_lic and has_cp:
            covered_inline += 1
            used_ids |= license_ids_from(text)
            if show_list:
                print("inline   %s" % path)
            continue
        ann = match_annotation(path, annotations)
        if ann and ann[0] and ann[1]:
            covered_annot += 1
            if show_list:
                print("annot    %s" % path)
            continue
        uncovered.append(path)
        if show_list:
            print("MISSING  %s" % path)

    used_ids |= annotations_ids(annotations)
    missing_text = sorted(i for i in used_ids
                          if i and not i.startswith("LicenseRef-")
                          and not os.path.exists(os.path.join(LICENSE_DIR, i)))
    missing_ref = sorted(i for i in used_ids if i.startswith("LicenseRef-")
                         and not os.path.exists(os.path.join(LICENSE_DIR, i)))
    missing_text = missing_text + missing_ref

    total = covered_inline + covered_annot + len(uncovered)
    print("SPDX coverage: %d/%d files (%.1f%%) inline=%d annotated=%d"
          % (total - len(uncovered), total,
             100.0 * (total - len(uncovered)) / max(1, total),
             covered_inline, covered_annot))
    if missing_text:
        print("missing license texts:", ", ".join(missing_text))
    if uncovered:
        print("uncovered files: %d (first 20)" % len(uncovered))
        for u in uncovered[:20]:
            print("  " + u)
        return 1
    if missing_text:
        return 1
    print("SPDX check: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
