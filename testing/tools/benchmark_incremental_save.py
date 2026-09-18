#!/usr/bin/env python3
# Copyright 2026 CloudPDF LTD
# SPDX-License-Identifier: Apache-2.0

"""Benchmark a file-backed incremental save after one square annotation edit.

The default sink counts and discards output, matching a native streaming-save
benchmark. --output writes the first result to a new file for independent
validation. Loading, editing, and closing are outside the measured save time.
"""

import argparse
import ctypes
import json
from pathlib import Path
import resource
import sys
import time


class Rect(ctypes.Structure):
    _fields_ = [(name, ctypes.c_float)
                for name in ("left", "top", "right", "bottom")]


class Writer(ctypes.Structure):
    pass


WriteBlock = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(Writer),
                              ctypes.c_void_p, ctypes.c_ulong)
Writer._fields_ = [("version", ctypes.c_int), ("WriteBlock", WriteBlock)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("input", type=Path)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--detached", action="store_true",
                        help="Leave an orphaned annotation appearance in memory")
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    if args.output and (args.output.exists() or
                        args.output.resolve() == args.input.resolve()):
        parser.error("--output must name a new file distinct from the input")

    library = ctypes.CDLL(str(args.library.resolve()))

    def bind(name, result, arguments):
        function = getattr(library, name)
        function.restype = result
        function.argtypes = arguments
        return function

    pointer = ctypes.c_void_p
    initialize = bind("FPDF_InitLibrary", None, [])
    shutdown = bind("FPDF_DestroyLibrary", None, [])
    load = bind("FPDF_LoadDocument", pointer,
                [ctypes.c_char_p, ctypes.c_char_p])
    close = bind("FPDF_CloseDocument", None, [pointer])
    load_page = bind("FPDF_LoadPage", pointer, [pointer, ctypes.c_int])
    close_page = bind("FPDF_ClosePage", None, [pointer])
    create = bind("FPDFPage_CreateAnnot", pointer, [pointer, ctypes.c_int])
    close_annotation = bind("FPDFPage_CloseAnnot", None, [pointer])
    set_rect = bind("FPDFAnnot_SetRect", ctypes.c_int,
                    [pointer, ctypes.POINTER(Rect)])
    save = bind("FPDF_SaveAsCopy", ctypes.c_int,
                [pointer, ctypes.POINTER(Writer), ctypes.c_ulong])

    initialize()
    document = None
    try:
        document = load(bytes(args.input.resolve()), None)
        if not document:
            raise RuntimeError("Could not open input")
        page = load_page(document, 0)
        if not page:
            raise RuntimeError("Could not load first page")
        try:
            annotation = create(page, 5)  # FPDF_ANNOT_SQUARE
            if not annotation:
                raise RuntimeError("Could not create annotation")
            try:
                if not set_rect(annotation,
                                ctypes.byref(Rect(20, 120, 120, 20))):
                    raise RuntimeError("Could not set annotation rectangle")
            finally:
                close_annotation(annotation)
            if args.detached:
                create_layer_annotation = bind("EPDFPage_CreateAnnot", pointer,
                                               [pointer, ctypes.c_int])
                generate = bind("EPDFAnnot_GenerateAppearance", ctypes.c_int,
                                [pointer])
                remove = bind("FPDFPage_RemoveAnnot", ctypes.c_int,
                              [pointer, ctypes.c_int])
                count = bind("FPDFPage_GetAnnotCount", ctypes.c_int, [pointer])
                index = count(page)
                temporary = create_layer_annotation(page, 5)
                if not temporary:
                    raise RuntimeError("Could not create temporary annotation")
                try:
                    if not set_rect(temporary,
                                    ctypes.byref(Rect(30, 130, 130, 30))):
                        raise RuntimeError("Could not set temporary rectangle")
                    if not generate(temporary):
                        raise RuntimeError("Could not generate appearance")
                finally:
                    close_annotation(temporary)
                if not remove(page, index):
                    raise RuntimeError("Could not detach temporary annotation")
        finally:
            close_page(page)

        for iteration in range(args.repeat):
            output = None
            if args.output and iteration == 0:
                output = args.output.open("xb")
            written = 0
            write_error = None

            @WriteBlock
            def write_block(_writer, data, size):
                nonlocal written, write_error
                try:
                    if output:
                        output.write(ctypes.string_at(data, size))
                    written += size
                    return 1
                except Exception as error:
                    write_error = error
                    return 0

            writer = Writer(1, write_block)
            try:
                started = time.perf_counter()
                success = save(document, ctypes.byref(writer), 1)
                elapsed = time.perf_counter() - started
                if not success or write_error:
                    raise RuntimeError("Incremental save failed") from write_error
                peak_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
                peak_bytes = peak_rss
                if sys.platform != "darwin":
                    peak_bytes *= 1024
                print(json.dumps({
                    "pass": iteration + 1,
                    "save_ms": round(elapsed * 1000, 2),
                    "output_bytes": written,
                    "peak_process_mib": round(peak_bytes / 1024**2, 2),
                    "sink": "file" if output else "counting",
                    "detached": args.detached,
                }), flush=True)
            finally:
                if output:
                    output.close()
    finally:
        if document:
            close(document)
        shutdown()


if __name__ == "__main__":
    main()
