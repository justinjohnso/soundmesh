#!/bin/bash
# Build portal assets for SPIFFS upload
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DATA_DIR="$PROJECT_DIR/data"

mkdir -p "$DATA_DIR"
rm -f "$DATA_DIR"/*

echo "Building SoundMesh Portal assets..."
echo ""

# Copy and optionally gzip
for f in index.html app.js app.css; do
    if command -v gzip &> /dev/null; then
        gzip -9 -c "$SCRIPT_DIR/$f" > "$DATA_DIR/$f.gz"
        echo "  $f → $f.gz ($(wc -c < "$DATA_DIR/$f.gz" | tr -d ' ') bytes)"
    else
        cp "$SCRIPT_DIR/$f" "$DATA_DIR/$f"
        echo "  $f ($(wc -c < "$DATA_DIR/$f" | tr -d ' ') bytes)"
    fi
done

echo ""
echo "SPIFFS data ready in: $DATA_DIR"
echo "Upload with: pio run -e tx -t uploadfs"
