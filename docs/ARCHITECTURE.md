# Architecture

The module intentionally uses a small async design rather than a multi-queue or
reactor framework.

```text
FreeSWITCH module runtime thread
  └─ one gRPC ServerCompletionQueue
       ├─ N outstanding RequestExecute acceptors
       ├─ validation + command authorization
       ├─ active-request concurrency gate
       └─ non-blocking submission to bounded worker queue

Worker pool
  ├─ normal RPC: switch_api_execute(command, arguments)
  │    ├─ response classification/parsing
  │    └─ asynchronous responder.Finish(...)
  └─ bgapi wrapper
       ├─ queue one nested authorized API command
       ├─ return a Job UUID immediately
       └─ fire BACKGROUND_JOB when the nested command completes
```

## Why one completion queue

The completion-queue thread performs only short operations: accepting tags,
validation, accounting, and queue submission. FreeSWITCH API execution and
response parsing run in the worker pool. A single CQ keeps lifecycle and request
ownership simple; more CQs should be added only when profiling proves this
thread is the bottleneck.

## Request lifecycle

1. A pending `CallData` accepts an RPC.
2. The runtime immediately posts a replacement acceptor unless shutdown began.
3. The request passes the concurrency gate.
4. Command and arguments are validated.
5. The command name is checked against exact allowlist entries or the
   controlled `uuid_*` family rule.
6. A task is pushed to the bounded queue without blocking.
7. A worker executes the FreeSWITCH API command.
8. The worker starts asynchronous `Finish`.
9. The CQ receives the finish tag, decrements active requests once, and deletes
   `CallData`.

The accounting release occurs only at step 9, preventing double decrement when
queue submission fails.

## Background command lifecycle

1. The outer request must be allowed as `bgapi`.
2. A worker parses the first argument token as the nested command.
3. Nested `bgapi` is rejected and the nested command is independently checked
   against the same hard-deny and allowlist rules.
4. A Job UUID is generated and a second task is submitted to the same bounded
   worker queue without blocking.
5. The outer gRPC request finishes with `+OK Job-UUID: ...`.
6. The background task executes `switch_api_execute()` exactly once.
7. Its output is bounded and published in a FreeSWITCH `BACKGROUND_JOB` event.

The module does not create detached threads for `bgapi`. Reusing the owned
worker pool keeps queue limits and module unload deterministic. Background jobs
are not counted as active gRPC requests after the Job UUID response finishes,
but they remain visible as `background_jobs` in module status and continue to
occupy worker/queue capacity.

## Overload behavior

- Concurrency gate full: fail immediately with `RESOURCE_EXHAUSTED`.
- Worker queue full: fail immediately with `RESOURCE_EXHAUSTED`.
- Background-job submission full: the `bgapi` RPC fails immediately with
  `RESOURCE_EXHAUSTED`; no Job UUID is returned.
- The CQ thread never waits for queue space.

## Shutdown flow

1. Atomically mark the module as shutting down.
2. Stop accepting worker tasks.
3. Complete queued RPC tasks with `UNAVAILABLE`; queued background jobs publish
   a canceled `BACKGROUND_JOB` event.
4. Call `Server::Shutdown` with the configured deadline.
5. Join workers that were already executing FreeSWITCH API commands.
6. Shut down the completion queue.
7. Drain every remaining tag.
8. Destroy the worker pool, server, and CQ while the async service is still
   alive in the runtime function.

Waiting for running workers is deliberate: unloading a shared library while its
code is executing would be unsafe.

## Response memory bounds

Raw FreeSWITCH output is capped by `max-response-bytes`. Structured parsing is skipped above 4 MiB, and smaller parsed responses are checked with protobuf's serialized-size calculation. If a table/map representation expands beyond the wire budget, it is replaced with the `plain` oneof before `Finish`.
