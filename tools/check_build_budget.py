#!/usr/bin/env python3
"""Ratchet on compiler warnings and app-image size.

Two things degrade silently on a device you cannot attach a console to: warnings
pile up until nobody reads them, and the image creeps toward the partition
ceiling until an OTA fails in the field. Both are numbers, so both can be gated.

Neither check tries to be clever. Each compares against a committed baseline in
tools/build_baseline.json and fails when the number gets worse:

  warnings   Counted only for this repo's own code (main/, components/).
             Warnings from ESP-IDF and managed_components/ are reported but not
             gated -- we cannot fix them and their count moves with the IDF
             version, which would make the gate noise.
  image size Fails if the app image grows more than GROWTH_TOLERANCE past the
             baseline, and separately if it uses more than MAX_PARTITION_USE of
             the app partition. Firmware is supposed to grow when you add a
             feature; the point is that growth is *declared* (bump the baseline
             in the same PR) rather than discovered later.

When a number legitimately changes, update the baseline and commit it:

    python tools/check_build_budget.py --log build.log --update

Usage:
    python tools/check_build_budget.py --log build.log
    python tools/check_build_budget.py --log build.log --image build/foo.bin

No third-party dependencies.
"""

import argparse
import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
BASELINE = REPO / "tools" / "build_baseline.json"
BUILD = REPO / "build"

# An image may grow this much past the baseline before the gate demands an
# explicit baseline bump. Sized to absorb ordinary churn, not a new subsystem.
GROWTH_TOLERANCE = 32 * 1024
# Independently of the baseline, never let the image exceed this share of the
# app partition -- an OTA needs the *other* app slot free, and a full slot is a
# field failure, not a build failure.
MAX_PARTITION_USE = 0.85

# gcc/clang: path:line:col: warning: message [-Wflag]
WARNING = re.compile(r"^(.+?):(\d+):(?:\d+:)?\s+warning:\s+(.*)$")

OURS = ("/main/", "\\main\\", "/components/", "\\components\\")
NOT_OURS = ("managed_components", "/build/", "\\build\\")


def parse_warnings(log_text: str) -> tuple:
    """(ours, external) -- deduplicated warning identities.

    A warning in a header is re-emitted once per translation unit that includes
    it. Dedup on (file, line, message) so one defect counts once.
    """
    ours, external = set(), set()
    for line in log_text.splitlines():
        m = WARNING.match(line.strip())
        if not m:
            continue
        path, lineno, message = m.group(1), m.group(2), m.group(3)
        norm = path.replace("\\", "/")
        identity = (pathlib.PurePath(norm).name, lineno, message)
        if any(s in norm for s in NOT_OURS):
            external.add(identity)
        elif any(s in path or s in norm for s in OURS):
            ours.add(identity)
        else:
            external.add(identity)
    return ours, external


def app_partition_bytes() -> int | None:
    """Size of the app partition named by the sdkconfig-selected CSV."""
    sdkconfig = REPO / "sdkconfig"
    if not sdkconfig.is_file():
        return None
    m = re.search(r'^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="(.+)"$',
                  sdkconfig.read_text(encoding="utf-8", errors="replace"),
                  re.MULTILINE)
    if not m:
        return None
    csv = REPO / m.group(1)
    if not csv.is_file():
        return None
    for row in csv.read_text(encoding="utf-8", errors="replace").splitlines():
        if row.lstrip().startswith("#"):
            continue
        cols = [c.strip() for c in row.split(",")]
        if len(cols) < 5 or cols[1] != "app":
            continue
        size = cols[4]
        if size.lower().startswith("0x"):
            return int(size, 16)
        if size.upper().endswith("K"):
            return int(size[:-1]) * 1024
        if size.upper().endswith("M"):
            return int(size[:-1]) * 1024 * 1024
        if size.isdigit():
            return int(size)
    return None


def find_app_image(explicit: str | None) -> pathlib.Path | None:
    """The app image this build produced.

    build/ accumulates images from every past build, so never glob for it --
    read the name the build itself recorded.
    """
    if explicit:
        p = pathlib.Path(explicit)
        return p if p.is_file() else None
    desc = BUILD / "project_description.json"
    if desc.is_file():
        try:
            name = json.loads(desc.read_text(encoding="utf-8")).get("app_bin")
        except (ValueError, OSError):
            name = None
        if name and (BUILD / name).is_file():
            return BUILD / name
    flasher = BUILD / "flasher_args.json"
    if flasher.is_file():
        try:
            data = json.loads(flasher.read_text(encoding="utf-8"))
        except (ValueError, OSError):
            return None
        for offset, name in (data.get("flash_files") or {}).items():
            if int(offset, 16) == 0x10000:
                cand = BUILD / pathlib.PurePath(name).name
                if cand.is_file():
                    return cand
    return None


