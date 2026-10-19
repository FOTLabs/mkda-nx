# MKDA runtime snapshot

Vendored from the tested MKDA XboxRecomp worktree at commit `55be5cf` (2026-10-19), based on the Buffy/Conker integration of upstream XboxRecomp. It includes the fixed-function secondary lighting and ordered GL completion-write fixes used by this release.

Unrelated captured Burnout menu/font data is replaced with the same empty placeholders used by the Buffy public repository. The locally generated analysis, retail fixtures and synthetic XBE binary are not included. Generate your own game analysis with the project tools.

Keep the original licences and attribution in LICENSE, NOTICE and LICENSES/. Project-specific ABI hooks live in the top-level src/ directory.

Patch v1.0.1: indexed textures bypass retained RGBA render targets at recycled addresses, preventing stale surfaces from overriding fighter palettes between bouts.
