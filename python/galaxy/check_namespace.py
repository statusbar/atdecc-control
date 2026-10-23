#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Cross-check the generated GALAXY namespace against the programming guide.

Compares the normalized path shapes (numeric segments -> '#') of the C++
generator's output (galaxy_namespace_tool) against the raw rows extracted
from BOTH guide editions (data/*.paths, produced by extract_paths.py). The
guide data passes through a curated list of corrections, each one a
documented PDF-extraction artifact; the two editions must agree with each
other and with the generator on every shape.

Usage:
    check_namespace.py --tool <path-to-galaxy-namespace-tool> [--data <dir>]
"""

import argparse
import pathlib
import re
import subprocess
import sys

# PDF-extraction artifacts, each verified against the page image / context:
#   - doubled slash in the input ushaping table
#   - '/processing/output/1/8/frequency' dropped the 'eq' segment
#   - 'as_path5' dropped a slash
#   - fan tach rows merged with following-line fragments
# Rows mapped to None are excluded: '/processing/matrix/Matrix' is a mangled
# table-header row present in both document editions and names no control
# point.
SHAPE_FIXUPS = {
    "/processing/input/#//ushaping/#/frequency": "/processing/input/#/ushaping/#/frequency",
    "/processing/output/#/#/frequency": "/processing/output/#/eq/#/frequency",
    "/status/clock/gptp/#/as_path5/trace_id": "/status/clock/gptp/#/as_path/#/trace_id",
    "/status/hardware/board/digital/fan/#/tach/h": "/status/hardware/board/digital/fan/#/tach",
    "/status/hardware/board/digital/fan/#/tach/ha": "/status/hardware/board/digital/fan/#/tach",
    "/status/hardware/board/digital/fan/#/tach/hardware/board/digital/fan/#/": "/status/hardware/board/digital/fan/#/tach",
    "/processing/matrix/Matrix": None,
}

# Guide-vs-firmware corrections, each verified against the live Q1 unit
# (firmware 2.10.0, 2026-08-29; the reconciliation reported 9766/9766 schema
# points, 0 unknown, 0 missing): the guide
# misprints these names in both editions; the generator emits the device's
# spelling.
DEVICE_CORRECTIONS = {
    "/status/connected_osc_upd_client_count": "/status/connected_osc_udp_client_count",
    "/status/hardware/board/digital/vxco_pull": "/status/hardware/board/digital/vcxo_pull",
    "/status/hardware/board/primary_io/i2c/analog_in_eeprom_present/#": "/status/hardware/board/primary_io/i2c/analog_in_eeprom_present",
}

# Shapes the generator emits beyond the guide's printed rows, each backed by
# a note in the guide rather than a table row of its own:
#   - the hardware-board block is printed for primary_io with a note that
#     secondary_io mirrors it (the generator emits both; primary_io shapes
#     are checked by the table rows)
GENERATOR_ONLY_PREFIXES = ("/status/hardware/board/secondary_io/",)


def normalize_shape(path: str) -> str:
    return re.sub(r"/\d+(?=/|$)", "/#", path)


def guide_shapes(data_dir: pathlib.Path):
    per_edition = {}
    for f in sorted(data_dir.glob("*.paths")):
        shapes = set()
        for line in f.read_text().splitlines():
            path = line.split("\t")[0]
            shape = SHAPE_FIXUPS.get(normalize_shape(path), normalize_shape(path))
            shape = SHAPE_FIXUPS.get(shape, shape)
            if shape is None:
                continue
            shape = DEVICE_CORRECTIONS.get(shape, shape)
            shape = shape.replace("/redundant-listener/", "/redundant_listener/")
            shapes.add(shape)
        per_edition[f.name] = shapes
    return per_edition


def device_shapes(captures_dir: pathlib.Path):
    """Shapes the live device actually served (the committed live dump). A
    generator shape absent from the guide is legitimate iff the device
    serves it."""
    shapes = set()
    dump = captures_dir / "q1-live-dump.tsv"
    if dump.exists():
        for line in dump.read_text().splitlines():
            shapes.add(normalize_shape(line.split("\t")[0]))
    return shapes


def tool_shapes(tool: str):
    out = subprocess.run([tool], capture_output=True, text=True, check=True)
    shapes = set()
    count = 0
    for line in out.stdout.splitlines():
        path = line.split("\t")[0]
        shapes.add(normalize_shape(path))
        count += 1
    return shapes, count


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument(
        "--data",
        default=str(pathlib.Path(__file__).parent / "data"),
        help="directory of extracted .paths files",
    )
    ap.add_argument(
        "--captures",
        default=str(pathlib.Path(__file__).parents[2] / "captures"),
        help="directory of live-device captures (device-verified shapes)",
    )
    args = ap.parse_args()

    editions = guide_shapes(pathlib.Path(args.data))
    if not editions:
        print(f"no .paths files under {args.data}", file=sys.stderr)
        return 2

    names = sorted(editions)
    ok = True
    for a, b in zip(names, names[1:]):
        if editions[a] != editions[b]:
            ok = False
            for p in sorted(editions[a] ^ editions[b]):
                where = a if p in editions[a] else b
                print(f"EDITION MISMATCH: {p} only in {where}")

    generated, count = tool_shapes(args.tool)
    generated_vs_guide = {
        s for s in generated if not s.startswith(GENERATOR_ONLY_PREFIXES)
    }
    guide = editions[names[0]]
    device = device_shapes(pathlib.Path(args.captures))

    for p in sorted(guide - generated_vs_guide):
        ok = False
        print(f"MISSING FROM GENERATOR: {p}")
    for p in sorted(generated_vs_guide - guide - device):
        ok = False
        print(f"NOT IN GUIDE OR ON DEVICE: {p}")

    if ok:
        print(
            f"OK: {len(guide)} shapes agree across {len(names)} editions and the generator ({count} control points)"
        )
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
