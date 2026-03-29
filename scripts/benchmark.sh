#!/bin/bash
#
# benchmark.sh — benchmark quine+zip vs zstd delta compression
#
# Usage:
#   ./scripts/benchmark.sh <dir_a> <dir_b>
#
# Runs 5 tasks:
#   1. quine compress
#   2. zip quine patch
#   3. unzip + quine decompress
#   4. zstd delta compress (zstd --patch-from)
#   5. zstd delta decompress
#
# Requires: build/quine, zstd, zip, unzip, tar, /usr/bin/time
#

set -uo pipefail

QUINE="${QUINE:-build/quine}"
TWO_GB=2147483648

if [ $# -ne 2 ]; then
    echo "Usage: $0 <dir_a> <dir_b>" >&2
    exit 1
fi

DIR_A="$1"
DIR_B="$2"

TMPDIR=$(mktemp -d /tmp/quine_bench_XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

# ── Helpers ──

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

fmt_wall() {
    local s=$1
    awk "BEGIN{
        t=$s;
        if (t >= 60) printf \"%dm %04.1fs\", int(t/60), t - int(t/60)*60;
        else printf \"%.1fs\", t
    }"
}

# Sets: LAST_WALL (seconds), LAST_RSS_KB, LAST_RC (exit code)
run_cmd() {
    echo "  \$ $*"
    /usr/bin/time -v "$@" 2>"$TMPDIR/time_out"
    LAST_RC=$?

    local wall_str=$(grep "Elapsed (wall clock)" "$TMPDIR/time_out" | sed 's/.*: //')
    LAST_WALL=$(echo "$wall_str" | awk -F: '{
        if (NF==3) print $1*3600 + $2*60 + $3;
        else if (NF==2) print $1*60 + $2;
        else print $1
    }')
    LAST_WALL=${LAST_WALL:-0}

    LAST_RSS_KB=$(grep "Maximum resident set size" "$TMPDIR/time_out" | awk '{print $NF}')
    LAST_RSS_KB=${LAST_RSS_KB:-0}

    if [ "$LAST_RC" -ne 0 ]; then
        grep -v "^\t" "$TMPDIR/time_out" | grep -v "Command being timed" | head -5 | sed 's/^/  /'
    fi
    return $LAST_RC
}

print_summary() {
    local label="$1"
    local wall="$2"
    local rss_kb="$3"
    local out_size="$4"
    local ref_size="$5"

    echo ""
    echo "  --- $label ---"
    echo "  wall time:  $(fmt_wall "$wall")"
    echo "  peak RSS:   $(fmt_size $((rss_kb * 1024)))"
    if [ -n "$out_size" ]; then
        echo "  output:     $(fmt_size "$out_size")"
    fi
    if [ -n "$out_size" ] && [ -n "$ref_size" ] && [ "$ref_size" -gt 0 ] && [ "$out_size" -gt 0 ]; then
        local ratio=$(awk "BEGIN{printf \"%.2f\", $ref_size/$out_size}")
        local savings=$(awk "BEGIN{printf \"%.0f\", 100*(1-$out_size/$ref_size)}")
        echo "  ratio:      ${ratio}x  (${savings}% savings)"
    fi
}

# ── Input sizes ──

SZ_A=$(dir_size "$DIR_A")
SZ_B=$(dir_size "$DIR_B")

echo "=== Input ==="
echo "  dir A:  $(fmt_size "$SZ_A")  ($DIR_A)"
echo "  dir B:  $(fmt_size "$SZ_B")  ($DIR_B)"

if [ "$SZ_A" -gt "$TWO_GB" ] || [ "$SZ_B" -gt "$TWO_GB" ]; then
    echo ""
    echo "  WARNING: input > 2 GB — zstd --patch-from will likely fail"
    echo "           (zstd delta mode has a 2 GB dictionary limit)"
fi

# ── Track results for summary table ──

Q_COMP_WALL=0; Q_COMP_RSS=0; Q_PATCH_SZ=0; Q_COMP_OK=0
Q_ZIP_WALL=0;  Q_ZIP_RSS=0;  Q_ZIP_SZ=0;   Q_ZIP_OK=0
Q_DEC_WALL=0;  Q_DEC_RSS=0;  Q_DEC_SZ=0;   Q_DEC_OK=0
Z_COMP_WALL=0; Z_COMP_RSS=0; Z_PATCH_SZ=0;  Z_COMP_OK=0
Z_DEC_WALL=0;  Z_DEC_RSS=0;  Z_DEC_SZ=0;    Z_DEC_OK=0

# ── 1. Quine compress ──

echo ""
echo "=== 1. Quine compress ==="
QUINE_PATCH="$TMPDIR/quine.qn"
if run_cmd "$QUINE" compress "$DIR_A" "$DIR_B" "$QUINE_PATCH"; then
    Q_COMP_WALL=$LAST_WALL
    Q_COMP_RSS=$LAST_RSS_KB
    Q_PATCH_SZ=$(stat -c%s "$QUINE_PATCH")
    Q_COMP_OK=1
    print_summary "quine compress" "$Q_COMP_WALL" "$Q_COMP_RSS" "$Q_PATCH_SZ" "$SZ_B"
else
    echo "  FAILED (exit $LAST_RC)"
fi

# ── 2. Zip quine patch ──

echo ""
echo "=== 2. Zip quine patch ==="
QUINE_ZIP="$TMPDIR/quine.qn.zip"
if [ "$Q_COMP_OK" -eq 1 ]; then
    if run_cmd zip -j -q "$QUINE_ZIP" "$QUINE_PATCH"; then
        Q_ZIP_WALL=$LAST_WALL
        Q_ZIP_RSS=$LAST_RSS_KB
        Q_ZIP_SZ=$(stat -c%s "$QUINE_ZIP")
        Q_ZIP_OK=1
        print_summary "quine + zip" "$Q_ZIP_WALL" "$Q_ZIP_RSS" "$Q_ZIP_SZ" "$SZ_B"
    else
        echo "  FAILED (exit $LAST_RC)"
    fi
else
    echo "  SKIPPED (compress failed)"
fi

# ── 3. Unzip + Quine decompress ──

echo ""
echo "=== 3. Unzip + Quine decompress ==="
QUINE_RESTORED="$TMPDIR/quine_restored"
if [ "$Q_ZIP_OK" -eq 1 ]; then
    # Unzip
    UNZIP_DIR="$TMPDIR/unzipped"
    mkdir -p "$UNZIP_DIR"
    if run_cmd unzip -o -q "$QUINE_ZIP" -d "$UNZIP_DIR"; then
        UNZIP_WALL=$LAST_WALL
        UNZIP_RSS=$LAST_RSS_KB
        UNZIPPED_PATCH="$UNZIP_DIR/$(basename "$QUINE_PATCH")"
        # Decompress
        if run_cmd "$QUINE" decompress "$DIR_A" "$UNZIPPED_PATCH" "$QUINE_RESTORED"; then
            Q_DEC_WALL=$(awk "BEGIN{printf \"%.1f\", $UNZIP_WALL + $LAST_WALL}")
            Q_DEC_RSS=$LAST_RSS_KB
            # Use max RSS of unzip and decompress
            [ "$UNZIP_RSS" -gt "$Q_DEC_RSS" ] && Q_DEC_RSS=$UNZIP_RSS
            Q_DEC_SZ=$(dir_size "$QUINE_RESTORED")
            Q_DEC_OK=1
            print_summary "unzip + quine decompress" "$Q_DEC_WALL" "$Q_DEC_RSS" "$Q_DEC_SZ" ""
        else
            echo "  FAILED: quine decompress (exit $LAST_RC)"
        fi
    else
        echo "  FAILED: unzip (exit $LAST_RC)"
    fi
else
    echo "  SKIPPED (zip failed)"
fi

# ── 4. zstd delta compress (tar + zstd) ──

echo ""
echo "=== 4. zstd delta compress (tar + zstd) ==="
TAR_A="$TMPDIR/a.tar"
TAR_B="$TMPDIR/b.tar"
# Time tar as part of zstd's pipeline — it's a required prerequisite
if run_cmd tar cf "$TAR_A" -C "$(dirname "$DIR_A")" "$(basename "$DIR_A")"; then
    TAR_A_WALL=$LAST_WALL
    TAR_A_RSS=$LAST_RSS_KB
else
    echo "  FAILED: tar A (exit $LAST_RC)"
fi
if run_cmd tar cf "$TAR_B" -C "$(dirname "$DIR_B")" "$(basename "$DIR_B")"; then
    TAR_B_WALL=$LAST_WALL
    TAR_B_RSS=$LAST_RSS_KB
else
    echo "  FAILED: tar B (exit $LAST_RC)"
fi
TAR_A_SZ=$(stat -c%s "$TAR_A" 2>/dev/null || echo 0)
if [ "$TAR_A_SZ" -gt "$TWO_GB" ]; then
    echo "  SKIPPED — tar of dir A is $(fmt_size "$TAR_A_SZ"), exceeds zstd 2 GB limit"
else
    ZSTD_PATCH="$TMPDIR/b_delta.tar.zst"
    if run_cmd zstd -T0 --patch-from="$TAR_A" "$TAR_B" -o "$ZSTD_PATCH"; then
        # Total compress = tar A + tar B + zstd
        Z_COMP_WALL=$(awk "BEGIN{printf \"%.1f\", ${TAR_A_WALL:-0} + ${TAR_B_WALL:-0} + $LAST_WALL}")
        Z_COMP_RSS=$LAST_RSS_KB
        [ "${TAR_A_RSS:-0}" -gt "$Z_COMP_RSS" ] && Z_COMP_RSS=${TAR_A_RSS}
        [ "${TAR_B_RSS:-0}" -gt "$Z_COMP_RSS" ] && Z_COMP_RSS=${TAR_B_RSS}
        Z_PATCH_SZ=$(stat -c%s "$ZSTD_PATCH")
        Z_COMP_OK=1
        print_summary "tar + zstd delta compress" "$Z_COMP_WALL" "$Z_COMP_RSS" "$Z_PATCH_SZ" "$SZ_B"
    else
        echo "  FAILED (exit $LAST_RC)"
    fi
fi

# ── 5. zstd delta decompress (zstd + untar) ──

echo ""
echo "=== 5. zstd delta decompress (zstd + untar) ==="
if [ "$Z_COMP_OK" -eq 1 ]; then
    ZSTD_RESTORED_TAR="$TMPDIR/b_restored.tar"
    if run_cmd zstd -d --long=31 --memory=2048MB --patch-from="$TAR_A" "$ZSTD_PATCH" -o "$ZSTD_RESTORED_TAR"; then
        ZSTD_DEC_WALL=$LAST_WALL
        ZSTD_DEC_RSS=$LAST_RSS_KB
        ZSTD_RESTORED="$TMPDIR/zstd_restored"
        mkdir -p "$ZSTD_RESTORED"
        # Time untar too
        if run_cmd tar xf "$ZSTD_RESTORED_TAR" -C "$ZSTD_RESTORED"; then
            Z_DEC_WALL=$(awk "BEGIN{printf \"%.1f\", $ZSTD_DEC_WALL + $LAST_WALL}")
            Z_DEC_RSS=$ZSTD_DEC_RSS
            [ "$LAST_RSS_KB" -gt "$Z_DEC_RSS" ] && Z_DEC_RSS=$LAST_RSS_KB
            Z_DEC_SZ=$(dir_size "$ZSTD_RESTORED")
            Z_DEC_OK=1
            print_summary "zstd delta + untar" "$Z_DEC_WALL" "$Z_DEC_RSS" "$Z_DEC_SZ" ""
        else
            echo "  FAILED: untar (exit $LAST_RC)"
        fi
    else
        echo "  FAILED: zstd decompress (exit $LAST_RC)"
    fi
else
    echo "  SKIPPED (zstd compress was skipped or failed)"
fi

# ── Summary table ──

echo ""
echo "=== Summary ==="
printf "  %-30s %12s %12s %12s %10s\n" "Task" "Wall Time" "Peak RSS" "Output" "Ratio"
printf "  %-30s %12s %12s %12s %10s\n" "------------------------------" "------------" "------------" "------------" "----------"

if [ "$Q_COMP_OK" -eq 1 ]; then
    Q_RATIO=$(awk "BEGIN{printf \"%.2fx\", $SZ_B/$Q_PATCH_SZ}")
    printf "  %-30s %12s %12s %12s %10s\n" \
        "quine compress" "$(fmt_wall "$Q_COMP_WALL")" "$(fmt_size $((Q_COMP_RSS * 1024)))" "$(fmt_size "$Q_PATCH_SZ")" "$Q_RATIO"
else
    printf "  %-30s %12s\n" "quine compress" "FAILED"
fi

if [ "$Q_ZIP_OK" -eq 1 ]; then
    QZ_RATIO=$(awk "BEGIN{printf \"%.2fx\", $SZ_B/$Q_ZIP_SZ}")
    Q_TOTAL_COMP_WALL=$(awk "BEGIN{printf \"%.1f\", $Q_COMP_WALL + $Q_ZIP_WALL}")
    printf "  %-30s %12s %12s %12s %10s\n" \
        "quine compress + zip" "$(fmt_wall "$Q_TOTAL_COMP_WALL")" "$(fmt_size $((Q_COMP_RSS * 1024)))" "$(fmt_size "$Q_ZIP_SZ")" "$QZ_RATIO"
else
    printf "  %-30s %12s\n" "quine compress + zip" "FAILED"
fi

if [ "$Q_DEC_OK" -eq 1 ]; then
    printf "  %-30s %12s %12s %12s %10s\n" \
        "unzip + quine decompress" "$(fmt_wall "$Q_DEC_WALL")" "$(fmt_size $((Q_DEC_RSS * 1024)))" "$(fmt_size "$Q_DEC_SZ")" "-"
else
    printf "  %-30s %12s\n" "unzip + quine decompress" "FAILED"
fi

if [ "$Z_COMP_OK" -eq 1 ]; then
    Z_RATIO=$(awk "BEGIN{printf \"%.2fx\", $SZ_B/$Z_PATCH_SZ}")
    printf "  %-30s %12s %12s %12s %10s\n" \
        "tar + zstd delta compress" "$(fmt_wall "$Z_COMP_WALL")" "$(fmt_size $((Z_COMP_RSS * 1024)))" "$(fmt_size "$Z_PATCH_SZ")" "$Z_RATIO"
else
    printf "  %-30s %12s\n" "tar + zstd delta compress" "FAILED"
fi

if [ "$Z_DEC_OK" -eq 1 ]; then
    printf "  %-30s %12s %12s %12s %10s\n" \
        "zstd delta + untar" "$(fmt_wall "$Z_DEC_WALL")" "$(fmt_size $((Z_DEC_RSS * 1024)))" "$(fmt_size "$Z_DEC_SZ")" "-"
else
    printf "  %-30s %12s\n" "zstd delta + untar" "FAILED"
fi
