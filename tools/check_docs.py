#!/usr/bin/env python3
"""Keep docs/internals/ from drifting out of sync with the code.

The dominant way these docs rot is not prose going subtly out of date -- it is a
symbol getting renamed or deleted while the doc keeps citing the old name. That
failure is mechanical, so a machine can catch it. This does.

Three checks:

  1. Every file path cited in an evergreen doc exists, and if the citation
     carries a line number (`config_server.c:1363`), the file is at least that
     long.
  2. Every function `symbol()` and every ALL_CAPS constant cited in an evergreen
     doc still appears somewhere in the source tree. Symbols that legitimately
     live outside this repo (ESP-IDF, libc) are listed in EXTERNAL_SYMBOLS and
     skipped -- the list is explicit so every exemption stays visible.
  3. docs/internals/README.md indexes every evergreen doc, and every doc it
     indexes exists.

Two classes of doc, and only one is checked:

  - Evergreen (architecture.md, poll_log.md, ...) describe how the code works
    now. They must track the code, so they are checked.
  - Dated snapshots (audit-2026-07.md, rewrite-vs-evolve-2026-07.md) are
    historical records of what was true on a date. They are *supposed* to go
    stale and are never updated, so they are skipped. The date suffix in the
    filename is what marks them.

What this does NOT catch: prose that describes behavior the code no longer has,
while still citing symbols that exist. Nothing automated catches that. The
README rule -- if a doc contradicts the code, fix the doc in the same PR --
still has to be enforced by whoever reviews.

Usage:
    python tools/check_docs.py            # exit 1 on any failure
    python tools/check_docs.py --list     # dump what was extracted, then exit 0

No third-party dependencies.
"""

import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
DOCS = REPO / "docs" / "internals"
INDEX = DOCS / "README.md"

# Where a cited symbol may be defined. Everything else in the tree (build
# output, vendored components, the docs themselves) is not searched.
SOURCE_DIRS = ("main", "components", "tools", ".github")
SOURCE_FILES = ("sdkconfig", "CMakeLists.txt", "partitions.csv")
SOURCE_SUFFIXES = {
    ".c", ".h", ".cpp", ".hpp", ".py", ".mjs", ".js", ".html", ".css",
    ".yml", ".yaml", ".json", ".csv", ".txt", ".cmake", "",
}
SKIP_DIRS = {"build", "managed_components", "node_modules", ".git", "dist"}

# A doc whose name ends in -YYYY-MM is a dated snapshot: a record of what was
# true then, deliberately never updated. Skipped by checks 1-3.
DATED_DOC = re.compile(r"-\d{4}-\d{2}$")

# Citations that are real but point outside this repo. Every entry carries the
# reason it is exempt, so an exemption can never quietly become a hiding place
# for a symbol that actually did get deleted. Keep both lists short.
EXTERNAL_SYMBOLS = {
    # FreeRTOS / ESP-IDF types and macros, named when explaining a portability
    # trap. Defined in the IDF install, not here.
    "StackType_t": "ESP-IDF (FreeRTOS)",
    "portSTACK_TYPE": "ESP-IDF (FreeRTOS)",
    "configSTACK_DEPTH_TYPE": "ESP-IDF (FreeRTOS)",
    # libc, named when describing an idiom.
    "strlcpy": "libc",
    "strncpy": "libc",
    "snprintf": "libc",
    "malloc": "libc",
    "calloc": "libc",
    "free": "libc",
    "memset": "libc",
    # The sibling desktop project (../nc-rom-editor), which speaks the same
    # protocol. Not vendored, so CI cannot see it.
    "read_rom_id": "nc-rom-editor (src/ecu/protocol.py)",
}

EXTERNAL_PATHS = {
    # ESP-IDF install tree.
    "freertos/FreeRTOS-Kernel/include/freertos/task.h": "ESP-IDF",
    "portable/xtensa/include/freertos/portmacro.h": "ESP-IDF",
    # The sibling desktop project (../nc-rom-editor).
    "constants.py": "nc-rom-editor",
    "src/ecu/constants.py": "nc-rom-editor",
    # Files that exist on the device or the user's SD card at runtime, not in
    # the repo. Paths with a leading '/' are skipped automatically; these are
    # the ones cited as bare basenames.
    "config.json": "device LittleFS",
    "auto_pid.json": "device LittleFS",
    "logcfg.txt": "user-supplied SD card file",
}

CODE_FENCE = re.compile(r"```.*?```", re.DOTALL)
INLINE_CODE = re.compile(r"`([^`\n]+)`")

# A backtick span is treated as a file citation when it looks like a path with a
# known source suffix, optionally followed by :LINE or :LINE-LINE.
FILE_CITATION = re.compile(
    r"^([\w./+-]+\.(?:c|h|cpp|hpp|py|mjs|js|html|css|yml|yaml|json|csv|md))"
    r"(?::(\d+)(?:-\d+)?)?$"
)
# `foo()` / `fooBar()` -- a function citation.
FUNC_CITATION = re.compile(r"^([A-Za-z_$][\w$]*)\(\)$")
# ALL_CAPS_WITH_UNDERSCORE -- a constant/macro citation. The underscore and the
# length floor keep bare words like `HTTP` or `JSON` out.
CONST_CITATION = re.compile(r"^([A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+)$")


def evergreen_docs() -> list:
    """Docs that must track the code, in sorted order. Excludes the index."""
    return sorted(
        p for p in DOCS.glob("*.md")
        if p.name != "README.md" and not DATED_DOC.search(p.stem)
    )


