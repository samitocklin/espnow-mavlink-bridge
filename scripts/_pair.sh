# Sourced by the other scripts. Selects which bridge pair's secrets to use.
#   (unset)       -> sdkconfig.defaults.local, builds in build/<role>-<target>
#   PAIR=drone2   -> pairs/drone2.defaults,   builds in build/drone2/<role>-<target>
# Each pair has its own key, network id and channel, so several drones (and
# several ground units on one relay computer) never pair across.
if [[ -n ${PAIR:-} ]]; then
    if ! [[ $PAIR =~ ^[A-Za-z0-9_-]+$ ]]; then
        echo "PAIR must be letters, digits, - or _" >&2; exit 2
    fi
    PAIR_FILE=pairs/$PAIR.defaults
    BUILD_ROOT=build/$PAIR
else
    PAIR_FILE=sdkconfig.defaults.local
    BUILD_ROOT=build
fi
