#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

python3 test/mock_api.py 8765 &
mock_pid=$!
tmp_output=$(mktemp)
trap 'kill "$mock_pid" 2>/dev/null || true; rm -f "$tmp_output"' EXIT

for _ in $(seq 1 50); do
  if curl -fsS -o /dev/null http://127.0.0.1:8765/ 2>/dev/null; then break; fi
  sleep 0.05
done

sqlite3 :memory: < test/test.sql > "$tmp_output"
diff -u test/expected.out "$tmp_output"
echo "sqlite-jev: all tests passed"

