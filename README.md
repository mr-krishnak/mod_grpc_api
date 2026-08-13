# mod_grpc_api for FreeSWITCH

`mod_grpc_api` exposes selected FreeSWITCH API commands through an asynchronous gRPC service.

This updated version keeps the original async design deliberately simple:

- one gRPC completion queue and one completion-queue polling thread;
- a fixed-size worker pool for `switch_api_execute()` calls;
- a bounded, non-blocking worker queue;
- a configurable number of pre-posted async acceptors;
- explicit overload responses instead of blocking the gRPC loop;
- deterministic request accounting and shutdown;
- a loopback-only default listener and command allowlist;
- native `bgapi` queuing with `BACKGROUND_JOB` event delivery;
- controlled `uuid_*` call-control authorization;
- bounded request and response sizes;
- raw-only and structured-only response modes to reduce copying.

## Important security note

The module currently uses gRPC insecure transport and does not authenticate clients. The supplied configuration listens on `127.0.0.1:50051` and uses an explicit command allowlist. Do not expose it directly to an untrusted network. See [SECURITY.md](SECURITY.md).

## Service definition

The service is:

```text
fsgrpc.FreeSwitchApi/Execute
```

Request:

```proto
message ApiRequest {
  string command = 1;
  string arguments = 2;
  ResponseMode response_mode = 3;
}
```

The response contains command success, raw output when requested, a detected format, one structured representation, and a `truncated` flag. Normal RPCs execute the supplied command exactly once. A `bgapi` RPC instead queues one nested allowed command, returns a Job UUID immediately, and publishes the eventual result as a FreeSWITCH `BACKGROUND_JOB` event.

## Quick build

Clone the repository (master branch) and build from source:

```bash
# clone the master branch
git clone --branch master https://github.com/mr-krishnak/mod_grpc_api.git
cd mod_grpc_api

# make the build script executable and run a local build (no install)
chmod +x scripts/build-install.sh
NO_INSTALL=1 ./scripts/build-install.sh

# install the module and configuration
sudo cmake --install build
```

Install the C++ build dependencies on Debian or Ubuntu if needed:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake pkg-config \
  libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
  libgrpc++-dev
```

FreeSWITCH development headers are also required. With a typical source installation, the expected header is:

```text
/usr/local/freeswitch/include/freeswitch/switch.h
```

For nonstandard FreeSWITCH paths, pass explicit CMake options:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DFREESWITCH_INCLUDE_DIR=/usr/include/freeswitch \
  -DFREESWITCH_MODULE_DIR=/usr/lib/freeswitch/mod \
  -DFREESWITCH_CONFIG_DIR=/etc/freeswitch/autoload_configs
cmake --build build --parallel
sudo cmake --install build
```

Detailed instructions and troubleshooting are in [docs/INSTALLATION.md](docs/INSTALLATION.md).

## Load the module

From `fs_cli`:

```text
reloadxml
load mod_grpc_api
grpc_api status
```

To load it automatically at FreeSWITCH startup, add this entry to `autoload_configs/modules.conf.xml`:

```xml
<load module="mod_grpc_api"/>
```

## First request with grpcurl

Reflection is disabled by default, so point `grpcurl` to the included proto:

```bash
grpcurl -plaintext \
  -import-path . \
  -proto freeswitch_api.proto \
  -d '{"command":"status","arguments":"","responseMode":"RESPONSE_MODE_RAW_ONLY"}' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

A structured `show channels` request:

```bash
grpcurl -plaintext \
  -import-path . \
  -proto freeswitch_api.proto \
  -d '{"command":"show","arguments":"channels as json","responseMode":"RESPONSE_MODE_STRUCTURED_ONLY"}' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

Queue an originate-and-bridge operation in the background:

```bash
grpcurl -plaintext \
  -import-path . \
  -proto freeswitch_api.proto \
  -d '{
        "command":"bgapi",
        "arguments":"originate user/1001 &bridge(user/1002)",
        "responseMode":"RESPONSE_MODE_RAW_ONLY"
      }' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

The response returns `+OK Job-UUID: ...`; command completion is delivered through a `BACKGROUND_JOB` event. To join two existing channels, call `uuid_bridge` with the two UUIDs. `bridge` by itself is a dialplan application, not a standalone FreeSWITCH API command.

More examples are in [docs/USAGE.md](docs/USAGE.md), [examples/node/client.js](examples/node/client.js), [examples/python/client.py](examples/python/client.py), and [examples/grpcurl.sh](examples/grpcurl.sh).

## Configuration

The installed configuration file is `autoload_configs/grpc_api.conf.xml`. Safe initial defaults are supplied:

| Setting | Default | Purpose |
|---|---:|---|
| `listen-address` | `127.0.0.1:50051` | gRPC listen endpoint |
| `worker-threads` | `8` | FreeSWITCH command workers |
| `max-concurrent-requests` | `64` | Executing plus queued RPC limit |
| `queue-capacity` | `64` | Bounded non-blocking worker queue |
| `acceptor-count` | `8` | Outstanding async accept operations |
| `max-response-bytes` | `8388608` | Maximum retained command output |
| `shutdown-grace-ms` | `5000` | gRPC shutdown grace period |
| `enable-reflection` | `false` | Enable server reflection when linked |
| `log-arguments` | `false` | Log sanitized command arguments |
| `allowed-commands` | call-control list | Exact API commands plus the controlled `uuid_*` family rule |

`max-concurrent-requests`, `shutdown-grace-ms`, `log-arguments`, and `allowed-commands` can be reloaded without unloading the module:

```text
grpc_api reload
```

Other settings require an unload/load cycle. See [docs/CONFIGURATION.md](docs/CONFIGURATION.md).

## Response modes

| Mode | Raw `message` | Structured field | Intended use |
|---|---|---|---|
| `RESPONSE_MODE_COMPAT` | Yes | Yes | Existing clients and debugging |
| `RESPONSE_MODE_RAW_ONLY` | Yes | No | Lowest parsing/copying overhead |
| `RESPONSE_MODE_STRUCTURED_ONLY` | No | Yes | Applications consuming typed data |

The field numbers used by the original proto are preserved. Existing clients that do not know `response_mode` or `truncated` can continue using compatibility behavior after rebuilding the server.

## Runtime control

Inside `fs_cli`:

```text
grpc_api help
grpc_api status
grpc_api reload
```

The status output includes worker count, queue usage, active requests, active background jobs, concurrency limit, response limit, reflection state, shutdown state, and active allowlist.

## Stress test

Install the Node dependencies once:

```bash
npm install
```

Run a normal throughput test below the configured concurrency limit:

```bash
TARGET=127.0.0.1:50051 \
CONCURRENCY=32 \
TOTAL_REQUESTS=10000 \
COMMAND=version \
npm run stress
```

Run an overload test by setting `CONCURRENCY` above `max-concurrent-requests`. The script reports successful commands, command failures, resource exhaustion, unavailable responses, transport failures, attempted throughput, successful throughput, and p50/p95/p99 latency.

## Project layout

```text
.
├── CMakeLists.txt
├── mod_grpc_api.cpp
├── freeswitch_api.proto
├── conf/autoload_configs/grpc_api.conf.xml
├── docs/
├── examples/grpcurl/
├── examples/node/
├── examples/python/
├── scripts/
├── package.json
├── CHANGELOG.md
└── LICENSE
```

## Additional documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Installation](docs/INSTALLATION.md)
- [Configuration](docs/CONFIGURATION.md)
- [Usage](docs/USAGE.md)
- [Security](SECURITY.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Validation notes](docs/VALIDATION.md)

## License

MIT. See [LICENSE](LICENSE).
