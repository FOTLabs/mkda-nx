# Buffy / Switch framework branch

This checkout is the local custom framework fork for `D:\xbox\buffy`.
Branch: `buffy-switch`. Its original `origin` points to the local Conker
checkout; do not push this branch there as an upstream contribution.

Rollback point: `buffy-pre-upstream-2026-10-14` (a904372).
The checkpoint's 16 runtime-file changes are line-ending normalization only
(`git diff --ignore-space-at-eol a904372^ a904372` is empty).

## Targeted upstream integration, 2026-10-14

Reviewed upstream main at 193e2995ffaec871bb3be43a43bd66aa5fccfeb1 (v0.13.1).

- #170 / a374a66: element-sized REP MOVS for MMIO, overlap and backward copies;
  64-bit range checks gate the ordinary-RAM memcpy fast path.
- #173 / 10f718b: recover arms after an inline jump table inside their owner.
- #174 / ef7fcc9: gap aliases end within their own body and backed section bytes.
- #175 / 6e2c914: adapt wrapper metadata, guaranteed generated bodies and protected
  branch/fallthrough ownership. Preserve local direct-wrapper routing and flat
  dispatch/leaf optimizations; these parts were already implemented here.

No bulk kernel, APU or renderer replacement. Heap reclaim (#158) remains an
independent candidate requiring review against this fork's allocator.

## Validation

Focused upstream regression tests: 169 passed, 3 subtests passed; no skips.
Three existing function-coalescence tests fail on both the rollback checkpoint
and this branch because their emitted-text assertions differ from the local
leaf optimization. They were explicitly deselected for the focused passing run.
Buffy MAP regression script: passed.
The REP access-counting test compiles and executes optimized C for all element
widths, overlap, direction, MMIO crossings and address wrap.
Full x86 differential conformance could not start: no 32-bit toolchain or
available Docker backend. It is not claimed as passing.

Game regeneration/build/replay results are recorded in the Buffy project's
NOTES.md and ignored logs directory.
