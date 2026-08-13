# Changelog

## 1.1.1 - 2026-08-12

### Call-control support

- Added native `bgapi` handling instead of passing `bgapi` incorrectly to
  `switch_api_execute()`.
- `bgapi` now returns a Job UUID immediately and publishes the nested command
  result as a FreeSWITCH `BACKGROUND_JOB` event.
- Added independent authorization for the nested `bgapi` command and rejected
  nested `bgapi` wrappers.
- Added the controlled `uuid_*` allowlist rule for the FreeSWITCH UUID command
  family, including `uuid_bridge`.
- Added `originate` to the supplied allowlist so trusted clients can run
  `bgapi originate ... &bridge(...)`.
- Kept background work in the existing bounded worker pool so queue overload,
  cancellation, shutdown, and module unload remain deterministic.
- Added `background_jobs` to `grpc_api status`.
- Added grpcurl, Node.js, Python, authorization, security, and troubleshooting
  documentation for background originate/bridge and `uuid_bridge`.

## 1.1.0 - 2026-08-12

### Correctness and lifecycle

- Fixed request accounting so every accepted call is incremented and released exactly once.
- Removed the queue-submission double-decrement path.
- Protected runtime configuration and active request state from reload races.
- Added a deterministic shutdown sequence: stop queue intake, cancel queued tasks, shut down the gRPC server, join running workers, shut down and drain the completion queue.
- Prevented queued calls from being deleted without a gRPC completion.
- Kept the gRPC service alive until the server is destroyed.
- Added exception handling for worker tasks and acceptor allocation.

### Performance and overload behavior

- Preserved the asynchronous gRPC architecture.
- Replaced blocking queue submission with a bounded non-blocking queue.
- Added configurable pre-posted acceptors.
- Added configurable executing-plus-queued concurrency control.
- Added raw-only and structured-only response modes to avoid unnecessary parsing/copying.
- Removed automatic command retries; every RPC now executes the supplied command exactly once.
- Added response truncation and explicit send/receive limits.
- Bounded structured parsing and added a serialized-size fallback for highly expansive table/map responses.

### Security

- Changed the default listener from wildcard to loopback.
- Added a required explicit command allowlist; wildcard configuration is rejected.
- Added a hard-denied list for lifecycle, shell, expansion, and scheduling commands.
- Rejected embedded NUL bytes, malformed command names, and oversized arguments.
- Disabled argument logging by default and sanitized debug values.
- Made reflection optional and disabled by default.

### Response behavior

- Treats FreeSWITCH output beginning with `-ERR` or `-USAGE` as command failure.
- Uses gRPC status codes for invalid, unauthorized, overloaded, shutting-down, and internal error conditions.
- Preserves original protobuf field numbers for wire compatibility.
- Uses `cJSON_free()` for cJSON-allocated serialized values.
- Corrected two-column table detection so table rows are not collapsed into a key/value record.

### Build and documentation

- Added configurable FreeSWITCH include/module/config installation paths.
- Installs `grpc_api.conf.xml` with the module.
- Added dependency checks for `protoc` and `grpc_cpp_plugin`.
- Corrected CMake `protoc` option quoting so generated files use the intended build directory.
- Added installation, configuration, usage, security, grpcurl, Node.js, Python, and stress-test documentation.
