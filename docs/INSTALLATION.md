# Installation

## 1. Requirements

The module requires:

- a C++17 compiler;
- CMake 3.16 or newer;
- pkg-config;
- Protobuf headers, library, and `protoc`;
- gRPC C++ headers and libraries;
- `grpc_cpp_plugin`;
- FreeSWITCH headers, including `switch.h` and `switch_json.h`;
- a running FreeSWITCH installation into which the module can be installed.

### Debian and Ubuntu build packages

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake pkg-config \
  libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
  libgrpc++-dev
```

The FreeSWITCH development package name and availability depend on the FreeSWITCH repository used by the host. When FreeSWITCH is built from source, install its headers or point this project at the source/build include directory that contains `switch.h`.

Confirm the required tools:

```bash
cmake --version
pkg-config --modversion grpc++
protoc --version
command -v grpc_cpp_plugin
test -f /usr/local/freeswitch/include/freeswitch/switch.h
```

## 2. Extract the archive

```bash
unzip mod_grpc_api-1.1.1-async-call-control.zip
cd mod_grpc_api-1.1.1-async-call-control
```

## 3. Configure and build

### Standard `/usr/local/freeswitch` installation

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The output module is normally:

```text
build/mod_grpc_api.so
```

The helper script performs the same steps:

```bash
chmod +x scripts/build-install.sh
NO_INSTALL=1 ./scripts/build-install.sh
```

### Custom FreeSWITCH installation

First identify the runtime module and configuration directories:

```bash
fs_cli -x "global_getvar mod_dir"
fs_cli -x "global_getvar conf_dir"
```

Then configure explicit paths. Example for a packaged installation:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DFREESWITCH_INCLUDE_DIR=/usr/include/freeswitch \
  -DFREESWITCH_MODULE_DIR=/usr/lib/freeswitch/mod \
  -DFREESWITCH_CONFIG_DIR=/etc/freeswitch/autoload_configs
cmake --build build --parallel
```

The module directory may instead be architecture-specific, such as `/usr/lib/x86_64-linux-gnu/freeswitch/mod`. Use the value reported by FreeSWITCH rather than assuming a path.

Available CMake path options:

| Option | Default |
|---|---|
| `FREESWITCH_ROOT` | `/usr/local/freeswitch` |
| `FREESWITCH_INCLUDE_DIR` | `${FREESWITCH_ROOT}/include/freeswitch` |
| `FREESWITCH_MODULE_DIR` | `${FREESWITCH_ROOT}/mod` |
| `FREESWITCH_CONFIG_DIR` | `${FREESWITCH_ROOT}/conf/autoload_configs` |

## 4. Install

Back up an existing configuration before installing an upgrade:

```bash
sudo cp -a \
  /usr/local/freeswitch/conf/autoload_configs/grpc_api.conf.xml \
  /usr/local/freeswitch/conf/autoload_configs/grpc_api.conf.xml.bak \
  2>/dev/null || true
```

Install with CMake:

```bash
sudo cmake --install build
```

This installs:

```text
/usr/local/freeswitch/mod/mod_grpc_api.so
/usr/local/freeswitch/conf/autoload_configs/grpc_api.conf.xml
```

With custom CMake directory options, those files are installed to the custom paths instead.

The helper can build and install in one command when run with sufficient permissions:

```bash
sudo -E ./scripts/build-install.sh
```

Running the build as a normal user and only the install step as root is usually cleaner:

```bash
NO_INSTALL=1 ./scripts/build-install.sh
sudo cmake --install build
```

## 5. Configure module loading

Edit FreeSWITCH `autoload_configs/modules.conf.xml` and add:

```xml
<load module="mod_grpc_api"/>
```

A minimal surrounding example is:

```xml
<configuration name="modules.conf" description="Modules">
  <modules>
    <load module="mod_grpc_api"/>
  </modules>
</configuration>
```

Do not replace an existing `modules.conf.xml` with this minimal fragment. Add the load line to the existing `<modules>` element.

## 6. Load and verify

From `fs_cli`:

```text
reloadxml
load mod_grpc_api
grpc_api status
```

Expected status begins with `+OK` and reports a loopback listener, worker count, queue usage, and allowlist.

Check that the port is listening:

```bash
ss -ltnp | grep 50051
```

Test the endpoint from the project directory:

```bash
grpcurl -plaintext \
  -import-path . \
  -proto freeswitch_api.proto \
  -d '{"command":"version","responseMode":"RESPONSE_MODE_RAW_ONLY"}' \
  127.0.0.1:50051 fsgrpc.FreeSwitchApi/Execute
```

## 7. Upgrade

```text
unload mod_grpc_api
```

Then rebuild/install the new version, review differences in `grpc_api.conf.xml`, and load it again:

```text
reloadxml
load mod_grpc_api
grpc_api status
```

The updated proto is wire-compatible with the original field numbers, but client stubs should be regenerated to use `response_mode` and `truncated`.

## 8. Uninstall

Unload the module first:

```text
unload mod_grpc_api
```

Remove the installed files using the paths selected during configuration:

```bash
sudo rm -f /usr/local/freeswitch/mod/mod_grpc_api.so
sudo rm -f /usr/local/freeswitch/conf/autoload_configs/grpc_api.conf.xml
```

Also remove `<load module="mod_grpc_api"/>` from `modules.conf.xml`.

## Troubleshooting

### CMake cannot find `switch.h`

Pass the directory containing the header, not the header itself:

```bash
cmake -S . -B build \
  -DFREESWITCH_INCLUDE_DIR=/path/to/include/freeswitch
```

### CMake cannot find Protobuf

Install both the compiler and development library, then remove the incomplete build directory and configure again:

```bash
sudo apt-get install -y libprotobuf-dev protobuf-compiler
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

### `grpc_cpp_plugin` was not found

```bash
sudo apt-get install -y protobuf-compiler-grpc
command -v grpc_cpp_plugin
```

If installed elsewhere:

```bash
cmake -S . -B build \
  -DGRPC_CPP_PLUGIN=/custom/path/grpc_cpp_plugin
```

### gRPC package is not found by pkg-config

```bash
pkg-config --cflags --libs grpc++
```

If this fails, install the gRPC C++ development package or add its `.pc` directory to `PKG_CONFIG_PATH`.

### FreeSWITCH cannot load the module

Inspect dependencies:

```bash
ldd /usr/local/freeswitch/mod/mod_grpc_api.so
```

Look for `not found`, then update the dynamic linker configuration or install the missing runtime library. Also confirm the module was built for the same CPU architecture and compatible FreeSWITCH build.

### Configuration cannot be opened

Confirm the file is in the active FreeSWITCH configuration directory:

```bash
fs_cli -x "global_getvar conf_dir"
```

The expected path is `<conf_dir>/autoload_configs/grpc_api.conf.xml`.

### Reflection was enabled but is unavailable

The module was built without `grpc++_reflection`. Either install the reflection library and rebuild, or keep:

```xml
<param name="enable-reflection" value="false"/>
```

Reflection is not needed when grpcurl is given `-proto freeswitch_api.proto`.
