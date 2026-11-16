#!/usr/bin/env python3
"""Test data for tools/hosttest/fast_flash_test.c: ROM images, a stand-in kernel, packages made by
the reference packer (tools/fast_flash_package.py), and manifests that break one rule each.

Usage: python tools/hosttest/fast_flash_gen.py <out_dir>

Writes into <out_dir>:
  A.bin  the ECU's flash before the test     B.bin  the image to flash (blocks 3, 9, 15 differ)
  kernel.bin                                 a 6 KB stand-in kernel (the fake ECU never runs it)
  b.ffp/b.json  a.ffp/a.json                 packages of B and A
  b_*.json      manifest variants of b.ffp   b_corrupt.ffp/.json  a block-9 stream that is wrong
  m_*.json (+ m_*.ffp)                       one broken rule each; expected FFERR stage in cases.txt
"""

import json
import pathlib
import random
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import fast_flash_package as ffp  # noqa: E402

ROM = ffp.ROM_SIZE


def make_image(rng):
    img = bytearray(ROM)
    for chunk in range(0, ROM, 0x800):
        kind = rng.random()
        if kind < 0.30:
            img[chunk:chunk + 0x800] = rng.randbytes(0x800)
        elif kind < 0.55:
            pat = rng.randbytes(rng.choice((2, 4, 8, 16)))
            img[chunk:chunk + 0x800] = (pat * (0x800 // len(pat) + 1))[:0x800]
        elif kind < 0.75:
            img[chunk:chunk + 0x800] = b"\xff" * 0x800
        else:  # table-like: slowly varying 16-bit values
            v = rng.randrange(0, 4000)
            out = bytearray()
            while len(out) < 0x800:
                v = max(0, min(0xFFFF, v + rng.randrange(-20, 21)))
                out += v.to_bytes(2, "big")
            img[chunk:chunk + 0x800] = out
    # a blank stretch inside block 12, as real ROMs have
    img[0x90000:0x98000] = b"\xff" * 0x8000
    # flash counter page: counter 1, complement, "_WDS"
    page = bytearray(b"\xff" * 128)
    page[0:8] = b"\x00\x01\xff\xfe_WDS"
    img[0xFFB00:0xFFB80] = page
    # commit page: start address and entry pointer programmed
    img[0xFFF80:0x100000] = rng.randbytes(120) + b"\x00\x00\x40\x00" + b"\x00\x00\x20\x00"
    return img


def make_kernel(rng):
    k = bytearray(rng.randbytes(0x1800))
    for i in range(len(k) - 7):  # no stray marker tail anywhere
        if k[i:i + 7] == b"SBL_END":
            k[i] ^= 0xFF
    k[0x1800 - 0x40:0x1800 - 0x38] = b"*SBL_END"
    return bytes(k)


def refresh_crcs(pkg, man):
    man["package_len"] = len(pkg)
    man["package_crc32"] = zlib.crc32(pkg)
    return man


def write(out, name, data):
    (out / name).write_bytes(data)


def wjson(out, name, man):
    write(out, name, ffp.manifest_bytes(man))


def main():
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20261115)
    A = make_image(rng)
    B = bytearray(A)
    B[0x3100:0x3110] = rng.randbytes(16)                 # block 3
    B[0x2A000:0x2A800] = rng.randbytes(0x800)            # block 9
    B[0xE1234] ^= 0xFF                                    # block 15, first range
    B[0xFFC40:0xFFC48] = rng.randbytes(8)                 # block 15, second range
    kernel = make_kernel(rng)
    write(out, "A.bin", bytes(A))
    write(out, "B.bin", bytes(B))
    write(out, "kernel.bin", kernel)

    pkg_b, man_b = ffp.build(bytes(B), kernel)
    write(out, "b.ffp", pkg_b)
    wjson(out, "b.json", man_b)
    pkg_a, man_a = ffp.build(bytes(A), kernel)
    write(out, "a.ffp", pkg_a)
    wjson(out, "a.json", man_a)

    def variant(fn):
        m = json.loads(json.dumps(man_b))
        fn(m)
        return m

    wjson(out, "b_all.json", variant(lambda m: m.update(rewrite_all=True)))
    other_boot = bytearray(B[:0x2000])
    other_boot[0x100] ^= 0x55
    wjson(out, "b_boot.json", variant(lambda m: m["boot"].update(crc32=zlib.crc32(bytes(other_boot)))))
    wjson(out, "b_bootallow.json", variant(lambda m: m["boot"].update(crc32=zlib.crc32(bytes(other_boot)),
                                                                     policy="allow")))

    # a stream that is wrong but internally consistent (a packer bug): block 9
    pkg_c = bytearray(pkg_b)
    man_c = json.loads(json.dumps(man_b))
    r9 = man_c["blocks"][9 - 2]["ranges"][0]
    pkg_c[r9["off"] + r9["len"] // 2] ^= 0x20
    r9["crc32"] = zlib.crc32(bytes(pkg_c[r9["off"]:r9["off"] + r9["len"]]))
    refresh_crcs(bytes(pkg_c), man_c)
    write(out, "b_corrupt.ffp", bytes(pkg_c))
    wjson(out, "b_corrupt.json", man_c)

    cases = []  # (manifest, package, expected stage)

    def bad(name, fn, stage, pkg_name="b.ffp"):
        wjson(out, name + ".json", variant(fn))
        cases.append((name + ".json", pkg_name, stage))

    bad("m_low", lambda m: m["blocks"][0]["ranges"][0].update(addr=0x1000), 3)
    bad("m_counter", lambda m: m["blocks"][13]["ranges"][1].update(addr=0xFFB00, size=0x480), 3)
    bad("m_commitdata", lambda m: m["blocks"][13]["ranges"][1].update(size=0x480), 3)
    bad("m_missing", lambda m: m["blocks"].pop(5), 3)
    bad("m_dup", lambda m: m["blocks"].__setitem__(5, dict(m["blocks"][6])), 3)
    bad("m_ksize", lambda m: m["kernel"].update(len=6000), 3)
    bad("m_kver", lambda m: m["kernel"].update(version=2), 3)
    bad("m_piece", lambda m: m["lz"].update(piece=1023), 3)
    bad("m_lp", lambda m: m["lz"].update(lp=3), 3)
    bad("m_kind", lambda m: m.update(kind="other"), 3)
    bad("m_cmp", lambda m: m["blocks"][2]["cmp"][0].__setitem__(0, 0x4080), 3)
    bad("m_pre", lambda m: m["blocks"][13]["cmp_pre"][1].__setitem__(1, 0x480), 3)
    bad("m_nopre", lambda m: m["blocks"][13].pop("cmp_pre"), 3)
    bad("m_commitaddr", lambda m: m["commit"].update(addr=0xFFF00), 3)
    bad("m_policy", lambda m: m["boot"].update(policy="maybe"), 3)
    bad("m_streamoff", lambda m: m["blocks"][4]["ranges"][0].update(off=len(pkg_b) - 3), 3)
    bad("m_pkgcrc", lambda m: m.update(package_crc32=m["package_crc32"] ^ 1), 5)
    bad("m_seccrc", lambda m: m["blocks"][7]["ranges"][0].update(crc32=m["blocks"][7]["ranges"][0]["crc32"] ^ 1), 5)
    bad("m_kcrc", lambda m: m["kernel"].update(crc32=m["kernel"]["crc32"] ^ 1), 5)
    bad("m_ccrc", lambda m: m["commit"].update(crc32=m["commit"]["crc32"] ^ 1), 5)
    bad("m_pkglen", lambda m: m.update(package_len=m["package_len"] + 1), 4)

    # broken JSON
    write(out, "m_json.json", ffp.manifest_bytes(man_b)[:-20])
    cases.append(("m_json.json", "b.ffp", 2))
    write(out, "m_float.json", ffp.manifest_bytes(man_b).replace(b'"version":3', b'"version":3.0'))
    cases.append(("m_float.json", "b.ffp", 2))

    # package variants with consistent CRCs that still break a rule
    pkg_e = bytearray(pkg_b)
    man_e = json.loads(json.dumps(man_b))
    co = man_e["commit"]["off"]
    pkg_e[co + 124:co + 128] = b"\xff\xff\xff\xff"            # no entry pointer
    man_e["commit"]["crc32"] = zlib.crc32(bytes(pkg_e[co:co + 128]))
    refresh_crcs(bytes(pkg_e), man_e)
    write(out, "m_entry.ffp", bytes(pkg_e))
    wjson(out, "m_entry.json", man_e)
    cases.append(("m_entry.json", "m_entry.ffp", 3))

    pkg_k = bytearray(pkg_b)
    man_k = json.loads(json.dumps(man_b))
    pkg_k[0x10:0x18] = b"*SBL_END"                             # marker in the first block too
    man_k["kernel"]["crc32"] = zlib.crc32(bytes(pkg_k[0:0x1800]))
    refresh_crcs(bytes(pkg_k), man_k)
    write(out, "m_marker.ffp", bytes(pkg_k))
    wjson(out, "m_marker.json", man_k)
    cases.append(("m_marker.json", "m_marker.ffp", 3))

    # a commit page from another image, every CRC made consistent: the compare CRCs no longer chain
    pkg_x = bytearray(pkg_b)
    man_x = json.loads(json.dumps(man_b))
    co = man_x["commit"]["off"]
    pkg_x[co + 10] ^= 0x01
    man_x["commit"]["crc32"] = zlib.crc32(bytes(pkg_x[co:co + 128]))
    refresh_crcs(bytes(pkg_x), man_x)
    write(out, "m_commitmix.ffp", bytes(pkg_x))
    wjson(out, "m_commitmix.json", man_x)
    cases.append(("m_commitmix.json", "m_commitmix.ffp", 3))
    bad("m_premix", lambda m: m["blocks"][3].update(cmp_pre=[[m["blocks"][3]["cmp"][0][0], m["blocks"][3]["cmp"][0][1],
                                                             m["blocks"][3]["cmp"][0][2] ^ 1]]), 3)

    pkg_s = bytearray(pkg_b)
    man_s = json.loads(json.dumps(man_b))
    r = man_s["blocks"][3]["ranges"][0]
    pkg_s[r["off"]] = 0x5D                                      # a stream must start with 0
    r["crc32"] = zlib.crc32(bytes(pkg_s[r["off"]:r["off"] + r["len"]]))
    refresh_crcs(bytes(pkg_s), man_s)
    write(out, "m_stream0.ffp", bytes(pkg_s))
    wjson(out, "m_stream0.json", man_s)
    cases.append(("m_stream0.json", "m_stream0.ffp", 3))

    (out / "cases.txt").write_text("".join(f"{j} {p} {s}\n" for j, p, s in cases))
    print(f"fast flash test data in {out}: package {len(pkg_b)} bytes, {len(cases)} bad manifests")


if __name__ == "__main__":
    main()
