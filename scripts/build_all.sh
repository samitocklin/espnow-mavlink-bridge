#!/usr/bin/env bash
# Compile matrix: every role on every supported target, warnings as errors.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/_pair.sh
fail=0
for target in esp32 esp32c3 esp32s3; do
    for role in ground air; do
        printf '%-8s %-8s ' "$role" "$target"
        if log=$(scripts/build.sh "$role" "$target" 2>&1); then
            size=$(stat -f%z "$BUILD_ROOT/$role-$target/espnow_mavlink_bridge.bin" 2>/dev/null ||
                   stat -c%s "$BUILD_ROOT/$role-$target/espnow_mavlink_bridge.bin")
            echo "OK  ($size bytes)"
        else
            echo "FAIL"; echo "$log" | grep -E "error|Error" | head -20; fail=1
        fi
    done
done
exit $fail
