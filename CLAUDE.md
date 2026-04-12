@AGENTS.md

# Claude Code — SoundMesh Project Instructions

## Quick Validation

After any firmware code change, confirm both environments compile:
```bash
pio run -e src && pio run -e out
```

Before hardware upload, run the full gate:
```bash
bash tools/preupload_gate.sh
```

## Hooks Guidance

Recommended PostToolUse hooks for this project:
- After editing `.c` or `.h` files: run `pio run -e src && pio run -e out` to catch breakage early
- After editing `test/native/**`: run `pio test -e native`
- After editing `lib/control/portal-ui/**`: run `cd lib/control/portal-ui && pnpm run build`

## Path-Scoped Rules

Rules in `.claude/rules/` load only when working on matching files:
- `firmware.md` — C/ESP-IDF conventions for `lib/**` and `src/**`
- `portal-ui.md` — Astro/JS conventions for `lib/control/portal-ui/**`

## Session Workflow

1. Start: check `docs/roadmap/implementation-roadmap.md` for current priorities
2. Work: use worktrees for non-trivial features (per AGENTS.md git workflow)
3. Validate: `pio test -e native && pio run -e src && pio run -e out`
4. End: `/learn` to persist findings, then `/handoff` if continuing later
