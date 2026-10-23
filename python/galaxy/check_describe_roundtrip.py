#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Round-trip the GALAXY `?` describe response as Python AND JavaScript.

Runs galaxy_namespace_tool --describe and parses the response (minus the
leading '?') with ast.literal_eval, verifying every key and value at every
level is a string — the every-value-quoted rule is what keeps the Python
and JavaScript literal grammars from diverging. When node is on the PATH,
the same text is also evaluated as a JavaScript expression and the entry
count compared.

Usage:
    check_describe_roundtrip.py <path-to-galaxy-namespace-tool>
"""

import ast
import json
import os
import shutil
import subprocess
import sys
import tempfile


def check_python(text: str) -> dict:
    obj = ast.literal_eval(text)
    assert isinstance(obj, dict) and obj, "top level must be a non-empty dict"
    for path, entry in obj.items():
        assert isinstance(path, str), f"non-string key {path!r}"
        assert isinstance(entry, dict), f"{path}: entry must be a dict"
        for k, v in entry.items():
            ok = isinstance(v, str) or (
                isinstance(v, list) and all(isinstance(i, str) for i in v)
            )
            assert isinstance(k, str) and ok, (
                f"{path}: {k!r}: every value must be a quoted string (or a list of them)"
            )
    return obj


def check_javascript(text: str, expected_entries: int) -> bool:
    node = shutil.which("node")
    if not node:
        print("node not found; JavaScript round-trip skipped")
        return True
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(text)
        name = f.name
    script = (
        "const fs = require('fs');"
        f"const text = fs.readFileSync({json.dumps(name)}, 'utf8');"
        "const obj = eval('(' + text + ')');"
        "const paths = Object.keys(obj);"
        "for (const p of paths) {"
        "  for (const [k, v] of Object.entries(obj[p])) {"
        "    const ok = typeof v === 'string' || (Array.isArray(v) && v.every(i => typeof i === 'string'));"
        "    if (!ok) { throw new Error(p + ': ' + k + ' is not a string or string list'); }"
        "  }"
        "}"
        "process.stdout.write(String(paths.length));"
    )
    env = dict(os.environ, NO_COLOR="1", FORCE_COLOR="0")
    out = subprocess.run([node, "-e", script], capture_output=True, text=True, env=env)
    if out.returncode != 0:
        print(f"JavaScript round-trip FAILED:\n{out.stderr}", file=sys.stderr)
        return False
    got = int(out.stdout.strip())
    if got != expected_entries:
        print(
            f"JavaScript entry count {got} != Python {expected_entries}",
            file=sys.stderr,
        )
        return False
    return True


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = subprocess.run(
        [sys.argv[1], "--describe"], capture_output=True, text=True, check=True
    )
    text = out.stdout.strip()
    assert text.startswith("?"), "describe response must start with '?'"
    text = text[1:]

    obj = check_python(text)
    if not check_javascript(text, len(obj)):
        return 1
    print(f"OK: describe response round-trips ({len(obj)} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
