#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${TARGET:-127.0.0.1:50051}"
COMMAND="${1:-version}"
ARGUMENTS="${2:-}"
MODE="${3:-RESPONSE_MODE_RAW_ONLY}"

command -v grpcurl >/dev/null 2>&1 || {
  echo "grpcurl is required" >&2
  exit 127
}

PAYLOAD="$(python3 - "$COMMAND" "$ARGUMENTS" "$MODE" <<'PY'
import json
import sys
print(json.dumps({
    "command": sys.argv[1],
    "arguments": sys.argv[2],
    "responseMode": sys.argv[3],
}))
PY
)"

grpcurl -plaintext \
  -import-path "$ROOT_DIR" \
  -proto freeswitch_api.proto \
  -d "$PAYLOAD" \
  "$TARGET" fsgrpc.FreeSwitchApi/Execute
