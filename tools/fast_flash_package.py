#!/usr/bin/env python3
"""Reference packer for the WiCAN fast flash ('K' command): <stem>.ffp + <stem>.json.

This is the specification NC Flash ports (its packer must produce byte-identical streams for the
same inputs; Python's lzma is deterministic for a given liblzma version). The firmware side is
main/fast_flash_logic.c; the wire contract and the manifest schema are in
docs/internals/fast_flash.md.

What it builds, from a checksum-corrected 1 MB ROM image and the fast-flash kernel binary:

  package (<stem>.ffp)  = kernel | one raw LZMA1 stream per program range | commit page (128 B)
  manifest (<stem>.json) = where each section sits, its CRC-32, and the CRC-32 the ECU must report
                           for every compare range before and after the flash

Every block 2..15 is packed: the PC cannot know which blocks differ until the ECU reports its CRCs.
The ranges are exactly the ones nc-rom-editor src/ecu/fast_flash.py uses (compare_ranges,
_lz_ranges) and the streams are made exactly like its lzma_stream(): raw LZMA1, lc 0, lp 2, pb 2,
dictionary 1 MB, preset 9 | extreme, no header.

Usage:
    python tools/fast_flash_package.py --image rom.bin --kernel kernel.bin --out build/LF9VEB_tune
        [--boot-policy refuse|allow] [--rewrite-all] [--cal-id ID] [--generation NC2]

No third-party dependencies.
"""

import argparse
import json
import lzma
import pathlib
import sys
import zlib

ROM_SIZE = 0x100000
BLOCK_STARTS = (
    0x00000, 0x01000, 0x02000, 0x03000, 0x04000, 0x05000, 0x06000, 0x07000,
    0x08000, 0x20000, 0x40000, 0x60000, 0x80000, 0xA0000, 0xC0000, 0xE0000, 0x100000,
)
FIRST_APP_BLOCK = 2
LAST_BLOCK = 15
APP_START = 0x2000
FLASH_COUNTER_OFFSET = 0xFFB00
COUNTER_PAGE_SIZE = 128
COMMIT_PAGE = 0xFFF80
COMMIT_PAGE_SIZE = 128
ENTRY_POINTER = 0xFFFFC
KERNEL_BLOCK = 0x400
KERNEL_MAX_SIZE = 0x3000
KERNEL_MARKER = b"*SBL_END"
KERNEL_VERSION = 3
LZ_LP = 2
LZ_PB = 2
LZ_PIECE = 1022
MANIFEST_MAX = 8192


class PackageError(ValueError):
    pass


def block_range(block):
    return BLOCK_STARTS[block], BLOCK_STARTS[block + 1] - BLOCK_STARTS[block]


def compare_ranges(block, with_commit=True):
    """fast_flash.py compare_ranges(): the counter page is never compared."""
    start, size = block_range(block)
    if block != LAST_BLOCK:
        return [(start, size)]
    end = start + size if with_commit else COMMIT_PAGE
    after = FLASH_COUNTER_OFFSET + COUNTER_PAGE_SIZE
    return [(start, FLASH_COUNTER_OFFSET - start), (after, end - after)]


def lz_ranges(block):
    """fast_flash.py _lz_ranges(): block 15 around the counter page, without the commit page."""
    start, size = block_range(block)
    if block != LAST_BLOCK:
        return [(start, size)]
    after = FLASH_COUNTER_OFFSET + COUNTER_PAGE_SIZE
    return [(start, FLASH_COUNTER_OFFSET - start), (after, COMMIT_PAGE - after)]


def lzma_stream(data):
    """fast_flash.py lzma_stream(): raw LZMA1, lc 0, lp 2, pb 2, no header."""
    filters = [{
        "id": lzma.FILTER_LZMA1,
        "preset": 9 | lzma.PRESET_EXTREME,
        "lc": 0,
        "lp": LZ_LP,
        "pb": LZ_PB,
        "dict_size": 1 << 20,
    }]
    return lzma.compress(data, format=lzma.FORMAT_RAW, filters=filters)


def validate_kernel(kernel):
    """fast_flash.py validate_kernel()."""
    if not kernel or len(kernel) % KERNEL_BLOCK or len(kernel) > KERNEL_MAX_SIZE:
        raise PackageError(f"kernel size 0x{len(kernel):X} is not a multiple of 0x400 up to 0x3000")
    if KERNEL_MARKER not in kernel[-KERNEL_BLOCK:] or KERNEL_MARKER[1:] in kernel[:-KERNEL_BLOCK]:
        raise PackageError("kernel must carry its end marker in the last block only")