def dated_docs() -> list:
    return sorted(p for p in DOCS.glob("*.md") if DATED_DOC.search(p.stem))


def source_files():
    """Every file a cited symbol could plausibly be defined in."""
    seen = []
    for name in SOURCE_FILES:
        p = REPO / name
        if p.is_file():
            seen.append(p)
    for d in SOURCE_DIRS:
        root = REPO / d
        if not root.is_dir():
            continue
        for p in root.rglob("*"):
            if not p.is_file():
                continue
            if any(part in SKIP_DIRS for part in p.parts):
                continue
            if p.suffix.lower() in SOURCE_SUFFIXES:
                seen.append(p)
    return seen


def identifier_index() -> set:
    """Every identifier appearing anywhere in the source tree."""
    idents = set()
    word = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
    for p in source_files():
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        idents.update(word.findall(text))
    return idents


def citations(md: str):
    """(files, symbols) cited in inline code spans, ignoring fenced blocks.

    Fenced blocks are skipped because they hold shell transcripts and code
    samples, where a bare word is not a claim about this repo's source.
    """
    prose = CODE_FENCE.sub(" ", md)
    files, symbols = {}, set()
    for span in INLINE_CODE.findall(prose):
        span = span.strip()
        m = FILE_CITATION.match(span)
        if m:
            path, line = m.group(1), m.group(2)
            files.setdefault(path, set()).add(int(line) if line else 0)
            continue
        m = FUNC_CITATION.match(span) or CONST_CITATION.match(span)
        if m:
            symbols.add(m.group(1))
    return files, symbols


def resolve(path: str, corpus: list) -> pathlib.Path | None:
    """Docs cite paths loosely -- repo-relative, or bare basenames like
    `main.js`. Accept a repo-relative hit first, then a unique basename match."""
    direct = REPO / path
    if direct.is_file():
        return direct
    hits = [p for p in corpus if p.name == pathlib.PurePath(path).name]
    if len(hits) == 1:
        return hits[0]
    # An ambiguous basename still proves the file exists somewhere; treat the
    # citation as satisfied but decline to verify the line number.
    return hits[0] if hits else None


def check_files(docs: list) -> list:
    # Built once: rglob per citation would be quadratic, and docs cross-link to
    # each other, so the docs directory is part of the corpus too.
    corpus = source_files() + sorted(DOCS.glob("*.md"))
    failures = []
    for doc in docs:
        files, _ = citations(doc.read_text(encoding="utf-8", errors="replace"))
        for path, lines in sorted(files.items()):
            # A leading '/' means a path on the device filesystem, which by
            # construction cannot be a repo file.
            if path.startswith("/") or path in EXTERNAL_PATHS:
                continue
            resolved = resolve(path, corpus)
            if resolved is None:
                failures.append(f"{doc.name}: cites `{path}`, which does not exist")
                continue
            longest = max(lines)
            if longest:
                n = len(resolved.read_text(
                    encoding="utf-8", errors="replace").splitlines())
                if longest > n:
                    failures.append(
                        f"{doc.name}: cites `{path}:{longest}` but the file is "
                        f"only {n} lines")
    return failures


def check_symbols(docs: list, idents: set) -> list:
    failures = []
    for doc in docs:
        _, symbols = citations(doc.read_text(encoding="utf-8", errors="replace"))
        for sym in sorted(symbols):
            if sym in EXTERNAL_SYMBOLS or sym in idents:
                continue
            failures.append(
                f"{doc.name}: cites `{sym}()`, which is nowhere in the source "
                f"tree -- renamed, deleted, or it belongs in EXTERNAL_SYMBOLS")
    return failures


def check_index(docs: list) -> list:
    failures = []
    if not INDEX.is_file():
        return [f"missing {INDEX.relative_to(REPO)}"]
    index = INDEX.read_text(encoding="utf-8", errors="replace")
    linked = set(re.findall(r"\(([\w.-]+\.md)\)", index))
    for doc in docs + dated_docs():
        if doc.name not in linked:
            failures.append(
                f"README.md does not index {doc.name} -- add it to the table")
    for name in sorted(linked):
        if not (DOCS / name).is_file():
            failures.append(f"README.md links {name}, which does not exist")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--list", action="store_true",
        help="print every extracted citation and exit 0 (for tuning)")
    args = parser.parse_args()

    if not DOCS.is_dir():
        sys.exit(f"error: missing {DOCS}")

    docs = evergreen_docs()
    skipped = dated_docs()

    if args.list:
        for doc in docs:
            files, symbols = citations(
                doc.read_text(encoding="utf-8", errors="replace"))
            print(f"\n== {doc.name}")
            for path, lines in sorted(files.items()):
                print(f"   file   {path} {sorted(lines) if any(lines) else ''}")
            for sym in sorted(symbols):
                print(f"   symbol {sym}")
        sys.exit(0)

    idents = identifier_index()
    failed = False

    for label, failures in (
        ("check 1: cited files exist and are long enough",
         check_files(docs)),
        ("check 2: cited symbols still exist in the source tree",
         check_symbols(docs, idents)),
        ("check 3: README.md indexes every doc",
         check_index(docs)),
    ):
        if failures:
            failed = True
            print(f"FAIL {label}:")
            for f in failures:
                print(f"    - {f}")
        else:
            print(f"ok  {label}")

    print(f"\nchecked {len(docs)} evergreen doc(s): "
          f"{', '.join(d.name for d in docs) or '(none)'}")
    if skipped:
        print(f"skipped {len(skipped)} dated snapshot(s): "
              f"{', '.join(d.name for d in skipped)}")

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
