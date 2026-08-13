#!/usr/bin/env python3
"""Small mod_grpc_api client.

Generate Python stubs first, as described in docs/USAGE.md, then put the
generated directory on PYTHONPATH.
"""

from __future__ import annotations

import argparse
import json
import sys
from typing import Sequence

try:
    import grpc
    from google.protobuf.json_format import MessageToDict
    import freeswitch_api_pb2
    import freeswitch_api_pb2_grpc
except ImportError as exc:  # pragma: no cover - user-facing setup path
    print(
        "Missing gRPC modules or generated stubs. Follow the Python example "
        "in docs/USAGE.md before running this client.\n"
        f"Import error: {exc}",
        file=sys.stderr,
    )
    raise SystemExit(2) from exc


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Execute a permitted FreeSWITCH API command over gRPC."
    )
    parser.add_argument("command", help="FreeSWITCH API command name")
    parser.add_argument(
        "arguments",
        nargs="*",
        help="Command arguments; multiple shell words are joined with spaces",
    )
    parser.add_argument(
        "--address",
        default="127.0.0.1:50051",
        help="gRPC endpoint (default: %(default)s)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="RPC deadline in seconds (default: %(default)s)",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--raw",
        action="store_true",
        help="Return only the raw FreeSWITCH output",
    )
    mode.add_argument(
        "--structured",
        action="store_true",
        help="Return only the structured protobuf representation",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    if args.timeout <= 0:
        print("--timeout must be greater than zero", file=sys.stderr)
        return 2

    if args.raw:
        response_mode = freeswitch_api_pb2.RESPONSE_MODE_RAW_ONLY
    elif args.structured:
        response_mode = freeswitch_api_pb2.RESPONSE_MODE_STRUCTURED_ONLY
    else:
        response_mode = freeswitch_api_pb2.RESPONSE_MODE_COMPAT

    request = freeswitch_api_pb2.ApiRequest(
        command=args.command,
        arguments=" ".join(args.arguments),
        response_mode=response_mode,
    )

    try:
        with grpc.insecure_channel(args.address) as channel:
            stub = freeswitch_api_pb2_grpc.FreeSwitchApiStub(channel)
            response = stub.Execute(request, timeout=args.timeout)
    except grpc.RpcError as exc:
        code = exc.code().name if exc.code() else "UNKNOWN"
        print(f"RPC failed: {code}: {exc.details() or ''}", file=sys.stderr)
        return 1

    output = MessageToDict(response, preserving_proto_field_name=True)
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0 if response.success else 3


if __name__ == "__main__":
    raise SystemExit(main())
