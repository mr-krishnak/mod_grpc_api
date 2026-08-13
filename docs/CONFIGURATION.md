# Configuration reference

The module reads `autoload_configs/grpc_api.conf.xml`.

## Complete example

```xml
<configuration name="grpc_api.conf" description="Asynchronous FreeSWITCH gRPC API">
  <settings>
    <param name="listen-address" value="127.0.0.1:50051"/>
    <param name="worker-threads" value="8"/>
    <param name="max-concurrent-requests" value="64"/>
    <param name="queue-capacity" value="64"/>
    <param name="acceptor-count" value="8"/>
    <param name="max-response-bytes" value="8388608"/>
    <param name="shutdown-grace-ms" value="5000"/>
    <param name="enable-reflection" value="false"/>
    <param name="log-arguments" value="false"/>
    <param name="allowed-commands"
           value="status,version,show,bgapi,originate,uuid_*,module_exists,global_getvar"/>
  </settings>
</configuration>
```

## Settings

| Setting | Range | Reloadable | Description |
|---|---:|---|---|
| `listen-address` | nonempty, max 255 bytes | No | Address passed to gRPC `AddListeningPort`. Keep loopback unless protected externally. |
| `worker-threads` | `1..128` | No | Number of threads that execute FreeSWITCH API commands. |
| `max-concurrent-requests` | `0..10000` | Yes | Total executing plus queued requests. `0` disables the gate and is not recommended. |
| `queue-capacity` | `1..4096` | No | Maximum queued worker tasks. Submission is non-blocking. |
| `acceptor-count` | `1..64` | No | Number of outstanding async `RequestExecute` operations. |
| `max-response-bytes` | `4096..67108864` | No | Maximum command output retained before truncation. |
| `shutdown-grace-ms` | `0..60000` | Yes | Deadline supplied to gRPC server shutdown. |
| `enable-reflection` | boolean | No | Enables gRPC reflection when the reflection library was linked. |
| `log-arguments` | boolean | Yes | Logs sanitized/truncated arguments at debug level. Disabled by default. |
| `allowed-commands` | nonempty command list | Yes | Exact command names plus the special controlled `uuid_*` family rule. A bare or general wildcard is rejected. |

Accepted boolean values are `true/false`, `yes/no`, `on/off`, and `1/0`.

## Dynamic reload

After editing the configuration:

```text
grpc_api reload
```

The following settings apply immediately:

- `max-concurrent-requests`;
- `shutdown-grace-ms`;
- `log-arguments`;
- `allowed-commands`.

If a startup-only setting changed, the module logs that an unload/load cycle is required. The reload still applies the dynamic settings.

## Concurrency model

`max-concurrent-requests` counts requests from acceptance until their gRPC `Finish` completion is observed. This includes:

- commands currently executing in a worker;
- commands waiting in the worker queue;
- requests being completed with an error.

When the limit is reached, new calls finish quickly with gRPC `RESOURCE_EXHAUSTED`. The completion-queue thread never blocks waiting for queue space.

For `bgapi`, the initial gRPC request leaves the active-request count after its Job UUID response finishes. The queued background command continues to consume a worker or queue slot and is reported as `background_jobs` by `grpc_api status` until it completes or is canceled during shutdown.

Recommended starting values for a moderate installation:

```xml
<param name="worker-threads" value="8"/>
<param name="max-concurrent-requests" value="64"/>
<param name="queue-capacity" value="64"/>
<param name="acceptor-count" value="8"/>
```

Increase workers only after measuring FreeSWITCH CPU usage and p95/p99 latency. More workers are not always faster, especially for commands that contend on FreeSWITCH internals.

## Queue sizing

The concurrency gate is applied before queue submission. A practical configuration normally uses:

```text
max-concurrent-requests <= worker-threads + queue-capacity
```

If the queue fills first, the call receives `RESOURCE_EXHAUSTED` with `Worker queue full`.

## Command allowlist

Commands are normalized to lowercase and must contain only letters, digits, `_`, `-`, or `.`. Command names are limited to 128 bytes. Arguments are limited to 64 KiB. Embedded NUL bytes are rejected.

A comma-, semicolon-, or whitespace-separated list is accepted:

```xml
<param name="allowed-commands" value="status, version, show"/>
```

A bare wildcard (`*`) and general wildcard patterns are rejected. The only supported family rule is:

```xml
<param name="allowed-commands" value="status,version,uuid_*"/>
```

`uuid_*` permits FreeSWITCH command names beginning with `uuid_`, including `uuid_bridge`, `uuid_transfer`, `uuid_kill`, `uuid_broadcast`, and commands contributed by loaded modules. This is intentionally broad call-control authority. Replace it with exact UUID commands when the client needs only a small subset, for example:

```xml
<param name="allowed-commands"
       value="status,version,uuid_exists,uuid_bridge,uuid_kill"/>
```

### `bgapi` authorization

`bgapi` is implemented by this module as a background wrapper because it is an Event Socket operation rather than a normal `switch_api_execute()` command. Both levels must be allowed:

```xml
<param name="allowed-commands"
       value="bgapi,originate,uuid_bridge"/>
```

This permits requests such as:

```text
command:   bgapi
arguments: originate user/1001 &bridge(user/1002)
```

Nested `bgapi` is rejected. A background command is executed once by the owned worker pool; the immediate gRPC response contains a Job UUID and the final result is fired as a `BACKGROUND_JOB` event.

`bridge` itself is a dialplan application, not a standalone API command. Use it inside `originate ... &bridge(...)`, or use `uuid_bridge <uuid-a> <uuid-b>` when both channels already exist.

Hard-denied commands:

```text
bg_spawn
bg_system
eval
expand
fsctl
load
reload
reloadxml
sched_api
sched_del
sched_transfer
shutdown
spawn
spawn_stream
system
unload
unsched_api
```

The hard-denied list is defense in depth. It does not replace review of every explicitly allowed command and its arguments.

`originate` can invoke inline dialplan applications, and UUID commands such as `uuid_broadcast` can execute applications on live calls. Treat clients with these permissions as fully trusted FreeSWITCH control-plane clients.

## Response size

When command output exceeds `max-response-bytes`, the module truncates it, appends a marker when space permits, sets `truncated=true`, and returns the typed representation as plain data. This avoids parsing incomplete JSON or XML.

Structured parsing is also skipped when retained output exceeds 4 MiB. Very large tables can expand substantially when every row repeats protobuf map keys, so these responses use the `plain` oneof instead. After parsing smaller output, the module checks the serialized protobuf size and falls back to `plain` if needed.

In compatibility mode the response may contain both raw and structured copies. The gRPC send limit is sized for the bounded raw-plus-plain fallback. High-throughput clients should use raw-only or structured-only mode.

## Listener

Default:

```xml
<param name="listen-address" value="127.0.0.1:50051"/>
```

IPv6 loopback:

```xml
<param name="listen-address" value="[::1]:50051"/>
```

A non-loopback address produces a warning because transport is unauthenticated and unencrypted. See [SECURITY.md](../SECURITY.md).
