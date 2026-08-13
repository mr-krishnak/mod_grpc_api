# Validation performed for this package

The 1.1.1 source package was checked on 2026-08-12 with the following steps:

- Bash syntax checks for the build, smoke-test, and grpcurl helper scripts.
- Node.js syntax checks for the example client and stress test.
- Python AST parsing for the example client.
- XML parsing and validation of the default `allowed-commands` rules.
- JSON parsing for every grpcurl request example.
- Version consistency checks across `VERSION` and `CMakeLists.txt`.
- Protobuf compatibility-marker checks.
- Local Markdown-link and code-fence validation.
- C++17 syntax checks with GCC and Clang using compatibility headers, with
  `-Wall -Wextra -Wpedantic -Werror`.
- C++ syntax checks for both reflection-disabled and reflection-enabled paths.
- Focused executable tests for:
  - `bgapi` nested-command parsing;
  - nested `bgapi` rejection;
  - case normalization;
  - the controlled `uuid_*` family match;
  - exact hard-deny enforcement;
  - default authorization for `bgapi`, `originate`, and `uuid_bridge`;
  - background-job counter release after execution.
- The focused executable tests were run with both GCC and Clang.
- The focused executable tests were also run under AddressSanitizer and
  UndefinedBehaviorSanitizer.

Run the source-level checks after extraction:

```bash
./scripts/verify-source.sh
```

The validation environment did not contain the real FreeSWITCH, Protobuf, and
gRPC development packages or a running FreeSWITCH process. Therefore, a native
link against the target host's exact libraries, a live module load, a real
`BACKGROUND_JOB` subscription, and a production originate/bridge call were not
performed here. Build and live smoke-test commands are provided in
[INSTALLATION.md](INSTALLATION.md) and [USAGE.md](USAGE.md).
