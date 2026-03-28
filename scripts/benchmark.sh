#!/bin/bash
#
# benchmark.sh — benchmark quine compress/decompress and compare with zstd
#
# Usage:
#   ./scripts/benchmark.sh <dir_a> <dir_b>
#
# Requires: build/quine, zstd, tar
#

set -euo pipefail

QUINE="${QUINE:-build/quine}"

if [ $# -ne 2 ]; then
    echo "Usage: $0 <dir_a> <dir_b>" >&2
    exit 1
fi

DIR_A="$1"
DIR_B="$2"

TMPDIR=$(mktemp -d /tmp/quine_bench_XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

# ── Measure dir sizes ──

dir_size() {
    du -sb "$1" 2>/dev/null | awk '{print $1}'
}

fmt_size() {
    local b=$1
    if [ "$b" -ge 1073741824 ]; then
        awk "BEGIN{printf \"%.2f GB\", $b/1073741824}"
    elif [ "$b" -ge 1048576 ]; then
        awk "BEGIN{printf \"%.2f MB\", $b/1048576}"
    elif [ "$b" -ge 1024 ]; then
        awk "BEGIN{printf \"%.1f KB\", $b/1024}"
    else
        echo "${b} B"
    fi
}

SZ_A=$(dir_size "$DIR_A")
SZ_B=$(dir_size "$DIR_B")

echo "=== Input ==="
echo "  dir A:  $(fmt_size "$SZ_A")  ($DIR_A)"
echo "  dir B:  $(fmt_size "$SZ_B")  ($DIR_B)"
echo ""

# ── Helper: run command and capture time + peak RSS ──

run_timed() {
    local label="$1"
    shift
    /usr/bin/time -v "$@" 2>"$TMPDIR/time_out" && local rc=0 || local rc=$?

    local wall=$(grep "Elapsed (wall clock)" "$TMPDIR/time_out" | sed 's/.*: //')
    local rss=$(grep "Maximum resident set size" "$TMPDIR/time_out" | awk '{print $NF}')

    echo "  wall time:  $wall"
    echo "  peak RSS:   $(fmt_size $((rss * 1024)))"
    return $rc
}

# ── Quine compress ──

echo "=== Quine compress ==="
PATCH="$TMPDIR/quine.patch"
run_timed "quine compress" "$QUINE" compress "$DIR_A" "$DIR_B" "$PATCH" >/dev/null

PATCH_SZ=$(stat -c%s "$PATCH")
RATIO=$(awk "BEGIN{printf \"%.2f\", $SZ_B/$PATCH_SZ}")
SAVINGS=$(awk "BEGIN{printf \"%.0f\", 100*(1-$PATCH_SZ/$SZ_B)}")
echo "  patch size: $(fmt_size "$PATCH_SZ")"
echo "  ratio:      ${RATIO}x  (${SAVINGS}% savings)"
echo ""

# ── Quine decompress ──

echo "=== Quine decompress ==="
RESTORED="$TMPDIR/quine_restored"
run_timed "quine decompress" "$QUINE" decompress "$DIR_A" "$PATCH" "$RESTORED" >/dev/null
echo ""

# ── zstd: tar dir_b, compress with zstd ──

echo "=== zstd compress (tar + zstd) ==="
ZSTD_TAR="$TMPDIR/b.tar.zst"
run_timed "zstd compress" bash -c "tar cf - -C '$(dirname "$DIR_B")' '$(basename "$DIR_B")' | zstd -T0 -o '$ZSTD_TAR'"

ZSTD_SZ=$(stat -c%s "$ZSTD_TAR")
ZSTD_RATIO=$(awk "BEGIN{printf \"%.2f\", $SZ_B/$ZSTD_SZ}")
ZSTD_SAVINGS=$(awk "BEGIN{printf \"%.0f\", 100*(1-$ZSTD_SZ/$SZ_B)}")
echo "  archive:    $(fmt_size "$ZSTD_SZ")"
echo "  ratio:      ${ZSTD_RATIO}x  (${ZSTD_SAVINGS}% savings)"
echo ""

# ── zstd: delta (diff against tar of dir_a) ──

echo "=== zstd delta compress (tar A as dict) ==="
TAR_A="$TMPDIR/a.tar"
TAR_B="$TMPDIR/b.tar"
tar cf "$TAR_A" -C "$(dirname "$DIR_A")" "$(basename "$DIR_A")"
tar cf "$TAR_B" -C "$(dirname "$DIR_B")" "$(basename "$DIR_B")"
ZSTD_DELTA="$TMPDIR/b_delta.tar.zst"
run_timed "zstd delta" zstd -T0 --patch-from="$TAR_A" "$TAR_B" -o "$ZSTD_DELTA"

ZSTD_D_SZ=$(stat -c%s "$ZSTD_DELTA")
ZSTD_D_RATIO=$(awk "BEGIN{printf \"%.2f\", $SZ_B/$ZSTD_D_SZ}")
ZSTD_D_SAVINGS=$(awk "BEGIN{printf \"%.0f\", 100*(1-$ZSTD_D_SZ/$SZ_B)}")
echo "  patch:      $(fmt_size "$ZSTD_D_SZ")"
echo "  ratio:      ${ZSTD_D_RATIO}x  (${ZSTD_D_SAVINGS}% savings)"
echo ""

# ── Summary table ──

echo "=== Summary ==="
printf "  %-25s %15s %15s %10s\n" "Method" "Output Size" "Ratio" "Savings"
printf "  %-25s %15s %15s %10s\n" "-------------------------" "---------------" "---------------" "----------"
printf "  %-25s %15s %15s %10s\n" "quine delta" "$(fmt_size "$PATCH_SZ")" "${RATIO}x" "${SAVINGS}%"
printf "  %-25s %15s %15s %10s\n" "zstd (standalone)" "$(fmt_size "$ZSTD_SZ")" "${ZSTD_RATIO}x" "${ZSTD_SAVINGS}%"
printf "  %-25s %15s %15s %10s\n" "zstd --patch-from (delta)" "$(fmt_size "$ZSTD_D_SZ")" "${ZSTD_D_RATIO}x" "${ZSTD_D_SAVINGS}%"
printf "  %-25s %15s %15s %10s\n" "raw dir B" "$(fmt_size "$SZ_B")" "1.00x" "0%"
