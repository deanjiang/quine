#!/bin/bash
#
# compress-and-verify.sh — compress, decompress, compare, keep patch on success
#
# Usage:
#   ./scripts/compress-and-verify.sh <dir_a> <dir_b> [patch_file] [max_mem]
#
# Arguments:
#   dir_a       reference directory
#   dir_b       target directory
#   patch_file  output patch file (kept on success, removed on failure)
#               if omitted, uses a temp file that is cleaned up after
#   max_mem     optional peak RSS limit for decompress (default: 10M)
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
    PATCH="$TMPDIR/output.patch"
fi

cleanup() {
    rm -rf "$TMPDIR"
    if [ "$KEEP_PATCH" -eq 1 ] && [ "$VERIFIED" -eq 0 ] && [ -f "$PATCH" ]; then
        echo "Verification failed — removing $PATCH" >&2
        rm -f "$PATCH"
    fi
}
trap cleanup EXIT

echo "=== Step 1: Compress ==="
"$QUINE" compress "$DIR_A" "$DIR_B" "$PATCH"

echo ""
echo "=== Step 2: Decompress (--verify-max-mem=$MAX_MEM) ==="
"$QUINE" decompress --verify-max-mem="$MAX_MEM" "$DIR_A" "$PATCH" "$RESTORED"

echo ""
echo "=== Step 3: Compare ==="
"$QUINE" compare "$DIR_B" "$RESTORED"

VERIFIED=1
echo ""
if [ "$KEEP_PATCH" -eq 1 ]; then
    echo "=== All steps passed — patch kept: $PATCH ==="
else
    echo "=== All steps passed ==="
fi
