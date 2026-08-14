#!/bin/sh
# Remove one explicitly named artifact below this repository's scratch tree.
# Refuses the scratch root itself and every path outside it.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
[ "$#" -eq 1 ] || {
    echo "usage: tools/cleanup_scratch_path.sh <scratch/path>" >&2
    exit 2
}

target=$(realpath -m "$1")
scratch=$(realpath -m "$REPO/scratch")
case "$target" in
    "$scratch"/*) ;;
    *)
        echo "REFUSING: cleanup target is outside $scratch: $target" >&2
        exit 2
        ;;
esac

rm -rf -- "$target"
