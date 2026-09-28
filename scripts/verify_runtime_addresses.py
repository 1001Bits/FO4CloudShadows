"""Verify built-in RVAs against private game images and migration databases.

Developer-only, Windows, Python standard library. Address Library is an optional
verification input here; it is neither packaged nor read by the plugin.
"""

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import struct


# Original desktop relocation IDs, retained as independent migration evidence.
# The last field identifies executable code rather than a global data address.
SYMBOLS = {
    "kRendererDataSymbol": (1235449, 2704429, False),
    "kGraphicsStateSymbol": (600795, 2704621, False),
    "kRenderTargetManagerSymbol": (1508457, 2666735, False),
    "kSkySingletonFunctionSymbol": (484694, 2192448, True),
    "kWorldRootCameraFunctionSymbol": (384264, 2228956, True),
    "kPlayerSingletonSymbol": (412034, 4798212, False),
    "kTESSingletonSymbol": (1194835, 2698044, False),
    "kBeginTechniqueSymbol": (1041640, 2318876, True),
    "kVRSetShadersSymbol": (None, None, True),
    "kUIMessageQueueSingletonSymbol": (82123, 4796377, False),
    "kUIMessageQueueAddMessageSymbol": (1182019, 2284929, True),
    "kStringPoolGetEntrySymbol": (507142, 2268729, True),
    "kWaterReflectionUpdateSymbol": (907876, 2213919, True),
}
VR_UNUSED = {
    "kUIMessageQueueSingletonSymbol",
    "kUIMessageQueueAddMessageSymbol",
    "kStringPoolGetEntrySymbol",
}
# NG keeps the same renderer/function IDs as AE. These two singleton IDs
# differ; both slots were independently checked in the NG executable.
NG_IDS = {"kPlayerSingletonSymbol": 2698073, "kUIMessageQueueSingletonSymbol": 2689091}
RUNTIMES = (("og", (1, 10, 163, 0)), ("ae", (1, 11, 240, 0)),
            ("vr", (1, 2, 72, 0)), ("ng", (1, 10, 984, 0)),
            ("ae221", (1, 11, 221, 0)))


def symbol_id(name, column):
    if column == 2:
        return None
    if column == 3 and name in NG_IDS:
        return NG_IDS[name]
    return SYMBOLS[name][0 if column == 0 else 1]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def file_version(path):
    api = ctypes.WinDLL("version", use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32)]
    api.GetFileVersionInfoSizeW.restype = ctypes.c_uint32
    api.GetFileVersionInfoW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    api.VerQueryValueW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_uint32)]
    size = api.GetFileVersionInfoSizeW(str(path), None)
    require(size > 0, f"No version resource: {path}")
    buffer = ctypes.create_string_buffer(size)
    require(api.GetFileVersionInfoW(str(path), 0, size, buffer), "Cannot read version resource")
    pointer, length = ctypes.c_void_p(), ctypes.c_uint32()
    require(api.VerQueryValueW(buffer, "\\", ctypes.byref(pointer), ctypes.byref(length)), "Cannot query file version")
    require(length.value >= 52, "Truncated fixed version info")
    fields = ctypes.cast(pointer, ctypes.POINTER(ctypes.c_uint32 * 13)).contents
    require(fields[0] == 0xFEEF04BD, "Invalid fixed version info")
    return (fields[2] >> 16, fields[2] & 0xFFFF, fields[3] >> 16, fields[3] & 0xFFFF)


def database(path, wanted):
    data = path.read_bytes()
    count, = struct.unpack_from("<Q", data)
    require(len(data) == 8 + count * 16, f"Unexpected database format: {path}")
    matches = {}
    for identity, rva in struct.iter_unpack("<QQ", memoryview(data)[8:]):
        if identity in wanted:
            require(identity not in matches, f"Duplicate address ID {identity}")
            matches[identity] = rva
    require(set(matches) == wanted, f"Missing IDs in {path}: {wanted - set(matches)}")
    return matches, hashlib.sha256(data).hexdigest()


