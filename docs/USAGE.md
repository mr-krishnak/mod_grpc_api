# Usage

## RPC method

```text
fsgrpc.FreeSwitchApi/Execute
```

Each RPC executes one allowed FreeSWITCH API command.

## Request fields

| Field | Type | Description |
|---|---|---|
| `command` | string | FreeSWITCH API command name, such as `status`, `version`, `uuid_bridge`, or the module-provided `bgapi` wrapper. |
| `arguments` | string | Arguments passed to the API command. For `bgapi`, this begins with the nested API command name. |
| `response_mode` | enum | Compatibility, raw-only, or structured-only output. |

## Response fields

| Field | Type | Description |
|---|---|---|
| `success` | bool | FreeSWITCH command success. `-ERR` and `-USAGE` prefixes produce `false`. |
| `message` | string | Raw command output when the selected mode includes raw data. |
| `format` | string | `json`, `xml`, `table`, or `plain`. |
| `table` | `TableData` | Parsed table/XML representation when selected. |
| `json` | `JsonData` | Parsed JSON representation when selected. |
| `plain` | `PlainData` | Plain response representation when selected. |
| `truncated` | bool | Output exceeded the configured response limit. |

`table`, `json`, and `plain` are a protobuf `oneof`, so at most one is returned. Output larger than 4 MiB, truncated output, or a parsed representation that would exceed the bounded wire size is returned through `plain` rather than expanded into table/JSON maps.

## grpcurl without reflection

Run these commands from the project root.

### FreeSWITCH status, raw-only

```bash
grpcurl -plaintext \
  -import-path . \
  -proto freeswitch_api.proto \
  -d '{
        "command": "status",
        "arguments": "",
        "responseMode": "RESPONSE_MODE_RAW_ONLY"
      }' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

### FreeSWITCH version, compatibility mode

Compatibility mode is the enum default, so `responseMode` may be omitted:

```bash
grpcurl -plaintext \
  -import-path . \
  -proto freeswitch_api.proto \
  -d '{"command":"version","arguments":""}' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

### Show channels, structured-only

```bash
grpcurl -plaintext \
  -import-path . \
  -proto freeswitch_api.proto \
  -d '{
        "command": "show",
        "arguments": "channels as json",
        "responseMode": "RESPONSE_MODE_STRUCTURED_ONLY"
      }' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

The module executes the supplied command and arguments exactly once. Request `channels as json` when native JSON output is desired, or use `channels` for the normal tabular output.

### UUID existence check

```bash
grpcurl -plaintext \
  -import-path . \
  -proto freeswitch_api.proto \
  -d '{
        "command": "uuid_exists",
        "arguments": "00000000-0000-0000-0000-000000000000",
        "responseMode": "RESPONSE_MODE_RAW_ONLY"
      }' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

### Bridge two existing channels

The `uuid_*` allowlist rule enables `uuid_bridge` and the other FreeSWITCH UUID API commands:

```bash
grpcurl -plaintext \
  -import-path . \
  -proto freeswitch_api.proto \
  -d '{
        "command": "uuid_bridge",
        "arguments": "11111111-1111-1111-1111-111111111111 22222222-2222-2222-2222-222222222222",
        "responseMode": "RESPONSE_MODE_RAW_ONLY"
      }' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

Both UUIDs must identify existing call legs. FreeSWITCH returns `-ERR` when either channel is unavailable or the bridge cannot be completed.

### Originate and bridge in the background

`bgapi` queues a nested allowed API command and returns immediately:

```bash
grpcurl -plaintext \
  -import-path . \
  -proto freeswitch_api.proto \
  -d '{
        "command": "bgapi",
        "arguments": "originate user/1001 &bridge(user/1002)",
        "responseMode": "RESPONSE_MODE_RAW_ONLY"
      }' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

Typical immediate response:

```json
{
  "success": true,
  "message": "+OK Job-UUID: 6a376b93-2aa8-4fa2-a260-63f2e8277886\n",
  "format": "plain",
  "plain": {
    "response": "+OK Job-UUID: 6a376b93-2aa8-4fa2-a260-63f2e8277886\n",
    "ok": true
  }
}
```

This response means the job was accepted, not that the originate or bridge succeeded. The module fires a FreeSWITCH `BACKGROUND_JOB` event when execution ends. The event includes:

