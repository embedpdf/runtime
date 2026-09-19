#!/usr/bin/env python3
# Copyright 2026 CloudPDF LTD
# SPDX-License-Identifier: Apache-2.0

import tempfile
from pathlib import Path
import unittest

import pikepdf

from check_saved_pdfs import InvalidSavedPdf, inspect_pdf, tokens


def fixture(orphan=False, custom_root=False, bad_reference=False):
    objects = [
        b'<</Type/Catalog/Pages 2 0 R>>',
        b'<</Type/Pages/Count 1/Kids[3 0 R]>>',
        b'<</Type/Page/Parent 2 0 R/MediaBox[0 0 100 100]/Resources<<>>>>',
    ]
    if bad_reference:
        objects[0] = objects[0].replace(b'2 0 R', b'2 1 R')
    if orphan or custom_root:
        objects.append(b'<</Filter/Standard/O(secret-owner)/U(secret-user)>>')
    data = bytearray(b'%PDF-1.7\n')
    offsets = []
    for number, body in enumerate(objects, 1):
        offsets.append(len(data))
        data.extend(f'{number} 0 obj\n'.encode() + body + b'\nendobj\n')
    start = len(data)
    data.extend(f'xref\n0 {len(objects) + 1}\n0000000000 65535 f\r\n'.encode())
    for offset in offsets:
        data.extend(f'{offset:010} 00000 n\r\n'.encode())
    extra = b'/Custom 4 0 R' if custom_root else b''
    data.extend(f'trailer\n<</Root 1 0 R/Size {len(objects) + 1}'.encode() + extra +
                f'>>\nstartxref\n{start}\n%%EOF\n'.encode())
    return bytes(data), start


class SavedPdfCheckerTest(unittest.TestCase):
    def check(self, data, original_size=0):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.pdf'
            path.write_bytes(data)
            return inspect_pdf(path, original_size)

    def test_valid_file(self):
        self.assertEqual(3, self.check(fixture()[0]))

    def test_wrong_reference_generation_fails(self):
        with self.assertRaises((InvalidSavedPdf, pikepdf.PdfError)):
            self.check(fixture(bad_reference=True)[0])

    def test_extra_encryption_dictionary_fails(self):
        with self.assertRaisesRegex(InvalidSavedPdf, 'orphan'):
            self.check(fixture(orphan=True)[0])

    def test_wrong_xref_offset_fails_without_recovery(self):
        data = fixture()[0].replace(b'0000000009 00000 n', b'0000000010 00000 n')
        with self.assertRaises((InvalidSavedPdf, pikepdf.PdfError)):
            self.check(data)

    def test_missing_xref_entry_fails(self):
        data, start = fixture()
        lines = data[start:].splitlines(keepends=True)
        lines[4] = b'0000000000 00000 f\r\n'  # object 2 (/Pages)
        with self.assertRaises((InvalidSavedPdf, pikepdf.PdfError)):
            self.check(data[:start] + b''.join(lines))

    def test_custom_trailer_roots_are_followed(self):
        # This object is ordinary application data, not an encryption dictionary.
        data = fixture(custom_root=True)[0].replace(b'/Filter/Standard', b'/Filter/Custom  ')
        self.assertEqual(4, self.check(data))

    def test_incremental_output_may_keep_old_orphans(self):
        base, previous = fixture(orphan=True)
        revision = b'3 0 obj\n<</Type/Page/Parent 2 0 R/MediaBox[0 0 100 100]/Resources<<>>/Rotate 90>>\nendobj\n'
        start = len(base) + len(revision)
        revision += (f'xref\n3 1\n{len(base):010} 00000 n\r\n'
                     f'trailer\n<</Root 1 0 R/Size 5/Prev {previous}>>\n'
                     f'startxref\n{start}\n%%EOF\n').encode()
        self.assertEqual(1, self.check(base + revision, len(base)))

    def test_strings_and_stream_payloads_are_not_references(self):
        data = b'1 0 obj <</A (nested \\( 9 5 R (endobj)) /B <3920352052> /C 2 0 R>>stream\n99 2 R'
        words = list(tokens(data, 0))
        self.assertEqual(1, words.count(b'R'))
        self.assertNotIn(b'99', words)


if __name__ == '__main__':
    unittest.main()
