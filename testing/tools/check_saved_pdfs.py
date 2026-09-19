#!/usr/bin/env python3
# Copyright 2026 CloudPDF LTD
# SPDX-License-Identifier: Apache-2.0

"""Check save-test outputs with qpdf via pikepdf, without repairing or saving.

Each .pdf has a .size sidecar (original prefix length, zero for a full rewrite)
and optionally a .pw sidecar containing its fixture password. Only full rewrites
must have no orphans: incremental output deliberately retains old revisions.
Install the pinned dependency from requirements-save-check.txt.
"""

import argparse
import json
from pathlib import Path
import re

import pikepdf


REFERENCE = re.compile(r"([0-9]+) ([0-9]+) R\Z")
INTEGER = re.compile(rb"[0-9]+\Z")
DELIMITERS = b"\x00\t\n\x0c\r ()<>[]{}/%"


class InvalidSavedPdf(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise InvalidSavedPdf(message)


def tokens(data, start):
    """Read PDF syntax outside strings; stop before any stream payload."""
    position = start
    while position < len(data):
        byte = data[position]
        if byte in b"\x00\t\n\x0c\r ":
            position += 1
            continue
        if byte == ord('%'):
            while position < len(data) and data[position] not in b"\r\n":
                position += 1
            continue
        if byte == ord('('):
            depth = 1
            position += 1
            while position < len(data) and depth:
                char = data[position]
                position += 1
                if char == ord('\\'):
                    position += 1
                elif char == ord('('):
                    depth += 1
                elif char == ord(')'):
                    depth -= 1
            require(depth == 0, "Unterminated literal string")
            yield b"STRING"
            continue
        if data[position:position + 2] in (b"<<", b">>"):
            yield data[position:position + 2]
            position += 2
            continue
        if byte == ord('<'):
            end = data.find(b'>', position + 1)
            require(end != -1, "Unterminated hex string")
            position = end + 1
            yield b"STRING"
            continue
        end = position + 1
        if byte not in b"[]{}>":
            while end < len(data) and data[end] not in DELIMITERS:
                end += 1
        token = data[position:end]
        position = end
        if token in (b"stream", b"endobj", b"startxref"):
            return
        yield token


def check_references(words, xref):
    previous = []
    for word in words:
        if word == b"R" and len(previous) == 2:
            identity = tuple(previous)
            require(identity in xref, f"Reference has missing identity {identity}")
        if INTEGER.fullmatch(word):
            previous = (previous + [int(word)])[-2:]
        else:
            previous = []


def json_references(value):
    if isinstance(value, str):
        match = REFERENCE.fullmatch(value)
        if match:
            yield tuple(map(int, match.groups()))
    elif isinstance(value, list):
        for item in value:
            yield from json_references(item)
    elif isinstance(value, dict):
        for item in value.values():
            yield from json_references(item)


def inspect_pdf(path, original_size=0, password=""):
    data = Path(path).read_bytes()
    require(0 <= original_size <= len(data), "Invalid original prefix length")
    try:
        document = pikepdf.open(path, password=password, attempt_recovery=False,
                               inherit_page_attributes=False)
    except pikepdf.PasswordError:
        # The public PDFium API accepts UTF-8 and converts legacy passwords to
        # PDFDocEncoding. qpdf's byte-password form accepts those encoded bytes.
        encoded = bytes(pikepdf.String(password))
        document = pikepdf.open(path, password=encoded, attempt_recovery=False,
                               inherit_page_attributes=False)
    with document as pdf:
        warnings = pdf.check_pdf_syntax()
        require(not warnings, f"qpdf warnings: {warnings}")
        xref = pdf.get_xref_table()
        objects = {
            identity: json.loads(pdf.get_object(identity).to_json(dereference=True))
            for identity in xref
        }
        for identity, entry in xref.items():
            if entry.type != 1:
                continue
            words = iter(tokens(data, entry.offset))
            expected = [str(identity[0]).encode(), str(identity[1]).encode(), b"obj"]
            actual = [next(words, None) for _ in range(3)]
            require(actual == expected, f"Object header disagrees with xref: {identity}")
            check_references(words, xref)

        starts = list(re.finditer(rb"startxref\s+([0-9]+)\s+%%EOF", data))
        require(starts, "Missing final startxref")
        start = int(starts[-1][1])
        if data[start:start + 4] == b"xref":
            words = iter(tokens(data, start))
            for word in words:
                if word == b"trailer":
                    break
            check_references(words, xref)
        trailer = json.loads(pdf.trailer.to_json())
        pending = list(json_references(trailer))
        reachable = set()
        while pending:
            identity = pending.pop()
            if identity in reachable:
                continue
            require(identity in objects, f"Unresolved trailer graph reference: {identity}")
            reachable.add(identity)
            pending.extend(json_references(objects[identity]))

        # A newly emitted xref stream is structural, rather than a trailer ref.
        current_xref = {
            identity for identity, entry in xref.items()
            if entry.type == 1 and entry.offset == start
        }
        reachable.update(current_xref)
        written = {
            identity for identity, entry in xref.items()
            if entry.type == 1 and entry.offset >= original_size
        }
        if original_size == 0:
            require(set(xref) == reachable,
                    f"Full rewrite has orphan objects: {set(xref) - reachable}")
            require(all(gen == 0 for _, gen in xref),
                    "Full rewrite did not normalize generations")
        else:
            require(written <= reachable,
                    f"Revision wrote orphan objects: {written - reachable}")

        encryption_dicts = {
            identity for identity, obj in objects.items()
            if isinstance(obj, dict) and obj.get('/Filter') == '/Standard'
            and ('/O' in obj or '/U' in obj)
        }
        if original_size == 0:
            expected = set(json_references(trailer.get('/Encrypt')))
            require(encryption_dicts == expected,
                    f"Unexpected encryption dictionaries: {encryption_dicts - expected}")
        return len(written)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    files = sorted(args.directory.glob('*.pdf'))
    require(files, f"No dumped PDFs in {args.directory}")
    for path in files:
        original_size = int(path.with_suffix('.size').read_text())
        password_file = path.with_suffix('.pw')
        password = password_file.read_text() if password_file.exists() else ''
        inspect_pdf(path, original_size, password)
    print(f"Independently checked {len(files)} saved PDFs without recovery")


if __name__ == '__main__':
    main()
