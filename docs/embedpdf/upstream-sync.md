# Syncing upstream PDFium

How to merge upstream PDFium into this fork, by hand or by an agent. Read
[the ledger](fork-ledger.md) first: it says what our changes protect.

## When

- **At the start of every release that ships PDFium**: every fork release, and
  every npm release (the packages are versioned together, so each one
  republishes `@embedpdf/engine-runtime`).
- **When the mirror job flags a likely security fix**: start a release.
- **After a month without one**, so the next sync stays small.

A release may not miss an upstream commit that was more than 2 days old when it
started. The sync takes the newest upstream commit that is at least 1 day old:
upstream `main` is a development branch, and some commits get reverted within
a day.

## Steps

All commands run from the fork's root.

1. **Fetch upstream and pick the target**:
   ```bash
   git fetch --no-tags https://pdfium.googlesource.com/pdfium main:refs/remotes/upstream/main
   git rev-list -1 --first-parent --before="1 day ago" upstream/main
   ```
2. **Preview.** This writes the report skeleton, without touching any branch:
   ```bash
   python3 scripts/embedpdf-runtime/upstream-preview.py <target> --out docs/embedpdf/syncs/<date>-<target>.md
   ```
   It lists the conflicts with the rules each block carries, flags `EPDF_TLS`
   blocks, and pre-fills the write audit, the appearance generator's commits,
   new globals, build changes and the API diff.
3. **Branch and merge.** Never rebase.
   ```bash
   # --no-track: a branch made from origin/embedpdf/main would otherwise track
   # it, and a plain push or an editor's "Sync" would go to embedpdf/main.
   git switch --no-track -c embedpdf/sync/<target> origin/embedpdf/main
   git config rerere.enabled true
   git merge --no-ff <target>
   ```
4. **Resolve each conflict block:**
   - read its tags, the ledger entries they name, and the upstream commits that
     touched the file (the preview lists them);
   - take upstream's change and keep our rule;
   - fill in the report's Resolution column: what upstream changed, what we
     kept.
5. **Fill in the rest of the report:**
   - **Write audit:** a verdict for every line: `fresh`, `door`, `fixed`, or
     `not-built`.
   - **Appearance generator:** port or skip each upstream commit, with the
     reason.
   - **Globals:** give each new upstream global `EPDF_TLS`, an atomic, or an
     allowlist entry with its reason.
6. **Build and test**, then tick the report's checklist:
   ```bash
   PDF_RUNTIME_SYNC=auto bash scripts/embedpdf-runtime/test-target.sh linux-x64   # or darwin-arm64
   python3 testing/tools/run_layer_mode.py out/embedpdf-runtime-tests-<target>
   python3 scripts/embedpdf-runtime/check-fork-tags.py --strict
   python3 scripts/embedpdf-runtime/check-c-api.py
   bash scripts/embedpdf-runtime/build-target.sh <target>
   python3 scripts/embedpdf-runtime/check-globals.py out/embedpdf-runtime/<target>/libembedpdf.{so,dylib}
   bash scripts/embedpdf-runtime/build-target.sh wasm32
   ```
   - Run `apply-patches.sh` for every target. A `DEPS` roll can break a patch;
     refresh it and say so in the report.
   - The ThreadSanitizer soak runs in CI on the sync PR
     (`pdfium-tsan-thread-soak.yml`).
   - Engine: build the local runtime, regenerate the bindings
     (`build/generate-functions.mjs` in the runtime package) and diff them, and
     run the engine tests and `engine-conformance`.
   - A moved pixel golden is listed with the upstream commit that moved it.
   - After building, run `unapply-patches.sh <target>` and remove
     `BUILD.gn.orig`, so only the merge is committed.
7. **Open a PR into `embedpdf/main`** with the report.

## Release gates

`release-libpdfium.yml` runs these before anything is published, on every run:

- **Upstream freshness** (`check-upstream-freshness.py`): lists the upstream
  commits the ref misses, likely security fixes marked. With `release` on it
  fails when the oldest one landed more than 2 days ago. The release notes carry
  the list, and the manifest the upstream commit the release is based on.
- **Fork tags** (`check-fork-tags.py --strict`).
- **Layer mode** on the Linux test build (`run_layer_mode.py`).
- **Writable globals** on the linux-x64 library (`check-globals.py`).
- **ThreadSanitizer soak** (`pdfium-tsan-thread-soak.yml`).

**The override.** When a sync can't land before a release, set
`upstream_gate_override_reason`. The release then publishes with a warning, and
its notes give the reason and every upstream commit it misses. Every likely
security fix it holds back must be cherry-picked into the release first, with
`git cherry-pick -x` (the gate checks for the `cherry picked from commit` line).
The next release tries the full sync again.

## Never silently

- Take upstream's side of a tagged block without a line in the report.
- Take upstream's side of a block that holds `EPDF_TLS` without putting
  `EPDF_TLS` back on the variable, renamed or not.
- Delete a tag, or change a ledger rule. Rules change only with review: put
  proposals in the report's "Ledger changes proposed".
- Change a test expectation (a golden, a checksum, an expected count) without a
  line saying why.
- Mark a write-audit line safe without its verdict and reason.
- Add a global to the allowlist without its category and reason.

## When an agent stops and asks

- A conflict touches a rule that has no guard test.
- A write-audit line doesn't fit a verdict.
- A test needs an expectation change that no upstream commit explains.
- A new upstream feature has no layer behaviour we know of (layer mode shows
  it failing, and the cause isn't in the ledger).

## A clean sync

A sync is clean when every one of these holds:
- no conflict blocks;
- every write-audit verdict is `fresh` or `not-built`;
- no new caller of `CPDF_GenerateAP::`, no new globals;
- no changed test expectations, no `DEPS` or build-flag changes;
- every check green.

A person reviews every sync for now; whether a clean one may merge without
review is still open (upstream-sync plan, decision D8).

## Reports

One per sync in `docs/embedpdf/syncs/`, named `<date>-<upstream target>.md`.
They are the record of why each choice was made. Keep them.
