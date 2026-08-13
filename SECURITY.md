# Security guidance

## Default posture

`mod_grpc_api` invokes FreeSWITCH API handlers inside the FreeSWITCH process.
Treat access to the endpoint as privileged control-plane access.

The module currently uses `grpc::InsecureServerCredentials()`. It does not
provide TLS, client certificates, bearer tokens, user identities, or per-client
authorization. The supplied configuration therefore listens only on:

```text
127.0.0.1:50051
```

## Do not expose the listener directly

Do not change the listener to `0.0.0.0`, `[::]`, or a public address unless an
external security layer provides authentication, authorization, encryption,
rate limiting, and network filtering. Suitable deployment patterns include:

- an authenticated TLS/mTLS reverse proxy that supports gRPC/HTTP2;
- a private VPN or service mesh with workload identity;
- a host-local sidecar reachable only by the trusted application;
- host firewall rules that restrict source addresses.

Encryption alone is not sufficient when every network peer can execute
administrative commands.

## Use a narrow command allowlist

The supplied configuration supports status inspection and the requested call-control workflows:

```text
status,version,show,bgapi,originate,uuid_*,module_exists,global_getvar
```

This is privileged access. `originate` can run inline dialplan applications,
and `uuid_*` includes mutating commands such as `uuid_bridge`, `uuid_transfer`,
`uuid_kill`, and `uuid_broadcast`. `uuid_dump` and `global_getvar` may return
operational or sensitive values. Remove commands your application does not
need.

A bare wildcard and general wildcard patterns are deliberately rejected. The
only family rule is `uuid_*`. It exists for call-control deployments that need
many UUID commands, but exact entries are safer. For example, replace
`uuid_*` with `uuid_exists,uuid_bridge,uuid_kill` when that is sufficient.

`bgapi` does not bypass authorization. The module checks both the outer
`bgapi` command and its nested API command against the same allowlist. Nested
`bgapi` is rejected. Nevertheless, allowing `originate` or broad UUID commands
must be limited to fully trusted clients and documented workflows.

The module also hard-denies command names that can directly affect the core,
load modules, schedule nested API work, expand another command, or execute host
processes:

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

This list is defense in depth. The explicit allowlist remains the primary
authorization boundary.

The string `bridge` is a dialplan application, not a standalone API command.
Allowing `originate` permits syntax such as `originate ... &bridge(...)`.
Allowing `uuid_bridge` permits bridging two existing channel UUIDs.

## Input and output controls

The server rejects empty or malformed command names, embedded NUL bytes,
arguments larger than 64 KiB, and requests beyond the configured concurrency
or queue limits. Command names are limited to 128 ASCII characters.

`max-response-bytes` bounds the output retained and serialized by the module.
The underlying FreeSWITCH API implementation may allocate its output before the
module can truncate it, so do not allow commands capable of producing
unbounded output for untrusted callers.

Background job output is placed in the FreeSWITCH event bus as a
`BACKGROUND_JOB` event. Event subscribers must be authorized and logs/event
storage must be treated as sensitive because call-control output can contain
phone numbers, routing data, UUIDs, and hangup causes.

## Sensitive data and logging

`log-arguments` defaults to `false`. Arguments and responses can contain phone
numbers, UUIDs, credentials, tokens, routing data, and customer information.
When argument logging is enabled for troubleshooting, values are sanitized and
limited to 256 bytes, but the resulting logs must still be treated as
sensitive.

## Operational recommendations

- Keep concurrency, queue, and response limits bounded.
- Set client deadlines and bounded retry policies.
- Monitor `RESOURCE_EXHAUSTED`, `PERMISSION_DENIED`, and unusual command rates.
- Keep FreeSWITCH, gRPC, Protobuf, and the host operating system patched.
- Review the allowlist whenever workflows or loaded FreeSWITCH modules change.
- Test load, unload, and restart behavior in staging before production rollout.

## Reporting vulnerabilities

Report suspected vulnerabilities privately to the repository maintainer. Do
not include production credentials, call records, or exploit details in a
public issue.
