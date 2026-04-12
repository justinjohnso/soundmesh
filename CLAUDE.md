@AGENTS.md

# Claude Code — SoundMesh Project Instructions

## Hooks Guidance

Recommended PostToolUse hooks for this project:
- After editing `.c` or `.h` files: run `pio run -e src && pio run -e out` to catch breakage early
- After editing `test/native/**`: run `pio test -e native`
- After editing `lib/control/portal-ui/**`: run `cd lib/control/portal-ui && pnpm run build`

## Path-Scoped Rules

Rules in `.claude/rules/` load only when working on matching files:
- `firmware.md` — C/ESP-IDF conventions for `lib/**` and `src/**`
- `portal-ui.md` — Astro/JS conventions for `lib/control/portal-ui/**`
