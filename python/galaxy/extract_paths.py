#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Extract raw control-point rows from a GALAXY Programming Guide PDF.

Mechanical extraction only: finds lines in the Control Points reference
tables that start with '/', normalizes typography (curly quotes, ligatures),
and emits "path<TAB>default" rows. It deliberately does NOT correct the
PDF-extraction artifacts (dropped quotes, merged or split segments) — those
corrections are curated, with justifications, in check_namespace.py, so the
raw data stays byte-faithful to what the document says.

Usage:
    uv run --with pypdf extract_paths.py <guide.pdf> > data/<edition>.paths
"""

import re
import sys


def normalize_typography(s: str) -> str:
    return (
        s.replace("‘", "'")
        .replace("’", "'")
        .replace("“", '"')
        .replace("”", '"')
        .replace("ﬁ", "fi")
        .replace("ﬂ", "fl")
        .replace("–", "-")
        .replace("—", "-")
    )


def extract(pdf_path: str):
    from pypdf import PdfReader

    rows = []
    reader = PdfReader(pdf_path)
    for page in reader.pages:
        text = normalize_typography(page.extract_text() or "")
        for line in text.splitlines():
            line = line.strip()
            if not line.startswith("/"):
                continue
            # Guide URLs and OSC example lines are not table rows.
            if line.startswith("/products/") or "," in line.split(" ")[0]:
                continue
            m = re.match(r"^(/[A-Za-z0-9_/.\-]+)\s*(.*)$", line)
            if not m:
                continue
            path, rest = m.group(1), m.group(2).strip()
            # Table rows carry a quoted default (possibly with the leading
            # quote lost to extraction); set-command examples continue with
            # '=value' immediately after the path. A '=' deeper inside the
            # default is data (/status/log_message embeds ERR_MJR=0).
            if "=" in path or rest.startswith("="):
                continue
            # Regex-example fragments truncate at the wildcard, leaving a
            # trailing slash; no table row ends with one.
            if path.endswith("/"):
                continue
            if rest and not (rest.endswith("'") or rest.startswith("'")):
                continue
            rows.append((path, rest))
    return rows


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    seen = set()
    for path, default in extract(sys.argv[1]):
        key = (path, default)
        if key in seen:
            continue
        seen.add(key)
        print(f"{path}\t{default}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
