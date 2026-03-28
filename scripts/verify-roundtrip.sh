#!/bin/bash
#
# verify-roundtrip.sh — compress, decompress, and compare
#
# Usage:
#   ./scripts/verify-roundtrip.sh <dir_a> <dir_b> [max_mem]
#
# Arguments:
#   dir_a    reference directory
#   dir_b    target directory
#   max_mem  optional peak RSS limit for decompress (default: 10M)
#
# The script exits non-zero if any step fails.
#

set -euo pipefail

QUINE="${QUINE:-build/quine}"

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
    echo "Usage: $0 <dir_a> <dir_b> [max_mem]" >&2
    echo "  max_mem: peak RSS limit for decompress (default: 10M)" >&2
    exit 1
fi

DIR_A="$1"
DIR_B="$2"
MAX_MEM="${3:-10M}"

TMPDIR=$(mktemp -d /tmp/quine_verify_XXXXXX)
PATCH="$TMPDIR/output.patch"
RESTORED="$TMPDIR/restored"

cleanup() {
    rm -rf "$TMPDIR"
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

echo ""
echo "=== All steps passed ==="
