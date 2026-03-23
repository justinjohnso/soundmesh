# SoundMesh Documentation

This folder is organized so active docs are obvious and historical materials are easy to find without cluttering current execution.

## Start Here

- Need to know what to do next:
  - `roadmap/implementation-roadmap.md` (canonical execution order)
- Need current architecture context:
  - `architecture/audio.md`
  - `architecture/network.md`
  - `architecture/control.md`
- Need audit findings for current hardening cycle:
  - `audits/2026-03-professionalization/`
- Need older progress logs and development posts:
  - `history/progress-notes/`
  - `history/posts/`
- Need retired plans kept for traceability:
  - `history/superseded-plans/`

## Directory Guide

- `roadmap/`
  - Canonical active execution plan.
- `architecture/`
  - Active reference docs for system layers.
- `audits/`
  - Time-bounded audit packets and gap analyses.
- `operations/`
  - Operator runbooks and deployment checklists.
- `quality/`
  - Testing strategy, benchmarks/SLO docs, and quality policy.
- `history/`
  - Historical materials not used as primary execution inputs.

## Governance Rules

- Keep `roadmap/implementation-roadmap.md` as the only canonical "what's next" source.
- Mark replaced docs as superseded and move them to `history/superseded-plans/`.
- Place narrative progress artifacts under `history/`, not active planning folders.
- Prefer updating existing canonical docs over creating one-off planning files.
