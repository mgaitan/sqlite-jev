#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

wheel=${1:-}
if [[ -z "$wheel" || ! -f "$wheel" ]]; then
  echo "usage: $0 path/to/sqlite_jev-*.whl" >&2
  exit 2
fi

venv=$(mktemp -d)
trap 'rm -rf "$venv"' EXIT

uv venv "$venv"
uv pip install --python "$venv/bin/python" "$wheel"
"$venv/bin/python" test/python_package.py
