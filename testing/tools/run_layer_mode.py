#!/usr/bin/env python3
# Copyright 2026 CloudPDF LTD
# SPDX-License-Identifier: Apache-2.0

"""Run the embedder tests through EmbedPDF layer documents and compare.

Layer mode (testing/embedpdf_layer_mode.h; docs/embedpdf/fork-ledger.md, rule
layer-mode) opens every test document as a fresh layer over a frozen base, so
upstream's own tests exercise the layer's read and write paths.

This runs the suite once as usual, to know which tests fail anyway, then runs
every test again in layer mode, each in its own process (a failed DCHECK
aborts the whole binary). It reports:

  - tests that fail only in layer mode and match no pattern in
    testing/embedpdf_layer_mode_expectations.txt (`<glob> <reason>` lines);
  - tests that leave objects in a layer and aren't in
    testing/embedpdf_layer_mode_writers.txt: a test that only reads must leave
    none, so a new one is either a new writing test or a read path that writes
    (rule reads-dont-write);
  - entries in either file that no longer apply (warnings).

Usage:
  run_layer_mode.py <build_dir> [--filter GTEST_FILTER] [--jobs N]
                    [--out DIR] [--record-writers]

  --record-writers  rewrite the writers file from this run (review the diff);
                    the comment lines at its top are kept.
Exit status 1 when anything is reported as an error.
"""

import argparse
import concurrent.futures
import fnmatch
import json
import os
import re
import subprocess
import sys

SOURCE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
EXPECTATIONS = os.path.join(SOURCE_DIR, 'testing',
                            'embedpdf_layer_mode_expectations.txt')
WRITERS = os.path.join(SOURCE_DIR, 'testing', 'embedpdf_layer_mode_writers.txt')
TEST_TIMEOUT_SECONDS = 300
FAILURE_LINE = re.compile(r'(Check failed|Assertion failed|Assertion .* failed|'
                          r'FATAL|Failure$|\[  FAILED  \])')


def list_tests(binary, gtest_filter):
  out = subprocess.run([binary, '--gtest_list_tests',
                        f'--gtest_filter={gtest_filter}'],
                       check=True,
                       capture_output=True,
                       text=True).stdout
  tests, suite = [], None
  for line in out.splitlines():
    if not line.strip():
      continue
    if not line.startswith(' '):
      suite = line.split('#')[0].strip()
    else:
      tests.append(suite + line.split('#')[0].strip())
  return tests


def run_plain(binary, gtest_filter, out_dir):
  report = os.path.join(out_dir, 'plain.json')
  subprocess.run([binary, f'--gtest_filter={gtest_filter}',
                  f'--gtest_output=json:{report}'],
                 capture_output=True,
                 text=True)
  if not os.path.exists(report):
    sys.exit('The plain run wrote no report: did it crash? Run it by hand.')
  with open(report) as f:
    data = json.load(f)
  failed = set()
  for suite in data.get('testsuites', []):
    for test in suite.get('testsuite', []):
      if test.get('failures'):
        failed.add(f"{suite['name']}.{test['name']}")
  return failed


def run_one_layered(binary, test, out_dir):
  objects_file = os.path.join(out_dir, 'objects', test.replace('/', '_'))
  env = dict(os.environ,
             EPDF_EMBEDDER_LAYER_MODE='1',
             EPDF_EMBEDDER_LAYER_MODE_REPORT=objects_file)
  try:
    result = subprocess.run([binary, f'--gtest_filter={test}'],
                            capture_output=True,
                            text=True,
                            errors='replace',
                            env=env,
                            timeout=TEST_TIMEOUT_SECONDS)
    output = result.stdout + result.stderr
    if result.returncode < 0 or 'Check failed' in output or (
        'Assertion failed' in output):
      status = 'crash'
    elif result.returncode != 0:
      status = 'fail'
    else:
      status = 'pass'
  except subprocess.TimeoutExpired:
    output, status = '', 'timeout'
  objects = 0
  if os.path.exists(objects_file):
    with open(objects_file) as f:
      objects = max((int(line.rsplit(' ', 1)[1]) for line in f if line.strip()),
                    default=0)
  reason = next((line.strip() for line in output.splitlines()
                 if FAILURE_LINE.search(line)), '')
  with open(os.path.join(out_dir, 'logs', test.replace('/', '_')), 'w') as f:
    f.write(output)
  return test, status, objects, reason


