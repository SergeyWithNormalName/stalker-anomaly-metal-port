#!/usr/bin/env python3
"""DRAFT ONLY. Build a candidate patched copy; default mode writes nothing."""
from __future__ import annotations
import argparse
import hashlib
import json
import struct
from pathlib import Path

PINNED_EXE_SHA256 = "c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3"
ENTRY_RVA = 0x00CD5E60
ENTRY_EXPECTED = bytes.fromhex("48 89 5C 24 10")
CAVE_RVA = 0x00416F55
CAVE_EXPECTED = b"\xCC" * 27
PDB_FUNC_START = 0x00CD5E60
PDB_FUNC_END = 0x00CD61EF
UNWIND_RVA = 0x0150ADA0
UNWIND_HEADER = bytes.fromhex("01 1F 0D 00")


def u16(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise ValueError(f"u16 outside file at {offset:#x}")
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError(f"u32 outside file at {offset:#x}")
    return struct.unpack_from("<I", data, offset)[0]


def parse_pe(data: bytes):
    if data[:2] != b"MZ":
        raise ValueError("not a PE image")
    peoff = u32(data, 0x3C)
    if data[peoff:peoff + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    coff = peoff + 4
    if u16(data, coff) != 0x8664:
        raise ValueError("expected AMD64 image")
    nsections = u16(data, coff + 2)
    optional_size = u16(data, coff + 16)
    optional = coff + 20
    magic = u16(data, optional)
    if magic != 0x20B:
        raise ValueError("expected PE32+ image")
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    directories = optional + 112
    section_table = optional + optional_size
    sections = []
    for i in range(nsections):
        off = section_table + i * 40
        name = data[off:off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, off + 8)
        characteristics = u32(data, off + 36)
        sections.append({
            "name": name, "va": virtual_address,
            "virtual_size": virtual_size, "raw_size": raw_size,
            "raw_pointer": raw_pointer, "characteristics": characteristics,
        })

    def rva_to_offset(rva: int, size: int) -> tuple[int, dict]:
        if rva < 0 or size < 0:
            raise ValueError("negative RVA/size")
        for section in sections:
            start = section["va"]
            if start <= rva and rva + size <= start + max(section["virtual_size"], section["raw_size"]):
                delta = rva - start
                if delta + size > section["raw_size"]:
                    raise ValueError(f"RVA range {rva:#x}+{size:#x} is not file-backed")
                off = section["raw_pointer"] + delta
                if off < 0 or off + size > len(data):
                    raise ValueError(f"mapped file range {off:#x}+{size:#x} is out of bounds")
                return off, section
        raise ValueError(f"RVA range {rva:#x}+{size:#x} not in any section")

    def directory(index: int) -> tuple[int, int]:
        return struct.unpack_from("<II", data, directories + index * 8)

    return image_base, sections, rva_to_offset, directory


def rel32(from_instruction_rva: int, target_rva: int) -> bytes:
    displacement = target_rva - (from_instruction_rva + 5)
    if not -(1 << 31) <= displacement < (1 << 31):
        raise ValueError("rel32 target out of range")
    return struct.pack("<i", displacement)


def decode_rel32(from_instruction_rva: int, encoded: bytes) -> int:
    if len(encoded) != 4:
        raise ValueError("rel32 must be four bytes")
    return from_instruction_rva + 5 + struct.unpack("<i", encoded)[0]


def make_stub() -> bytes:
    # All branch displacements are calculated from labels, not copied literals.
    code = bytearray()
    code += bytes.fromhex("48 85 D2")                 # test rdx,rdx
    null_jcc = len(code); code += b"\x74\x00"       # jz empty
    code += bytes.fromhex("80 3A 00")                 # cmp byte ptr [rdx],0
    empty_jcc = len(code); code += b"\x74\x00"      # jz empty
    code += ENTRY_EXPECTED                              # displaced store RBX,[RSP+0x10]
    back_jmp = len(code); code += b"\xE9\0\0\0\0"  # jmp original+5
    empty_label = len(code)
    code += bytes.fromhex("31 C0 C3")                 # false return, no stack changes
    for branch in (null_jcc, empty_jcc):
        displacement = empty_label - (branch + 2)
        if not -128 <= displacement <= 127:
            raise ValueError("short conditional branch out of range")
        code[branch + 1] = displacement & 0xFF
    back_rva = CAVE_RVA + back_jmp
    code[back_jmp + 1:back_jmp + 5] = rel32(back_rva, ENTRY_RVA + len(ENTRY_EXPECTED))
    if decode_rel32(back_rva, code[back_jmp + 1:back_jmp + 5]) != ENTRY_RVA + 5:
        raise ValueError("resume branch does not target original entry + 5")
    if len(code) != 23 or len(code) > len(CAVE_EXPECTED):
        raise ValueError(f"stub length {len(code)} does not fit the pinned cave")
    return bytes(code)


def validate_pdata(data: bytes, directory, rva_to_offset) -> tuple[list[tuple[int, int]], int]:
    rva, size = directory(3)
    if not rva or size < 12 or size % 12:
        raise ValueError("missing or malformed x64 exception directory")
    off, _ = rva_to_offset(rva, size)
    entries = [struct.unpack_from("<III", data, pos) for pos in range(off, off + size, 12)]
    ranges = [(begin, end) for begin, end, _unwind in entries]
    matching = [(begin, end, unwind) for begin, end, unwind in entries if begin <= ENTRY_RVA < end]
    if matching != [(PDB_FUNC_START, PDB_FUNC_END, UNWIND_RVA)]:
        raise ValueError(f"unexpected Exists unwind entry: {matching!r}")
    if any(begin < CAVE_RVA + len(CAVE_EXPECTED) and CAVE_RVA < end for begin, end in ranges):
        raise ValueError("candidate cave overlaps a runtime-function range")
    unwind_off, _ = rva_to_offset(UNWIND_RVA, len(UNWIND_HEADER))
    if data[unwind_off:unwind_off + len(UNWIND_HEADER)] != UNWIND_HEADER:
        raise ValueError("pinned Exists UNWIND_INFO header mismatch")
    return ranges, size


def validate_relocations(data: bytes, directory, rva_to_offset, patch_ranges: list[tuple[int, int]]) -> tuple[int, int]:
    rva, size = directory(5)
    if not rva and not size:
        return 0, 0
    if not rva or size < 8:
        raise ValueError("malformed base-relocation directory")
    off, _ = rva_to_offset(rva, size)
    end = off + size
    targets = 0
    pos = off
    while pos < end:
        if pos + 8 > end:
            raise ValueError("truncated base-relocation block")
        page_rva, block_size = struct.unpack_from("<II", data, pos)
        if block_size < 8 or pos + block_size > end or (block_size - 8) % 2:
            raise ValueError("invalid base-relocation block size")
        for slot in range(pos + 8, pos + block_size, 2):
            item = u16(data, slot)
            kind, delta = item >> 12, item & 0x0FFF
            if kind == 0:  # IMAGE_REL_BASED_ABSOLUTE padding
                continue
            targets += 1
            target = page_rva + delta
            if any(start <= target < finish for start, finish in patch_ranges):
                raise ValueError(f"base relocation target overlaps patch at {target:#x}")
        pos += block_size
    if pos != end:
        raise ValueError("base-relocation parsing did not consume directory")
    return rva, targets


def changed_ranges(before: bytes, after: bytes) -> list[tuple[int, int]]:
    changed = [i for i, (a, b) in enumerate(zip(before, after)) if a != b]
    if len(before) != len(after):
        raise ValueError("patch changed file size")
    out = []
    for index in changed:
        if not out or index != out[-1][1]:
            out.append([index, index + 1])
        else:
            out[-1][1] = index + 1
    return [(a, b) for a, b in out]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", required=True, type=Path, help="Owned copy of the SHA-pinned original EXE")
    ap.add_argument("--output", required=True, type=Path, help="New isolated scratch destination; must not exist")
    ap.add_argument("--write-owned-copy", action="store_true", help="Explicitly write output + JSON receipt; omitted means dry plan only")
    args = ap.parse_args()
    source = args.input.resolve(strict=True)
    output = args.output.resolve()
    if source == output:
        raise SystemExit("refusing to target the source EXE")
    if output.exists():
        raise SystemExit("refusing existing output")
    expected_prefix = Path("scratch/native-silo-parity-20260929").resolve()
    if expected_prefix not in output.parents:
        raise SystemExit("output must remain under scratch/native-silo-parity-20260929")
    original = source.read_bytes()
    original_sha = hashlib.sha256(original).hexdigest()
    if original_sha != PINNED_EXE_SHA256:
        raise SystemExit(f"pinned EXE hash mismatch: {original_sha}")
    image_base, sections, rva_to_offset, directory = parse_pe(original)
    entry_off, entry_section = rva_to_offset(ENTRY_RVA, len(ENTRY_EXPECTED))
    cave_off, cave_section = rva_to_offset(CAVE_RVA, len(CAVE_EXPECTED))
    if entry_section["name"] != ".text" or cave_section["name"] != ".text":
        raise SystemExit("entry/cave are not both in .text")
    for section in (entry_section, cave_section):
        if section["characteristics"] & 0x60000000 != 0x60000000:
            raise SystemExit("entry/cave section is not read+execute")
    if original[entry_off:entry_off + 5] != ENTRY_EXPECTED:
        raise SystemExit("CModelPool::Exists entry bytes mismatch")
    if original[cave_off:cave_off + 27] != CAVE_EXPECTED:
        raise SystemExit("all-CC cave bytes mismatch")
    _ranges, pdata_size = validate_pdata(original, directory, rva_to_offset)
    stub = make_stub()
    entry_patch = b"\xE9" + rel32(ENTRY_RVA, CAVE_RVA)
    if decode_rel32(ENTRY_RVA, entry_patch[1:]) != CAVE_RVA:
        raise SystemExit("entry jump does not target cave")
    ranges = [(ENTRY_RVA, ENTRY_RVA + len(entry_patch)), (CAVE_RVA, CAVE_RVA + len(stub))]
    reloc_rva, reloc_targets = validate_relocations(original, directory, rva_to_offset, ranges)
    patched = bytearray(original)
    patched[entry_off:entry_off + len(entry_patch)] = entry_patch
    patched[cave_off:cave_off + len(stub)] = stub
    differences = changed_ranges(original, bytes(patched))
    expected_differences = sorted([(entry_off, entry_off + len(entry_patch)), (cave_off, cave_off + len(stub))])
    if differences != expected_differences:
        raise SystemExit(f"expected exactly two modified file ranges, got {differences!r}")
    patched_sha = hashlib.sha256(patched).hexdigest()
    receipt = {
        "status": "planned" if not args.write_owned_copy else "written",
        "original_sha256": original_sha,
        "patched_sha256": patched_sha,
        "image_base": hex(image_base),
        "entry": {"rva": hex(ENTRY_RVA), "file_offset": hex(entry_off), "before": ENTRY_EXPECTED.hex(), "after": entry_patch.hex()},
        "cave": {"rva": hex(CAVE_RVA), "file_offset": hex(cave_off), "before": CAVE_EXPECTED.hex(), "after": stub.hex(), "unused_bytes_after": 4},
        "resume_rva": hex(ENTRY_RVA + 5),
        "modified_file_ranges": [{"start": hex(a), "end_exclusive": hex(b), "length": b-a} for a,b in differences],
        "pdata_size": pdata_size,
        "base_relocation_directory_rva": hex(reloc_rva),
        "base_relocation_targets_checked": reloc_targets,
        "pdb_public_symbol_range_check": "verified separately in compatibility report",
        "output": str(output),
    }
    if args.write_owned_copy:
        receipt_path = Path(str(output) + ".patch-receipt.json")
        if receipt_path.exists():
            raise SystemExit("refusing existing receipt")
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("xb") as f:
            f.write(patched)
        with receipt_path.open("x", encoding="utf-8") as f:
            json.dump(receipt, f, indent=2, sort_keys=True)
            f.write("\n")
    print(json.dumps(receipt, indent=2, sort_keys=True))

if __name__ == "__main__":
    main()
