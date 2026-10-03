#!/usr/bin/env bash
# Set the radio channel (and optionally the regulatory country) for this
# bridge pair:   scripts/set_channel.sh <1-13> [COUNTRY]
# e.g.           scripts/set_channel.sh 9 US
# Written next to the pairing key (sdkconfig.defaults.local, or
# pairs/<PAIR>.defaults with PAIR=<name>), so both units of the pair always
# agree. Rebuild and reflash BOTH units.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/_pair.sh
CH=${1:?usage: $0 <channel 1-13> [country code, e.g. FI]}
if ! [[ $CH =~ ^[0-9]+$ ]] || (( CH < 1 || CH > 13 )); then
    echo "channel must be 1-13" >&2; exit 2
fi
COUNTRY=${2:-}
if [[ -n $COUNTRY && ! $COUNTRY =~ ^([A-Z]{2}|01)$ ]]; then
    echo "country must be a 2-letter ISO code (e.g. FI, US) or 01" >&2; exit 2
fi

F=$PAIR_FILE
if [[ -n ${PAIR:-} && ! -f $F ]]; then
    echo "$F does not exist: run PAIR=$PAIR scripts/gen_key.sh first" >&2; exit 1
fi
touch "$F"; chmod 600 "$F"
tmp=$(mktemp)
grep -v -e '^CONFIG_BRIDGE_WIFI_CHANNEL=' ${COUNTRY:+-e '^CONFIG_BRIDGE_COUNTRY='} "$F" > "$tmp" || true
echo "CONFIG_BRIDGE_WIFI_CHANNEL=$CH" >> "$tmp"
[[ -n $COUNTRY ]] && echo "CONFIG_BRIDGE_COUNTRY=\"$COUNTRY\"" >> "$tmp"
mv "$tmp" "$F"
echo "Channel $CH${COUNTRY:+, country $COUNTRY} saved to $F. Rebuild and reflash BOTH units."