def build(image, kernel, boot_policy="refuse", rewrite_all=False, cal_id="", generation="",
          kernel_version=KERNEL_VERSION):
    """Return (package bytes, manifest dict)."""
    if len(image) != ROM_SIZE:
        raise PackageError(f"image must be {ROM_SIZE} bytes, got {len(image)}")
    if image[ENTRY_POINTER:ENTRY_POINTER + 4] == b"\xff" * 4:
        raise PackageError("this ROM has no entry pointer; it would never start")
    if boot_policy not in ("refuse", "allow"):
        raise PackageError("boot policy must be 'refuse' or 'allow'")
    validate_kernel(kernel)

    pkg = bytearray()
    kernel_off = len(pkg)
    pkg += kernel
    blocks = []
    for block in range(FIRST_APP_BLOCK, LAST_BLOCK + 1):
        entry = {
            "block": block,
            "cmp": [[a, n, zlib.crc32(image[a:a + n])] for a, n in compare_ranges(block, True)],
        }
        if block == LAST_BLOCK:
            entry["cmp_pre"] = [[a, n, zlib.crc32(image[a:a + n])] for a, n in compare_ranges(block, False)]
        ranges = []
        for a, n in lz_ranges(block):
            stream = lzma_stream(image[a:a + n])
            ranges.append({"addr": a, "size": n, "off": len(pkg), "len": len(stream),
                           "crc32": zlib.crc32(stream)})
            pkg += stream
        entry["ranges"] = ranges
        blocks.append(entry)
    commit_off = len(pkg)
    page = image[COMMIT_PAGE:COMMIT_PAGE + COMMIT_PAGE_SIZE]
    pkg += page

    manifest = {
        "kind": "nc_fast_flash",
        "manifest_version": 1,
        "package_len": len(pkg),
        "package_crc32": zlib.crc32(pkg),
        "kernel": {"off": kernel_off, "len": len(kernel), "crc32": zlib.crc32(kernel),
                   "version": kernel_version},
        "lz": {"lp": LZ_LP, "pb": LZ_PB, "piece": LZ_PIECE},
        "blocks": blocks,
        "boot": {"crc32": zlib.crc32(image[:APP_START]), "policy": boot_policy},
        "commit": {"addr": COMMIT_PAGE, "off": commit_off, "len": COMMIT_PAGE_SIZE,
                   "crc32": zlib.crc32(page)},
        "rewrite_all": bool(rewrite_all),
        "cal_id": cal_id,
        "generation": generation,
    }
    if len(manifest_bytes(manifest)) > MANIFEST_MAX:
        raise PackageError("manifest larger than the firmware accepts (8192 bytes)")
    return bytes(pkg), manifest


def manifest_bytes(manifest):
    return json.dumps(manifest, separators=(",", ":")).encode("ascii")


def write(out_stem, pkg, manifest):
    out = pathlib.Path(out_stem)
    out.parent.mkdir(parents=True, exist_ok=True)
    ffp = out.with_name(out.name + ".ffp")
    js = out.with_name(out.name + ".json")
    ffp.write_bytes(pkg)
    js.write_bytes(manifest_bytes(manifest))
    return ffp, js


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--image", required=True, help="checksum-corrected 1 MB ROM")
    ap.add_argument("--kernel", required=True, help="fast-flash kernel binary (v3)")
    ap.add_argument("--out", required=True, help="output stem: writes <stem>.ffp and <stem>.json")
    ap.add_argument("--boot-policy", default="refuse", choices=("refuse", "allow"))
    ap.add_argument("--rewrite-all", action="store_true")
    ap.add_argument("--cal-id", default="")
    ap.add_argument("--generation", default="")
    a = ap.parse_args(argv)
    try:
        pkg, man = build(pathlib.Path(a.image).read_bytes(), pathlib.Path(a.kernel).read_bytes(),
                         a.boot_policy, a.rewrite_all, a.cal_id, a.generation)
    except PackageError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    ffp, js = write(a.out, pkg, man)
    print(f"{ffp} {len(pkg)} bytes crc32 {man['package_crc32']:08x}; {js}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