def verify_image(path, expected_version, table, column, addresses):
    require(file_version(path) == expected_version, f"Unexpected executable version: {path}")
    image = path.read_bytes()
    require(image[:2] == b"MZ", "Invalid DOS header")
    pe, = struct.unpack_from("<I", image, 0x3C)
    require(image[pe:pe + 4] == b"PE\0\0", "Invalid PE signature")
    machine, section_count = struct.unpack_from("<HH", image, pe + 4)
    optional_size, = struct.unpack_from("<H", image, pe + 20)
    optional = pe + 24
    magic, = struct.unpack_from("<H", image, optional)
    require(machine == 0x8664 and magic == 0x20B, "Expected x64 PE32+ image")
    image_size, = struct.unpack_from("<I", image, optional + 56)
    sections = []
    for index in range(section_count):
        start = optional + optional_size + index * 40
        name = image[start:start + 8].rstrip(b"\0").decode("ascii")
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from("<IIII", image, start + 8)
        flags, = struct.unpack_from("<I", image, start + 36)
        require(raw_offset + raw_size <= len(image), f"Truncated section {name}")
        sections.append((name, rva, max(virtual_size, raw_size), raw_offset, raw_size, flags))
    checked = []
    for name, (og_id, ae_id, code) in SYMBOLS.items():
        rva = table[name][column]
        identity = symbol_id(name, column)
        unused = name in VR_UNUSED if column == 2 else identity is None
        if unused:
            require(rva == 0, f"{name}: unsupported runtime must have zero RVA")
            continue
        if identity is not None:
            require(rva == addresses[identity], f"{name}: RVA differs from original ID {identity}")
        byte_count = 16 if code else 8
        require(0 < rva and rva + byte_count <= image_size, f"{name}: outside image")
        matches = [section for section in sections if section[1] <= rva and rva + byte_count <= section[1] + section[2]]
        require(len(matches) == 1, f"{name}: ambiguous/missing PE section")
        section, base, _, raw_offset, raw_size, flags = matches[0]
        require(flags & 0x40000000, f"{name}: section is not readable")
        require(bool(flags & 0x20000000) == code, f"{name}: unexpected section executable flag")
        if code:
            relative = rva - base
            require(relative + byte_count <= raw_size, f"{name}: code is not file-backed")
            entry = image[raw_offset + relative:raw_offset + relative + byte_count]
            require(entry not in (b"\0" * byte_count, b"\xCC" * byte_count), f"{name}: empty code entry")
        checked.append({"symbol": name, "id": identity, "rva": f"0x{rva:08X}", "section": section})
    return {"version": ".".join(map(str, expected_version)), "exe_sha256": hashlib.sha256(image).hexdigest(), "symbols": checked}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parent.parent)
    for runtime, _ in RUNTIMES:
        parser.add_argument(f"--{runtime}-exe", type=Path, required=True)
        if runtime != "vr":
            parser.add_argument(f"--{runtime}-addresses", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    header = (args.source_root / "src/RuntimeAddresses.h").read_text(encoding="utf-8")
    number = r"(0x[0-9A-Fa-f]+|[0-9]+)"
    columns = r"\s*,\s*".join([number] * len(RUNTIMES))
    pattern = rf"inline constexpr RuntimeRVA (\w+)\s*\{{\s*{columns}\s*\}};"
    entries = re.findall(pattern, header)
    table = {name: tuple(int(value, 0) for value in values) for name, *values in entries}
    require(len(entries) == len(table) and set(table) == set(SYMBOLS), "Runtime address table has missing, duplicate or unreviewed symbols")
    results = {}
    for column, (runtime, version) in enumerate(RUNTIMES):
        addresses, digest = {}, None
        if runtime != "vr":
            wanted = {symbol_id(name, column) for name in SYMBOLS} - {None}
            addresses, digest = database(getattr(args, f"{runtime}_addresses"), wanted)
        result = verify_image(getattr(args, f"{runtime}_exe"), version, table, column, addresses)
        if digest:
            result["verification_database_sha256"] = digest
        results[runtime] = result
        print(f"{runtime.upper()}: {len(result['symbols'])} addresses verified for {result['version']}")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
