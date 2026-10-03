#!/usr/bin/env bash
# Build one unit:   [PAIR=<name>] scripts/build.sh <ground|air> <esp32|esp32c3|esp32s3> [idf.py args...]
# PAIR selects which pair's key/channel to use (see scripts/_pair.sh).
# With no extra args it builds; otherwise the args replace the action, e.g.
#                   scripts/build.sh air esp32s3 -p /dev/cu.usbmodem101 flash monitor
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/_pair.sh
ROLE=${1:?role: ground|air}; TARGET=${2:?target: esp32|esp32c3|esp32s3}; shift 2
case $ROLE in ground|air) ;; *) echo "bad role $ROLE" >&2; exit 2;; esac
case $TARGET in esp32|esp32c3|esp32s3) ;; *) echo "bad target $TARGET" >&2; exit 2;; esac

if ! command -v idf.py >/dev/null; then
    # Use whichever IDF Python env the installer created, even if another
    # python3 (pyenv etc.) comes first in PATH.
    if [[ -z ${IDF_PYTHON_ENV_PATH:-} ]]; then
        env_dir=$(ls -d "$HOME"/.espressif/python_env/idf5.*_env 2>/dev/null | tail -1 || true)
        [[ -n $env_dir ]] && export IDF_PYTHON_ENV_PATH=$env_dir
    fi
    # shellcheck disable=SC1091
    source "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null
fi

DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$ROLE"
if [[ -f $PAIR_FILE ]]; then
    DEFAULTS="$DEFAULTS;$PAIR_FILE"
elif [[ -n ${PAIR:-} ]]; then
    echo "$PAIR_FILE does not exist: run PAIR=$PAIR scripts/gen_key.sh first" >&2; exit 1
else
    echo "WARNING: no $PAIR_FILE - firmware will refuse to transmit (run scripts/gen_key.sh)" >&2
fi

B=$BUILD_ROOT/$ROLE-$TARGET
# Defaults only seed a fresh sdkconfig; regenerate so key/role changes apply.
rm -f "$B/sdkconfig"
[[ $# -eq 0 ]] && set -- build
exec idf.py -B "$B" -DIDF_TARGET="$TARGET" -DSDKCONFIG="$B/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="$DEFAULTS" "$@"
