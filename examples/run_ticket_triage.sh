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
  echo "Missing TYPESAFE_API_KEY in the environment" >&2
  exit 1
fi

sqlite3 :memory: < examples/ticket_triage.sql