```text
Job-UUID
Job-Command
Job-Command-Arg
Job-Success
Job-Truncated
```

The command output is the event body. Subscribe to `BACKGROUND_JOB` with an Event Socket client or a FreeSWITCH event consumer and correlate the event using `Job-UUID`. The unary gRPC service does not stream event results back to the caller.

Both `bgapi` and its nested command must be allowed. For the preceding example, the configuration needs at least:

```xml
<param name="allowed-commands" value="bgapi,originate"/>
```

Nested `bgapi` is rejected. `bridge` by itself is a dialplan application rather than a standalone API command; use `originate ... &bridge(...)` to create and join calls, or `uuid_bridge` to join two existing channels.

## grpcurl with reflection

When the module was linked with `grpc++_reflection` and configuration contains:

```xml
<param name="enable-reflection" value="true"/>
```

restart the module, then use:

```bash
grpcurl -plaintext 127.0.0.1:50051 list
grpcurl -plaintext 127.0.0.1:50051 describe fsgrpc.FreeSwitchApi
grpcurl -plaintext \
  -d '{"command":"version","responseMode":"RESPONSE_MODE_RAW_ONLY"}' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

## Node.js client

Install dependencies at the project root:

```bash
npm install
```

Run the included client:

```bash
node examples/node/client.js version
node examples/node/client.js status "" raw
node examples/node/client.js show "channels as json" structured
node examples/node/client.js bgapi "originate user/1001 &bridge(user/1002)" raw
```

Environment variables:

```bash
TARGET=127.0.0.1:50051 DEADLINE_MS=5000 \
  node examples/node/client.js version
```

Accepted mode names are `compat`, `raw`, and `structured`.

## Python client

Create an isolated environment, install dependencies, and generate client stubs:

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r examples/python/requirements.txt
mkdir -p examples/python/generated
python -m grpc_tools.protoc \
  -I. \
  --python_out=examples/python/generated \
  --grpc_python_out=examples/python/generated \
  freeswitch_api.proto
```

Run the client with the generated directory on `PYTHONPATH`:

```bash
PYTHONPATH=examples/python/generated \
  python examples/python/client.py version --raw

PYTHONPATH=examples/python/generated \
  python examples/python/client.py show channels as json --structured

PYTHONPATH=examples/python/generated \
  python examples/python/client.py bgapi \
    'originate user/1001 &bridge(user/1002)' --raw
```

The Python example sets an RPC deadline and returns a nonzero exit status for transport or command failure.

## Application behavior

### Always set a deadline

The module cannot cancel a FreeSWITCH API implementation that is already executing. Clients should set an RPC deadline appropriate for their application and avoid commands known to block indefinitely.

### Distinguish transport and command failures

A successful gRPC transport status does not guarantee that the FreeSWITCH command succeeded. Check `response.success`. For `bgapi`, `response.success=true` means the background task was queued; inspect the correlated `BACKGROUND_JOB` event and its `Job-Success` header for the nested command result.

The module uses gRPC status codes for request/infrastructure failures:

| gRPC status | Meaning |
|---|---|
| `OK` | Command ran; inspect `response.success`. |
| `INVALID_ARGUMENT` | Invalid command name or oversized/NUL-containing arguments. |
| `PERMISSION_DENIED` | Command is not in the allowlist or is hard-denied. |
| `RESOURCE_EXHAUSTED` | Concurrency limit or worker queue is full. |
| `UNAVAILABLE` | Module/server is shutting down. |
| `INTERNAL` | Unexpected worker failure. |

Retry `RESOURCE_EXHAUSTED` and `UNAVAILABLE` only with bounded exponential backoff and jitter. Do not retry command-level failures blindly because some allowed commands may not be idempotent.

## Response mode guidance

### Compatibility

Use when migrating an existing client or when both raw and parsed output are useful. It has the highest response-copying cost.

### Raw-only

Use for highest throughput, logging, or commands whose output format is already handled by the client.

### Structured-only

Use when the application consumes protobuf maps/rows and does not need the original text.

## Control command inside FreeSWITCH

```text
grpc_api help
grpc_api status
grpc_api reload
```

`grpc_api reload` does not restart the listener or worker pool. It reloads only the settings identified as dynamic in [CONFIGURATION.md](CONFIGURATION.md).
