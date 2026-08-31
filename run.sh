#!/bin/sh
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$repo"
exec uv run --frozen python bootstrap.py "$@"
