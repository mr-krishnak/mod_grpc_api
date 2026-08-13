# Troubleshooting

## CMake cannot find `switch.h`

Pass the directory containing `switch.h`:

```bash
cmake -S . -B build \
  -DFREESWITCH_INCLUDE_DIR=/actual/path/include/freeswitch \
  -DFREESWITCH_MODULE_DIR=/actual/path/mod \
  -DFREESWITCH_CONFIG_DIR=/actual/path/autoload_configs
```

Locate it with:

```bash
find /usr /usr/local -name switch.h 2>/dev/null
```

## `grpc_cpp_plugin was not found`

Install the distribution package that provides the gRPC C++ protoc plugin. On
Debian/Ubuntu this is normally `protobuf-compiler-grpc`.

```bash
command -v grpc_cpp_plugin
```

If it is installed in a nonstandard location, add that directory to `PATH`
before running CMake.

## `Package grpc++ was not found`

Verify the package metadata:

```bash
pkg-config --modversion grpc++
pkg-config --cflags --libs grpc++
```

Set `PKG_CONFIG_PATH` to the directory containing `grpc++.pc` when gRPC was
installed under a custom prefix.

## FreeSWITCH reports `module load file routine returned an error`

Check:

```bash
ldd /path/to/mod/mod_grpc_api.so
```

Fix any missing library. Also verify that the module was compiled against a
compatible FreeSWITCH installation and that `grpc_api.conf.xml` is in the active
`autoload_configs` directory.

## Server does not start

Inspect the FreeSWITCH log for one of these common causes:

- Invalid XML or configuration value.
- Port already in use.
- Reflection enabled but `grpc++_reflection` was not linked.
- Listener address not supported by the installed gRPC version.
- Worker thread creation failure.

Check the port:

```bash
ss -ltnp | grep 50051
```

## `PERMISSION_DENIED`

The command is not present in `allowed-commands`, or it is always hard-denied.
Edit `grpc_api.conf.xml` and apply:

```bash
fs_cli -x "grpc_api reload"
fs_cli -x "grpc_api status"
```

Do not add commands broadly just to remove the error. Add the smallest exact
set required by the client. `uuid_*` is the only supported family rule and
should be replaced with exact `uuid_` command names when practical.

For `bgapi`, both levels are authorized. The allowlist must contain `bgapi`
and the nested command. For example:

```xml
<param name="allowed-commands" value="bgapi,originate"/>
```

permits `bgapi originate ...`, while allowing `bgapi` alone does not.

## `bgapi` returns a Job UUID but no final result

That is expected. The gRPC method is unary and returns as soon as the nested
command is queued. Subscribe to FreeSWITCH `BACKGROUND_JOB` events with an
Event Socket client or another event consumer, then correlate the event's
`Job-UUID` with the UUID returned by gRPC. The command output is in the event
body, and this module adds `Job-Success` and `Job-Truncated` headers.

Check current jobs with:

```bash
fs_cli -x "grpc_api status"
```

The `background_jobs` value includes queued and running jobs owned by this
module.

## `bridge` reports command not found

`bridge` is a dialplan application, not a standalone FreeSWITCH API command.
Use one of these forms:

```text
bgapi originate user/1001 &bridge(user/1002)
uuid_bridge <existing-uuid-a> <existing-uuid-b>
```

The first requires `bgapi` and `originate` in the allowlist. The second
requires `uuid_bridge` or the broader `uuid_*` rule.

## `RESOURCE_EXHAUSTED`

The active-request limit or worker queue is full. Check:

```bash
fs_cli -x "grpc_api status"
```

Reduce client concurrency first. Then measure the command latency and
FreeSWITCH load before increasing workers or limits.

## grpcurl says reflection is unavailable

Use the included proto:

```bash
grpcurl -plaintext -import-path . -proto freeswitch_api.proto \
  -d '{"command":"status"}' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

Or enable reflection in the config and restart the module, provided the build
found `grpc++_reflection`.

## Unload takes longer than the shutdown grace

The gRPC deadline stops/cancels RPC transport, but a FreeSWITCH API command
already running in a worker may not be interruptible. The module waits for those
workers before allowing its code to unload. Running `bgapi` jobs are also
joined before unload; queued background jobs are canceled and publish a final
`BACKGROUND_JOB` error event. Identify and remove long-running or blocking
commands from the allowlist.

## Response is truncated

Increase `max-response-bytes` cautiously and restart the module, or request a
smaller result. Prefer `RESPONSE_MODE_RAW_ONLY` or
`RESPONSE_MODE_STRUCTURED_ONLY` to avoid duplicated response data.
