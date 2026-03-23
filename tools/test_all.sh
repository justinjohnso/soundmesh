#!/usr/bin/env bash
set -euo pipefail

pio test -e native
pio run -e tx
pio run -e rx
pio run -e combo

