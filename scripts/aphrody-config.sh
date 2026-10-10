#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

profile=${1:-generic}
case "$profile" in generic|nvidia) ;; *) echo "expected generic or nvidia" >&2; exit 2 ;; esac
: "${KBUILD_OUTPUT:?set KBUILD_OUTPUT to an absolute build directory}"
case "$KBUILD_OUTPUT" in /*) ;; *) echo "KBUILD_OUTPUT must be absolute" >&2; exit 2 ;; esac
case "${ARCH:-x86_64}" in
    x86|x86_64) arch=x86; carch=x86_64 ;;
    arm64|aarch64) arch=arm64; carch=aarch64 ;;
    *) echo "supported architectures: x86_64 and aarch64" >&2; exit 2 ;;
esac
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$KBUILD_OUTPUT"
make -C "$root" O="$KBUILD_OUTPUT" ARCH="$arch" LLVM=1 rustavailable
make -C "$root" O="$KBUILD_OUTPUT" ARCH="$arch" LLVM=1 defconfig
awk -v carch="$carch" '
    /^# @arch / {
        n=split(substr($0, 9), a, " ")
        ok=0
        for (i=1;i<=n;i++) if (a[i]==carch) ok=1
        pending=1
        next
    }
    /^(CONFIG_|# CONFIG_)/ {
        if (pending && !ok) { pending=0; next }
        pending=0
        print
    }
' "$root/kernel/configs/aphrody-bun.config" > "$KBUILD_OUTPUT/aphrody.fragment"
if [ "$profile" = nvidia ]; then
    cat "$root/kernel/configs/aphrody-nvidia.config" >> "$KBUILD_OUTPUT/aphrody.fragment"
fi
KCONFIG_CONFIG="$KBUILD_OUTPUT/.config" \
    "$root/scripts/kconfig/merge_config.sh" -m -O "$KBUILD_OUTPUT" \
    "$KBUILD_OUTPUT/.config" "$KBUILD_OUTPUT/aphrody.fragment"
make -C "$root" O="$KBUILD_OUTPUT" ARCH="$arch" LLVM=1 olddefconfig
awk '{ s=$0; sub(/^# /,"",s); sub(/[= ].*/,"",s); if (s ~ /^CONFIG_/) v[s]=$0 }
    END { for (s in v) print v[s] }' "$KBUILD_OUTPUT/aphrody.fragment" \
    > "$KBUILD_OUTPUT/aphrody.expected"
failed=0
while IFS= read -r line; do
    case "$line" in
        "# CONFIG_"*" is not set")
            sym=${line#\# }
            sym=${sym%% *}
            grep -q "^$sym=" "$KBUILD_OUTPUT/.config" || continue
            ;;
        *) grep -qxF "$line" "$KBUILD_OUTPUT/.config" && continue ;;
    esac
    echo "Kconfig refused: $line" >&2
    failed=1
done < "$KBUILD_OUTPUT/aphrody.expected"
exit "$failed"
