#!/usr/bin/env python3
"""Guard the AutoTLM One partition table's two immovable offsets.

The esp32 core hardcodes 0xe000 (boot_app0) and 0x10000 (app) as LITERALS in
its upload / merge-bin / program recipes. A table that relocates `otadata` or
`app0` makes every arduino-cli/IDE upload write the sketch into whatever
partition sits at 0x10000 — the bootloader then finds no bootable slot and the
unit boot-loops. Boards 0.3.0 shipped exactly that and bricked the bench unit
on its first flash.

Compile-only CI cannot catch it (it only appears on a real upload), so this
check stands in for the flash: it is pure CSV arithmetic and runs anywhere.

Usage: python3 verify-partitions.py [path/to/autotlm_ota.csv]
"""
import sys
from pathlib import Path

# (name, required offset) — the two the esp32 tooling hardcodes.
REQUIRED_OFFSETS = {"otadata": 0xE000, "app0": 0x10000}
FLASH_SIZE = 0x400000  # 4 MB


def parse(csv_path):
    rows = []
    # Explicit utf-8: the CSV header carries non-ASCII, and Python on Windows
    # would otherwise default to cp1252 and blow up.
    for raw in csv_path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) < 5:
            sys.exit(f"FAIL: malformed row: {raw!r}")
        name, ptype, subtype, offset, size = cols[:5]
        rows.append(
            {
                "name": name,
                "type": ptype,
                "subtype": subtype,
                "offset": int(offset, 0),
                "size": int(size, 0),
            }
        )
    return rows


def main():
    csv_path = Path(
        sys.argv[1]
        if len(sys.argv) > 1
        else Path(__file__).parent / "autotlm" / "esp32" / "tools" / "partitions" / "autotlm_ota.csv"
    )
    if not csv_path.is_file():
        sys.exit(f"FAIL: no such file: {csv_path}")

    rows = parse(csv_path)
    by_name = {r["name"]: r for r in rows}
    errors = []

    # 1. The immovable offsets.
    for name, want in REQUIRED_OFFSETS.items():
        row = by_name.get(name)
        if row is None:
            errors.append(f"partition '{name}' is missing (required at {want:#x})")
        elif row["offset"] != want:
            errors.append(
                f"'{name}' is at {row['offset']:#x} but the esp32 upload/merge-bin/program "
                f"recipes hardcode {want:#x} - this WILL brick uploads (see boards 0.3.0)"
            )

    # 2. Two OTA app slots, and identically sized (an OTA image must fit either).
    app0, app1 = by_name.get("app0"), by_name.get("app1")
    if not app1:
        errors.append("no 'app1' slot — the owner's ruling reserves TWO OTA slots")
    elif app0 and app0["size"] != app1["size"]:
        errors.append(
            f"OTA slots differ: app0 {app0['size']:#x} vs app1 {app1['size']:#x} "
            "- an image that fits one must fit the other"
        )

    # 3. No overlaps, and nothing past the end of flash.
    for a, b in zip(sorted(rows, key=lambda r: r["offset"]), sorted(rows, key=lambda r: r["offset"])[1:]):
        if a["offset"] + a["size"] > b["offset"]:
            errors.append(
                f"'{a['name']}' ({a['offset']:#x}+{a['size']:#x}) overlaps "
                f"'{b['name']}' ({b['offset']:#x})"
            )
    for r in rows:
        if r["offset"] + r["size"] > FLASH_SIZE:
            errors.append(f"'{r['name']}' runs past {FLASH_SIZE:#x} (4 MB flash)")

    if errors:
        print(f"FAIL: {csv_path}")
        for e in errors:
            print(f"  - {e}")
        sys.exit(1)

    print(f"OK: {csv_path}")
    for r in rows:
        print(f"  {r['name']:<9} {r['offset']:#08x}  {r['size']:#08x}")
    if app0:
        print(f"  OTA slot: {app0['size'] // 1024} KB x2")


if __name__ == "__main__":
    main()
