#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ -f .env ]]; then
  set -a
  # shellcheck disable=SC1091
  source .env
  set +a
fi

if [[ -z "${TYPESAFE_API_KEY:-}" ]]; then
  echo "Missing TYPESAFE_API_KEY" >&2
  exit 1
fi

output=$(sqlite3 :memory: < test/live_smoke.sql)
expected=$'2|1|1|1\n1|2'

if [[ "$output" != "$expected" ]]; then
  echo "Unexpected live integration result:" >&2
  printf '%s\n' "$output" >&2
  exit 1
fi

echo "sqlite-jev: live TypeSafe integration passed"

