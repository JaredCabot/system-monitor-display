#!/usr/bin/env python3
"""Writes lang/en.json from the English in systemmonitordisplay.c.

Every string the settings window, its messages and the tray menu say is
written as T(L"...") or N_(L"...") in the code. This collects them, in the
order they first appear, as the keys of lang/en.json, which is the list a
translation is checked against (--check-translations) and the file a new
translation starts from.

    python lang/sync_english.py          writes lang/en.json
    python lang/sync_english.py --check  exits 1 if lang/en.json is out of date
"""
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
SOURCE = HERE.parent / "systemmonitordisplay.c"
TARGET = HERE / "en.json"

# T( or N_( then one or more wide literals, which C joins into one.
CALL = re.compile(r'\b(?:T|N_)\(\s*((?:L"(?:[^"\\]|\\.)*"\s*)+)\)')
PIECE = re.compile(r'L"((?:[^"\\]|\\.)*)"')
ESCAPES = {"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'"}


def unescape(text):
    """A C literal's body as the text it stands for."""
    out, i = [], 0
    while i < len(text):
        if text[i] != "\\":
            out.append(text[i])
            i += 1
        elif text[i + 1] == "u":
            out.append(chr(int(text[i + 2:i + 6], 16)))
            i += 6
        else:
            out.append(ESCAPES[text[i + 1]])
            i += 2
    return "".join(out)


def english():
    source = SOURCE.read_text(encoding="utf-8")
    seen = {}
    for call in CALL.finditer(source):
        text = "".join(unescape(p) for p in PIECE.findall(call.group(1)))
        if text:
            seen.setdefault(text, "")
    return seen


def main():
    catalogue = {"_meta": {"name": "English", "native": "English"}}
    catalogue.update(english())
    text = json.dumps(catalogue, ensure_ascii=False, indent=2) + "\n"
    if "--check" in sys.argv:
        old = TARGET.read_text(encoding="utf-8") if TARGET.exists() else ""
        if old != text:
            print("lang/en.json is out of date: run python lang/sync_english.py")
            return 1
        return 0
    TARGET.write_text(text, encoding="utf-8", newline="\n")
    print(f"lang/en.json: {len(catalogue) - 1} strings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
