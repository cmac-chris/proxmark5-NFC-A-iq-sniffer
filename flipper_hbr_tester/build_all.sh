#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the tester for several firmwares into dist_all/, each against its own SDK (one ufbt home per target, so the
# targets never overwrite each other). A .fap loads on any firmware with the same API major version: the file name
# carries the firmware it was built for and its API version.
# usage: ./build_all.sh [target ...]       (default: all; the first build of a target downloads its SDK)
set -e
cd "$(dirname "$0")"

# name | SDK zip
TARGETS=(
    "ofw-1.5.1|https://update.flipperzero.one/builds/firmware/1.5.1/flipper-z-f7-sdk-1.5.1.zip"
    "mntm-012|https://up.momentum-fw.dev/builds/firmware/mntm-012/flipper-z-f7-sdk-mntm-012.zip"
    "unlshd-094|https://unleashedflip.com/fw/unlshd-094/flipper-z-f7-sdk-unlshd-094.zip"
    "unlshd-082|https://unleashedflip.com/fw/unlshd-082/flipper-z-f7-sdk-unlshd-082.zip"
)
UFBT_HOMES=${UFBT_HOMES:-$HOME/.ufbt_targets}
OUT=dist_all
mkdir -p "$OUT"

for t in "${TARGETS[@]}"; do
    name=${t%%|*}
    url=${t#*|}
    if [ $# -gt 0 ] && [[ " $* " != *" $name "* ]]; then continue; fi
    export UFBT_HOME="$UFBT_HOMES/$name"
    # the default ufbt home already holding this SDK is used as it is
    if grep -q "\"url\": \"$url\"" "$HOME/.ufbt/current/ufbt_state.json" 2>/dev/null; then
        export UFBT_HOME="$HOME/.ufbt"
    fi
    echo "==== $name ($UFBT_HOME)"
    if ! grep -q "\"url\": \"$url\"" "$UFBT_HOME/current/ufbt_state.json" 2>/dev/null; then
        python -m ufbt update --hw-target=f7 --url="$url"
    fi
    python -m ufbt
    csv=$(find "$UFBT_HOME/current/sdk_headers" -name api_symbols.csv -path "*f7*" | head -1)
    api=$(grep -m1 '^Version' "$csv" | cut -d, -f3)
    cp dist/iso14443a_hbr_tester.fap "$OUT/iso14443a_hbr_tester_${name}_api${api}.fap"
    echo "     -> $OUT/iso14443a_hbr_tester_${name}_api${api}.fap"
done
