#!/usr/bin/env bash
# Generate the shared pairing key and network id for one bridge pair.
# Writes sdkconfig.defaults.local, or pairs/<PAIR>.defaults with PAIR=<name>
# (both git-ignored). Build BOTH units of the pair afterwards so they share it.
#   scripts/gen_key.sh            PAIR=drone2 scripts/gen_key.sh
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/_pair.sh
OUT=$PAIR_FILE
mkdir -p "$(dirname "$OUT")"
if [[ -f $OUT && ${1:-} != "--force" ]]; then
    echo "$OUT already exists (use --force to replace; both units must then be reflashed)" >&2
    exit 1
fi
KEY=$(openssl rand -hex 16)
NET=$(printf '0x%04X' $(( ($(od -An -N2 -tu2 /dev/urandom) % 65534) + 1 )))
umask 077
cat > "$OUT" <<CFG
# Pairing secret for this bridge pair - do not commit.
CONFIG_BRIDGE_KEY="$KEY"
CONFIG_BRIDGE_NET_ID=$NET
CFG
echo "Wrote $OUT (net id $NET). Rebuild and flash both units${PAIR:+ with PAIR=$PAIR}."
