#!/bin/sh
# Script to recursively copy ESP-ADF component dependencies
# Usage: copy-adf-deps.sh <esp-adf-path> <target-components-path> <initial-component>

set -e

ADF_COMPONENTS_PATH="$1"
TARGET_PATH="$2"
shift 2

COPIED_LIST=""
TO_PROCESS="$*"

# Check if component was already copied
is_copied() {
    echo "$COPIED_LIST" | grep -q "^$1$"
}

# Add to copied list
mark_copied() {
    COPIED_LIST="$COPIED_LIST
$1"
}

# Check if component should be skipped (ESP-IDF built-in)
is_builtin() {
    case "$1" in
        esp_*|driver|nvs_flash|spi_flash|freertos|log|newlib|vfs|fatfs|wear_levelling|console|json|spiffs|mbedtls)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

# Function to extract REQUIRES and PRIV_REQUIRES from CMakeLists.txt
get_component_deps() {
    component_path="$1"
    cmake_file="$component_path/CMakeLists.txt"
    
    if [ ! -f "$cmake_file" ]; then
        return
    fi
    
    # Extract component names from REQUIRES and PRIV_REQUIRES lines
    grep -E "COMPONENT_(PRIV_)?REQUIRES|set\(COMPONENT_(PRIV_)?REQUIRES" "$cmake_file" 2>/dev/null | \
        sed -E 's/.*REQUIRES[[:space:]]+//; s/\).*//; s/#.*//' | \
        tr ' ' '\n' | \
        grep -v '^$' || true
}

# Process components iteratively
while [ -n "$TO_PROCESS" ]; do
    # Get first component from list
    component=$(echo "$TO_PROCESS" | head -n1 | awk '{print $1}')
    TO_PROCESS=$(echo "$TO_PROCESS" | sed 's/^[^ ]* *//')
    
    # Skip if already copied
    if is_copied "$component"; then
        continue
    fi
    
    # Skip ESP-IDF built-in components
    if is_builtin "$component"; then
        continue
    fi
    
    component_src="$ADF_COMPONENTS_PATH/$component"
    
    # Check if component exists in ESP-ADF
    if [ ! -d "$component_src" ]; then
        continue
    fi
    
    # Copy component
    echo "Copying $component..."
    cp -R "$component_src" "$TARGET_PATH/$component"
    mark_copied "$component"
    
    # Get dependencies and add to processing queue
    deps=$(get_component_deps "$component_src")
    for dep in $deps; do
        if ! is_copied "$dep"; then
            TO_PROCESS="$TO_PROCESS $dep"
        fi
    done
done

copied_count=$(echo "$COPIED_LIST" | grep -c "^" || echo 0)
echo "Done! Copied $copied_count components."
