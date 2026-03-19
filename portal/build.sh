#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

cd "$SCRIPT_DIR"

pnpm install
pnpm run build
pnpm run export:spiffs

echo ""
echo "SPIFFS data ready in: $(dirname "$SCRIPT_DIR")/data"
echo "Upload with: pio run -e tx -t uploadfs"
