#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

bash -n "${ROOT_DIR}/scripts/build-install.sh"
bash -n "${ROOT_DIR}/scripts/smoke-test.sh"
bash -n "${ROOT_DIR}/examples/grpcurl.sh"
node --check "${ROOT_DIR}/scripts/stress-test.js"
node --check "${ROOT_DIR}/examples/node/client.js"

python3 - "${ROOT_DIR}/examples/python/client.py" <<'PY'
import ast
import pathlib
import sys
ast.parse(pathlib.Path(sys.argv[1]).read_text(), filename=sys.argv[1])
print("Python example syntax: OK")
PY

python3 - "${ROOT_DIR}/conf/autoload_configs/grpc_api.conf.xml" <<'PY'
import re
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
params = [item.get("name") for item in root.findall("./settings/param")]
duplicates = sorted({name for name in params if params.count(name) > 1})
if duplicates:
    raise SystemExit("Duplicate configuration parameters: " + ", ".join(duplicates))
allowed = root.find("./settings/param[@name='allowed-commands']")
if allowed is None or not (allowed.get("value") or "").strip():
    raise SystemExit("allowed-commands must be present and nonempty")
tokens = {
    token.lower()
    for token in re.split(r"[,;\s]+", allowed.get("value") or "")
    if token
}
required = {"bgapi", "originate", "uuid_*"}
missing = sorted(required - tokens)
if missing:
    raise SystemExit("Default allowlist is missing: " + ", ".join(missing))
invalid_wildcards = sorted(
    token for token in tokens if "*" in token and token != "uuid_*"
)
if invalid_wildcards:
    raise SystemExit(
        "Unsupported wildcard rules: " + ", ".join(invalid_wildcards)
    )
print("XML configuration: OK")
PY

python3 - "${ROOT_DIR}/VERSION" "${ROOT_DIR}/CMakeLists.txt" <<'PY'
import pathlib
import re
import sys
version = pathlib.Path(sys.argv[1]).read_text().strip()
cmake = pathlib.Path(sys.argv[2]).read_text()
match = re.search(r"project\(mod_grpc_api VERSION ([0-9.]+)", cmake)
if not match or match.group(1) != version:
    raise SystemExit("VERSION and CMake project version do not match")
print("Version metadata: OK")
PY

python3 - "${ROOT_DIR}/mod_grpc_api.cpp" <<'PY'
import pathlib
import sys
text = pathlib.Path(sys.argv[1]).read_text()
required = [
    'rule == "uuid_*"',
    'output->command == "bgapi"',
    'SWITCH_EVENT_BACKGROUND_JOB',
    '"Job-UUID"',
    '"Job-Success"',
    'execute_bgapi_request',
]
missing = [marker for marker in required if marker not in text]
if missing:
    raise SystemExit("Missing bgapi/uuid implementation markers: " + ", ".join(missing))
print("bgapi and uuid family markers: OK")
PY

python3 - "${ROOT_DIR}/examples/grpcurl" <<'PY'
import json
import pathlib
import sys
for path in pathlib.Path(sys.argv[1]).glob("*.json"):
    json.loads(path.read_text())
print("grpcurl JSON examples: OK")
PY

python3 - "${ROOT_DIR}/freeswitch_api.proto" <<'PY'
import pathlib
import sys
text = pathlib.Path(sys.argv[1]).read_text()
required = [
    "service FreeSwitchApi",
    "rpc Execute",
    "string command = 1;",
    "string arguments = 2;",
    "ResponseMode response_mode = 3;",
    "bool success = 1;",
    "string message = 2;",
    "bool truncated = 7;",
]
missing = [item for item in required if item not in text]
if missing:
    raise SystemExit("Missing expected proto declarations: " + ", ".join(missing))
print("Proto compatibility markers: OK")
PY

python3 - "${ROOT_DIR}" <<'PY'
import pathlib
import re
import sys
root = pathlib.Path(sys.argv[1])
missing = []
unbalanced = []
for markdown in root.rglob("*.md"):
    text = markdown.read_text()
    if text.count("```") % 2:
        unbalanced.append(str(markdown.relative_to(root)))
    for target in re.findall(r"\[[^\]]+\]\(([^)]+)\)", text):
        if target.startswith(("http://", "https://", "mailto:", "#")):
            continue
        path = (markdown.parent / target.split("#", 1)[0]).resolve()
        if target and not path.exists():
            missing.append(f"{markdown.relative_to(root)} -> {target}")
if missing:
    raise SystemExit("Broken local Markdown links:\n" + "\n".join(missing))
if unbalanced:
    raise SystemExit("Unbalanced Markdown code fences:\n" + "\n".join(unbalanced))
print("Local Markdown links and code fences: OK")
PY

echo "Source-level checks passed"
