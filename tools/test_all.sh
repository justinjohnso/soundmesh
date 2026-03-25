#!/usr/bin/env bash
set -euo pipefail

pio test -e native
pio run -e src
pio run -e out

# Pre-upload crash-risk gate (must pass before flashing hardware)
bash tools/preupload_gate.sh
