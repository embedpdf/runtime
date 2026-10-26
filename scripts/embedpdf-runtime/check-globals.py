#!/usr/bin/env python3
# Copyright 2026 CloudPDF LTD
# SPDX-License-Identifier: Apache-2.0

"""Check a built runtime library's writable globals against the allowlist.

The native server runs PDFium on several threads in one process, each with its
own copy of PDFium's globals (EPDF_TLS; docs/embedpdf/fork-ledger.md, rule
thread-confined-globals). A writable global that is not thread-local is shared
by every thread. This lists the library's writable data symbols, split into
shared (.data, .bss, common) and thread-local (.tdata, .tbss; __thread_* on
macOS), and compares them with scripts/embedpdf-runtime/globals-allowlist.txt.

It fails when:
  - a shared global matches no allowlist entry (upstream or we added one);
  - a global the allowlist marks thread-local is shared now (an EPDF_TLS lost
    in a merge).
It warns when an entry matches nothing, so the list doesn't go stale.

Run it on the unstripped library a native build writes, before packaging:
  check-globals.py out/embedpdf-runtime/<target>/libembedpdf.{so,dylib}

Allowlist lines are `<category> | <pattern> | <reason>`. The pattern is a glob
(fnmatch) over demangled names. Categories:
  thread-local  must stay thread-local
  atomic        an atomic, safe to share
  test-only     a switch only tests write
  set-once      written once at start-up, or a function-local static that is
                read-only once initialized
  third-party   shared by design in a third-party library
"""

import argparse
import fnmatch
import os
import re
import subprocess
import sys

SOURCE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
ALLOWLIST = os.path.join(SOURCE_DIR, 'scripts', 'embedpdf-runtime',
                         'globals-allowlist.txt')
OBJDUMP = os.path.join(SOURCE_DIR, 'third_party', 'llvm-build',
                       'Release+Asserts', 'bin', 'llvm-objdump')
CATEGORIES = ('thread-local', 'atomic', 'test-only', 'set-once', 'third-party')

SHARED_SECTIONS = ('.data', '.bss', '*COM*', '__DATA,__data', '__DATA,__bss',
                   '__DATA,__common')
# On macOS a thread-local variable's name is on its descriptor in
# __thread_vars; __thread_bss and __thread_data hold `$tlv$init` copies.
THREAD_LOCAL_SECTIONS = ('.tdata', '.tbss', '__DATA,__thread_vars')

# `<address> <flags> O <section> [<size>] [.hidden] <name>`; ELF has the size.
# ELF thread-local symbols (STT_TLS) have no `O` flag: `<address> l .tbss ...`.
SYMBOL = re.compile(r'^[0-9a-f]+\s.*?\s(?:O\s+(\S+)|(\.tbss|\.tdata))\s+'
                    r'(?:[0-9a-f]{8,}\s+)?(?:\.hidden\s+)?(.+)$')


def normalize(name, macho):
  name = re.sub(r' \(\.\d+\)$', '', name)  # `foo (.0)`: a compiler clone
  name = re.sub(r'\.llvm\.\d+$', '', name)
  # Mach-O prefixes C names with `_`; demangled C++ names have no prefix.
  if macho and name.startswith('_') and '::' not in name and '(' not in name:
    name = name[1:]
  return name


def read_symbols(library):
  out = subprocess.run([OBJDUMP, '--syms', '--demangle', library],
                       check=True,
                       capture_output=True,
                       text=True,
                       errors='replace').stdout
  macho = '__DATA,' in out
  shared, thread_local = set(), set()
  for line in out.splitlines():
    m = SYMBOL.match(line)
    if not m:
      continue
    section = m.group(1) or m.group(2)
    name = normalize(m.group(3).strip(), macho)
    if name == section:  # the section's own symbol
      continue
    if section in SHARED_SECTIONS:
      shared.add(name)
    elif section in THREAD_LOCAL_SECTIONS:
      thread_local.add(name)
  return shared, thread_local


def read_allowlist():
  entries = []
  with open(ALLOWLIST, encoding='utf-8') as f:
    for number, line in enumerate(f, 1):
      line = line.strip()
      if not line or line.startswith('#'):
        continue
      fields = [field.strip() for field in line.split(' | ')]
      if len(fields) != 3 or fields[0] not in CATEGORIES or not fields[2]:
        sys.exit(f'{ALLOWLIST}:{number}: expected `<category> | <pattern> | '
                 f'<reason>` with a category from {", ".join(CATEGORIES)}')
      entries.append(tuple(fields))
  return entries


def main():
  parser = argparse.ArgumentParser(
      description=__doc__.split('\n\n')[0],
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('library')
  args = parser.parse_args()

  shared, thread_local = read_symbols(args.library)
  entries = read_allowlist()
  print(f'{args.library}: {len(shared)} shared writable globals, '
        f'{len(thread_local)} thread-local')

  used = set()
  errors = []
  for name in sorted(shared):
    hits = [e for e in entries if fnmatch.fnmatchcase(name, e[1])]
    used.update(hits)
    if not hits:
      errors.append(f'shared and not on the allowlist: {name}')
    elif any(e[0] == 'thread-local' for e in hits):
      errors.append(f'must be thread-local, but is shared: {name}')
  for name in thread_local:
    used.update(e for e in entries if fnmatch.fnmatchcase(name, e[1]))

  for entry in entries:
    if entry not in used:
      print(f'warning: allowlist entry matches nothing: {entry[0]} | '
            f'{entry[1]}')
  for error in errors:
    print(f'error: {error}')
  if errors:
    print(f'{len(errors)} error(s). A new shared global needs EPDF_TLS, an '
          'atomic, or an allowlist entry with its reason (rule '
          'thread-confined-globals).')
    sys.exit(1)
  print('ok')


if __name__ == '__main__':
  main()