def read_list(path):
  entries = {}
  if os.path.exists(path):
    with open(path) as f:
      for line in f:
        line = line.strip()
        if line and not line.startswith('#'):
          name, _, reason = line.partition(' ')
          entries[name] = reason.strip()
  return entries


def read_header(path):
  """The comment lines at the top of a list file, kept when it is rewritten."""
  header = []
  if os.path.exists(path):
    with open(path) as f:
      for line in f:
        if not line.startswith('#'):
          break
        header.append(line)
  return header


def main():
  parser = argparse.ArgumentParser(
      description=__doc__.split('\n\n')[0],
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('build_dir')
  parser.add_argument('--filter', default='*')
  parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
  parser.add_argument('--out', default=None)
  parser.add_argument('--record-writers', action='store_true')
  args = parser.parse_args()

  binary = os.path.join(args.build_dir, 'pdfium_embeddertests')
  out_dir = args.out or os.path.join(args.build_dir, 'layer-mode')
  for sub in ('objects', 'logs'):
    os.makedirs(os.path.join(out_dir, sub), exist_ok=True)
  for name in os.listdir(os.path.join(out_dir, 'objects')):
    os.remove(os.path.join(out_dir, 'objects', name))

  tests = list_tests(binary, args.filter)
  print(f'{len(tests)} tests; plain run first')
  plain_failed = run_plain(binary, args.filter, out_dir)
  print(f'plain run: {len(plain_failed)} failing; now layer mode, '
        f'{args.jobs} at a time')

  results = []
  with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
    for result in pool.map(lambda t: run_one_layered(binary, t, out_dir),
                           tests):
      results.append(result)

  expectations = read_list(EXPECTATIONS)
  writers = read_list(WRITERS)

  # A test that fails only in layer mode, unexplained, runs once more on its
  # own: timing tests can fail under the parallel load.
  flaky = []
  for index, (test, status, objects, reason) in enumerate(results):
    explained = any(fnmatch.fnmatchcase(test, p) for p in expectations)
    if status != 'pass' and test not in plain_failed and not explained:
      again = run_one_layered(binary, test, out_dir)
      if again[1] == 'pass':
        flaky.append(test)
        results[index] = again
  errors, warnings, lines = [], [], []
  layer_only = {}
  for test, status, objects, reason in results:
    if status != 'pass' and test not in plain_failed:
      layer_only[test] = (status, reason)
      if not any(fnmatch.fnmatchcase(test, p) for p in expectations):
        errors.append(f'{status} only in layer mode: {test}: {reason}')
    if objects and test not in writers and not args.record_writers:
      errors.append(f'leaves {objects} objects in the layer but is not a '
                    f'known writer: {test}')
  if tests and not any(r[2] for r in results):
    errors.append('no test left an object in a layer: layer mode did not '
                  'take effect (check the hooks in embedder_test.cpp)')
  ran = [r[0] for r in results]
  for pattern in expectations:
    matched = [t for t in ran if fnmatch.fnmatchcase(t, pattern)]
    # A test that fails the plain run too (a platform's goldens) says nothing.
    if matched and not any(t in layer_only or t in plain_failed
                           for t in matched):
      warnings.append(f'expected to fail in layer mode, but passes: {pattern}')

  if args.record_writers:
    header = read_header(WRITERS) or [
        '# Embedder tests that leave objects in a layer: they write.\n',
        '# Written by run_layer_mode.py --record-writers; review the diff.\n',
    ]
    with open(WRITERS, 'w') as f:
      f.writelines(header)
      for test, _, objects, _ in sorted(results):
        if objects:
          f.write(f'{test} {objects}\n')

  counts = {}
  for _, status, _, _ in results:
    counts[status] = counts.get(status, 0) + 1
  lines.append(f'layer mode: {counts}; failing only in layer mode: '
               f'{len(layer_only)}; writers: '
               f'{sum(1 for r in results if r[2])}')
  lines += [f'warning: failed under load, passed alone: {t}' for t in flaky]
  lines += [f'warning: {w}' for w in warnings]
  lines += [f'error: {e}' for e in errors]
  report = '\n'.join(lines)
  print(report)
  with open(os.path.join(out_dir, 'report.txt'), 'w') as f:
    f.write(report + '\n')
  sys.exit(1 if errors else 0)


if __name__ == '__main__':
  main()
