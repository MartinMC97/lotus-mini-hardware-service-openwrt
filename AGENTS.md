# Repository Guidelines

## Project Structure & Module Organization

`src/server.cpp` implements `lotus-hardwared`; `src/client.cpp` implements `lotus-hwctl`. Headers in `src/` contain protocol, client SDK, hardware backends, logging, and persistence logic. `tests/` holds host tests; `tools/` contains smoke utilities and application-adapter checks. `profiles/` supplies the shared ALSA playback configuration. `openwrt/` contains SDK configuration, package definitions, UCI defaults, and procd startup scripts. Read `docs/API_V1.md` for the interface and `docs/V1_INSTALLATION.md` for deployment. Vendored picojson and its modification notes live in `third_party/`.

## Build, Test, and Development Commands

- `make`: build the daemon and CLI into `build/` using a C++17 compiler and pthreads.
- `make test`: build prerequisites, run C++ assertions, Python socket integration tests, and logging-policy checks. Requires Python 3.
- `make build/lotus-hardwared-alsa`: build the optional ALSA daemon; requires ALSA development headers and library.
- `sh openwrt/build-sdk.sh /path/to/matching-openwrt-sdk`: package against the matching SDK; writes packages to `openwrt/artifacts/` and updates the SDK configuration.

For local development, create a private socket directory with `mkdir -m 700 /tmp/lotus-dev`, then run `build/lotus-hardwared --mock --socket /tmp/lotus-dev/control.sock`. Query it with `build/lotus-hwctl --socket /tmp/lotus-dev/control.sock state.get`.

## Coding Style & Naming Conventions

Follow surrounding code: four-space indentation, same-line C++ braces, `hw` namespace, PascalCase types, and snake_case functions and variables. Use `.h` headers and `.cpp` sources; retain SPDX license headers. Builds enforce `-Wall -Wextra -Wpedantic -Werror`. No dedicated formatter or linter configuration is present. Preserve client-header compatibility documented in the source.

## Testing Guidelines

C++ tests use `assert`; Python integration tests use standard-library `unittest`. Name C++ tests `tests/<feature>_test.cpp` and Python test methods `test_<behavior>`. Add regression cases for malformed requests, lease cleanup, persistence failures, and fail-closed privacy. Use mock backends, temporary files, and explicit timestamps where possible. No numerical coverage threshold is configured; host fixtures do not establish hardware acceptance.

## Commit & Pull Request Guidelines

Recent commits use imperative subjects such as “Add”, “Implement”, and “Reduce”; occasional `feat:` prefixes appear. Keep changes focused. PR descriptions should explain behavior, validation performed, and protocol or deployment effects; link related issues when available. Update relevant documentation when changing public behavior.

## Hardware & Configuration Safety

Real controls require `--controls --handoff-confirmed` after stopping previous hardware owners. Keep development tests on mocks or fixtures. Preserve same-UID private sockets, bounded queues, and fail-closed privacy; never commit keys or private deployment backups.