def load_baseline() -> dict:
    if not BASELINE.is_file():
        return {}
    try:
        return json.loads(BASELINE.read_text(encoding="utf-8"))
    except ValueError:
        return {}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, help="captured build output")
    parser.add_argument("--image", help="app image (default: read from build/)")
    parser.add_argument("--update", action="store_true",
                        help="rewrite the baseline from this build and exit 0")
    args = parser.parse_args()

    log_path = pathlib.Path(args.log)
    if not log_path.is_file():
        sys.exit(f"error: missing build log {log_path}")

    ours, external = parse_warnings(
        log_path.read_text(encoding="utf-8", errors="replace"))
    image = find_app_image(args.image)
    partition = app_partition_bytes()
    baseline = load_baseline()

    if args.update:
        BASELINE.write_text(json.dumps({
            "_comment": "Ratchet baselines checked by tools/check_build_budget.py. "
                        "Bump in the same PR that legitimately changes a number.",
            "warnings": len(ours),
            "app_image_bytes": image.stat().st_size if image else 0,
        }, indent=2) + "\n", encoding="utf-8")
        print(f"baseline updated: warnings={len(ours)} "
              f"app_image_bytes={image.stat().st_size if image else 0}")
        sys.exit(0)

    failed = False

    # --- warnings -----------------------------------------------------------
    limit = baseline.get("warnings")
    if limit is None:
        print(f"FAIL warnings: {len(ours)} in our code, but no baseline to "
              f"compare against. Run with --update and commit "
              f"tools/build_baseline.json")
        failed = True
    elif len(ours) > limit:
        failed = True
        # The baseline stores a count, not identities -- storing identities would
        # churn the file on every line-number shift. So this lists everything and
        # says so, rather than claiming to know which ones are new.
        print(f"FAIL warnings: {len(ours)} in our code, baseline is {limit} "
              f"(+{len(ours) - limit}). Full list follows; the baseline tracks "
              f"the count only, so diff against your branch point to find yours:")
        for name, line, msg in sorted(ours)[:60]:
            print(f"    - {name}:{line}: {msg}")
        if len(ours) > 60:
            print(f"    ... and {len(ours) - 60} more")
    else:
        note = ""
        if len(ours) < limit:
            note = (f"  <-- improved; tighten the ratchet with "
                    f"`python tools/check_build_budget.py --log <log> --update`")
        print(f"ok  warnings: {len(ours)} in our code (baseline {limit}){note}")
    print(f"    ({len(external)} more in ESP-IDF / managed_components, not gated)")

    # --- image size ---------------------------------------------------------
    if image is None:
        print("FAIL image size: could not locate the app image "
              "(no build/project_description.json?)")
        failed = True
    else:
        size = image.stat().st_size
        base = baseline.get("app_image_bytes")
        print(f"    app image: {image.name} = {size:,} B")
        if base:
            delta = size - base
            if delta > GROWTH_TOLERANCE:
                failed = True
                print(f"FAIL image size: grew {delta:,} B past the baseline "
                      f"({base:,} B), over the {GROWTH_TOLERANCE:,} B tolerance. "
                      f"If this growth is intended, rerun with --update and "
                      f"commit the baseline.")
            else:
                print(f"ok  image size: {delta:+,} B vs baseline "
                      f"(tolerance {GROWTH_TOLERANCE:,} B)")
        else:
            print("FAIL image size: no baseline. Run with --update and commit "
                  "tools/build_baseline.json")
            failed = True
        if partition:
            use = size / partition
            if use > MAX_PARTITION_USE:
                failed = True
                print(f"FAIL partition headroom: image uses {use:.1%} of the "
                      f"{partition:,} B app partition (ceiling "
                      f"{MAX_PARTITION_USE:.0%})")
            else:
                print(f"ok  partition headroom: {use:.1%} of {partition:,} B "
                      f"used (ceiling {MAX_PARTITION_USE:.0%})")

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
