#!/usr/bin/env python3
# Copyright 2026 CloudPDF LTD
# SPDX-License-Identifier: Apache-2.0

"""Check that HEAD misses no upstream PDFium commit older than 2 days.

The release gate of docs/embedpdf/upstream-sync.md: a release may not miss an
upstream commit that was more than 2 days old when the release started. It
lists the upstream commits HEAD doesn't contain (`git log <upstream> ^HEAD`),
marks the ones that look like security fixes, and finds the upstream commit
HEAD is based on (the merge base), which the release manifest records.

It fails, with --enforce, when the oldest missing commit is older than
--max-age-hours. With --override-reason it warns instead, but a missing commit
that looks like a security fix must still be in HEAD as a cherry-pick: a commit
since the base whose message says `(cherry picked from commit <sha>)`
(`git cherry-pick -x`). Without --enforce it only reports, as the mirror job and
release=false runs do.

Commit times are committer dates: when the commit landed upstream.

Usage:
  check-upstream-freshness.py [--upstream REF] [--max-age-hours N] [--enforce]
                              [--override-reason TEXT] [--notes FILE]
                              [--json FILE]

  --upstream         upstream main, fetched. Default: upstream/main.
  --notes            write the report as Markdown (release notes, run summary).
  --json             write {sha, date, subject} of the upstream base, plus the
                     missing count and the override, for the release manifest.

In GitHub Actions the report also goes to the run summary, and each likely
security fix becomes a warning.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

SOURCE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

# Subjects that look like security fixes; the mirror job runs this script too.
# Not "fuzz" (fuzzer build changes, fuzzy image matching: fixes a fuzzer found
# say crash or overflow) nor a bare "heap" (heap allocators).
SECURITY = re.compile(
    r'overflow|crash|out[- ]of[- ]bounds|\boob\b|use[- ]after[- ]free|\buaf\b|'
    r'(?:heap|stack)[- ]buffer|null(?:ptr)? deref|security|sanitiz|\bcve\b',
    re.IGNORECASE)
CHERRY_PICK = re.compile(r'\(cherry picked from commit ([0-9a-f]{40})\)')


def git(*args):
  return subprocess.run(['git', '-C', SOURCE_DIR, *args],
                        check=True,
                        capture_output=True,
                        text=True,
                        errors='replace').stdout


def commits(*rev_args):
  """Returns [(sha, unix time, ISO date, subject)], newest first."""
  out = git('log', '--format=%H%x09%ct%x09%cI%x09%s', *rev_args)
  result = []
  for line in out.splitlines():
    sha, stamp, date, subject = line.split('\t', 3)
    result.append((sha, int(stamp), date, subject))
  return result


def cherry_picked(base):
  return set(CHERRY_PICK.findall(git('log', '--format=%B', f'{base}..HEAD')))


def age_text(seconds):
  hours = seconds / 3600
  return f'{hours / 24:.1f} days' if hours >= 48 else f'{hours:.0f} hours'


def main():
  parser = argparse.ArgumentParser(
      description=__doc__.split('\n\n')[0],
      formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('--upstream', default='upstream/main')
  parser.add_argument('--max-age-hours', type=float, default=48)
  parser.add_argument('--enforce', action='store_true')
  parser.add_argument('--override-reason', default='')
  parser.add_argument('--notes')
  parser.add_argument('--json')
  args = parser.parse_args()

  now = time.time()
  base = git('merge-base', 'HEAD', args.upstream).strip()
  base_sha, _, base_date, base_subject = commits('-1', base)[0]
  missing = commits(args.upstream, '^HEAD')
  security = [c for c in missing if SECURITY.search(c[3])]
  picked = cherry_picked(base)
  oldest = missing[-1] if missing else None
  stale = bool(oldest) and now - oldest[1] > args.max_age_hours * 3600
  override = args.override_reason.strip()
  unpicked = [c for c in security if c[0] not in picked]

  lines = [
      '## Upstream PDFium',
      '',
      f'- Based on upstream `{base_sha[:9]}` ({base_date[:10]}) {base_subject}',
  ]
  if oldest:
    lines.append(f'- Missing {len(missing)} upstream commits; the oldest landed '
                 f'{age_text(now - oldest[1])} ago (limit: '
                 f'{age_text(args.max_age_hours * 3600)}).')
  else:
    lines.append('- Missing no upstream commit.')
  if stale and override:
    lines.append(f'- **Gate overridden:** {override}')
  if missing:
    lines += ['', '### Missing upstream commits', '']
    for commit in missing:
      sha, _, date, subject = commit
      mark = ''
      if commit in security:
        mark = (' (likely security fix; cherry-picked)'
                if sha in picked else ' (**likely security fix**)')
      lines.append(f'- `{sha[:9]}` {date[:10]} {subject}{mark}')
  report = '\n'.join(lines) + '\n'

  print(report)
  if os.environ.get('GITHUB_STEP_SUMMARY'):
    with open(os.environ['GITHUB_STEP_SUMMARY'], 'a', encoding='utf-8') as f:
      f.write(report)
  if os.environ.get('GITHUB_ACTIONS'):
    for sha, _, _, subject in security:
      print(f'::warning title=Likely upstream security fix::{sha[:9]} {subject}')
  if args.notes:
    with open(args.notes, 'w', encoding='utf-8') as f:
      f.write(report)
  if args.json:
    with open(args.json, 'w', encoding='utf-8') as f:
      json.dump({'sha': base_sha, 'date': base_date, 'subject': base_subject,
                 'missing': len(missing), 'override': override if stale else ''},
                f, indent=2)
      f.write('\n')

  if not args.enforce or not stale:
    return
  if not override:
    sys.exit(f'error: missing upstream commits older than '
             f'{age_text(args.max_age_hours * 3600)}: sync first '
             '(docs/embedpdf/upstream-sync.md), or pass an override reason.')
  if unpicked:
    sys.exit('error: an override still needs every likely security fix it '
             'holds back, cherry-picked with -x: ' +
             ', '.join(c[0][:9] for c in unpicked))
  print('warning: release gate overridden: ' + override)


if __name__ == '__main__':
  main()
