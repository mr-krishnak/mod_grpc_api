#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
FREESWITCH_ROOT="${FREESWITCH_ROOT:-/usr/local/freeswitch}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DFREESWITCH_ROOT="${FREESWITCH_ROOT}" \
  "$@"

cmake --build "${BUILD_DIR}" --parallel "${JOBS}"

if [[ "${NO_INSTALL:-0}" == "1" ]]; then
  echo "Build completed: ${BUILD_DIR}/mod_grpc_api.so"
else
  cmake --install "${BUILD_DIR}"
  echo "Installed mod_grpc_api and grpc_api.conf.xml using the configured CMake destinations"
fi
