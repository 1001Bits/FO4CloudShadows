#!/usr/bin/env python3
"""Extract validated DXBC containers embedded in a Bethesda .fxp package.

The tool deliberately knows nothing about Fallout shader names.  It validates
the standard DXBC container header/chunk table, classifies the shader stage
from the SHDR/SHEX version token, and writes a deterministic manifest.  This
keeps runtime-port inspection reproducible without treating every occurrence
of the four-byte DXBC magic as trusted input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


STAGES = {
    0: "ps",
    1: "vs",
    2: "gs",
    3: "hs",
    4: "ds",
    5: "cs",
}


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def inspect_container(data: bytes, start: int) -> tuple[int, str, int] | None:
    if start < 0 or start + 32 > len(data) or data[start : start + 4] != b"DXBC":
        return None
    version = u32(data, start + 20)
    total = u32(data, start + 24)
    chunk_count = u32(data, start + 28)
    if version != 1 or total < 32 or total > 16 * 1024 * 1024:
        return None
    if start + total > len(data) or chunk_count == 0 or chunk_count > 64:
        return None
    table_end = start + 32 + chunk_count * 4
    if table_end > start + total:
        return None

    stage = "unknown"
    model = 0
    ranges: list[tuple[int, int]] = []
    for index in range(chunk_count):
        relative = u32(data, start + 32 + index * 4)
        chunk = start + relative
        if relative % 4 != 0 or chunk < table_end or chunk + 8 > start + total:
            return None
        size = u32(data, chunk + 4)
        end = chunk + 8 + size
        if end > start + total:
            return None
        ranges.append((chunk, end))
        fourcc = data[chunk : chunk + 4]
        if fourcc in (b"SHDR", b"SHEX") and size >= 4:
            token = u32(data, chunk + 8)
            stage = STAGES.get((token >> 16) & 0xFFFF, "unknown")
            model = token & 0xFFFF

    ranges.sort()
    if any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
        return None
    return total, stage, model


def extract(
    source: Path, destination: Path, write_containers: bool = True
) -> dict[str, object]:
    data = source.read_bytes()
    destination.mkdir(parents=True, exist_ok=True)
    records: list[dict[str, object]] = []
    cursor = 0
    ordinal = 0
    while True:
        start = data.find(b"DXBC", cursor)
        if start < 0:
            break
        cursor = start + 4
        inspected = inspect_container(data, start)
        if inspected is None:
            continue
        total, stage, model = inspected
        blob = data[start : start + total]
        digest = hashlib.sha256(blob).hexdigest().upper()
        name = f"{ordinal:06d}_{start:010X}_{stage}_{digest[:16]}.dxbc"
        if write_containers:
            (destination / name).write_bytes(blob)
        prefix_start = max(0, start - 76)
        records.append(
            {
                "ordinal": ordinal,
                "offset": start,
                "length": total,
                "stage": stage,
                "shader_model_token": model,
                "sha256": digest,
                "file": name,
                "record_prefix_hex": data[prefix_start:start].hex().upper(),
            }
        )
        ordinal += 1
        cursor = start + total

    counts: dict[str, int] = {}
    for record in records:
        stage = str(record["stage"])
        counts[stage] = counts.get(stage, 0) + 1
    manifest = {
        "schema_version": 1,
        "source": str(source.resolve()),
        "source_sha256": hashlib.sha256(data).hexdigest().upper(),
        "source_size": len(data),
        "container_count": len(records),
        "stage_counts": counts,
        "records": records,
    }
    (destination / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument(
        "--manifest-only",
        action="store_true",
        help="validate and index containers without writing each DXBC file",
    )
    args = parser.parse_args()
    manifest = extract(
        args.source, args.destination, write_containers=not args.manifest_only
    )
    print(
        f"Extracted {manifest['container_count']} validated DXBC containers: "
        f"{manifest['stage_counts']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
