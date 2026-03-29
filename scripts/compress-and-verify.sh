#!/bin/bash
#
# compress-and-verify.sh — compress, zip, unzip, decompress, compare
#
# Usage:
#   ./scripts/compress-and-verify.sh <dir_a> <dir_b> [patch_file] [max_mem]
#
# Arguments:
#   dir_a       reference directory
#   dir_b       target directory
#   patch_file  output patch file (kept on success with .zip alongside);
#               if omitted, uses temp files for test-only verification
#   max_mem     optional peak RSS limit for decompress (default: 10M)
#
# Steps:
#   1. quine compress → patch_file
#   2. zip patch_file → patch_file.zip
#   3. unzip patch_file.zip → temp patch
#   4. quine decompress temp patch
#   5. compare dir_b vs restored
#
# The script exits non-zero if any step fails.
#

set -euo pipefail

QUINE="${QUINE:-build/quine}"

if [ $# -lt 2 ] || [ $# -gt 4 ]; then
    echo "Usage: $0 <dir_a> <dir_b> [patch_file] [max_mem]" >&2
    echo "  patch_file: output patch (omit for test-only run)" >&2
    echo "  max_mem:    peak RSS limit for decompress (default: 10M)" >&2
    exit 1
fi

DIR_A="$1"
DIR_B="$2"

# Determine if patch_file was given or if we use a temp file
KEEP_PATCH=0
if [ $# -ge 3 ] && [[ "$3" != [0-9]* ]]; then
    PATCH="$3"
    # Ensure .qn extension
    [[ "$PATCH" != *.qn ]] && PATCH="${PATCH}.qn"
    KEEP_PATCH=1
    MAX_MEM="${4:-10M}"
else
    MAX_MEM="${3:-10M}"
    PATCH=""
fi

TMPDIR=$(mktemp -d /tmp/quine_verify_XXXXXX)
RESTORED="$TMPDIR/restored"
VERIFIED=0

if [ -z "$PATCH" ]; then
    PATCH="$TMPDIR/output.qn"
fi

ZIP_FILE="${PATCH}.zip"

cleanup() {
    rm -rf "$TMPDIR"
    if [ "$KEEP_PATCH" -eq 1 ] && [ "$VERIFIED" -eq 0 ]; then
        [ -f "$PATCH" ] && echo "Verification failed — removing $PATCH" >&2 && rm -f "$PATCH"
        [ -f "$ZIP_FILE" ] && rm -f "$ZIP_FILE"
    fi
}
trap cleanup EXIT

fmt_bytes() {
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

echo "=== Step 1: Compress ==="
"$QUINE" compress "$DIR_A" "$DIR_B" "$PATCH"
PATCH_SZ=$(stat -c%s "$PATCH")

echo ""
echo "=== Step 2: Zip ==="
echo "  zip $ZIP_FILE ..."
ZIP_START=$(date +%s%N)
zip -j -q "$ZIP_FILE" "$PATCH"
ZIP_END=$(date +%s%N)
ZIP_WALL=$(awk "BEGIN{printf \"%.1f\", ($ZIP_END - $ZIP_START) / 1000000000}")
ZIP_SZ=$(stat -c%s "$ZIP_FILE")
DIR_B_SZ=$(du -sb "$DIR_B" | awk '{print $1}')
ZIP_RATIO=$(awk "BEGIN{printf \"%.2f\", $DIR_B_SZ / $ZIP_SZ}")
ZIP_SAVINGS=$(awk "BEGIN{printf \"%.0f\", 100*(1 - $ZIP_SZ/$DIR_B_SZ)}")
echo "  patch size:    $(fmt_bytes "$PATCH_SZ")"
echo "  zip size:      $(fmt_bytes "$ZIP_SZ")"
echo "  zip ratio:     ${ZIP_RATIO}x  (${ZIP_SAVINGS}% savings vs dir B)"
echo "  zip time:      ${ZIP_WALL}s"

echo ""
echo "=== Step 3: Unzip + Decompress (--verify-max-mem=$MAX_MEM) ==="
UNZIPPED_PATCH="$TMPDIR/unzipped.qn"
echo "  unzip $ZIP_FILE ..."
UNZIP_START=$(date +%s%N)
unzip -o -q "$ZIP_FILE" -d "$TMPDIR"
# zip -j stores just the filename, so the extracted file is in TMPDIR
UNZIPPED_PATCH="$TMPDIR/$(basename "$PATCH")"
UNZIP_END=$(date +%s%N)
UNZIP_WALL=$(awk "BEGIN{printf \"%.1f\", ($UNZIP_END - $UNZIP_START) / 1000000000}")
echo "  unzip time:    ${UNZIP_WALL}s"
echo ""
"$QUINE" decompress --verify-max-mem="$MAX_MEM" "$DIR_A" "$UNZIPPED_PATCH" "$RESTORED"

echo ""
echo "=== Step 4: Compare ==="
echo "  diff -rq $DIR_B $RESTORED"
CMP_START=$(date +%s%N)
diff -rq "$DIR_B" "$RESTORED"
CMP_END=$(date +%s%N)
CMP_WALL=$(awk "BEGIN{printf \"%.1f\", ($CMP_END - $CMP_START) / 1000000000}")
echo "  OK: directories are identical  (${CMP_WALL}s)"

VERIFIED=1
echo ""
if [ "$KEEP_PATCH" -eq 1 ]; then
    echo "=== All steps passed ==="
    echo "  patch: $PATCH ($(fmt_bytes "$PATCH_SZ"))"
    echo "  zip:   $ZIP_FILE ($(fmt_bytes "$ZIP_SZ"))"
else
    echo "=== All steps passed ==="
fi
