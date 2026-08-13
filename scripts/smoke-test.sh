#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ADDRESS="${ADDRESS:-127.0.0.1:50051}"
COMMAND="${COMMAND:-status}"
ARGUMENTS="${ARGUMENTS:-}"
TIMEOUT="${TIMEOUT:-5s}"

if ! command -v grpcurl >/dev/null 2>&1; then
  echo "grpcurl is required for this smoke test" >&2
  exit 2
fi

payload="$(python3 - "$COMMAND" "$ARGUMENTS" <<'PY'
import json
import sys
print(json.dumps({
    "command": sys.argv[1],
    "arguments": sys.argv[2],
    "responseMode": "RESPONSE_MODE_RAW_ONLY",
}))
PY
)"

grpcurl -plaintext \
  -max-time "${TIMEOUT%s}" \
  -import-path "${ROOT_DIR}" \
  -proto freeswitch_api.proto \
  -d "${payload}" \
  "${ADDRESS}" \
  fsgrpc.FreeSwitchApi/Execute
