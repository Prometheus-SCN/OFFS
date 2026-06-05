# OFFS Project Design

## Overview

OFFS (Owner Free File System) is a node and client application built on top of liboffs. It consists of two binaries — `offsd` (daemon) and `offs` (CLI admin tool) — modeled after the Docker architecture. The project consumes liboffs as a git submodule and builds with CMake using the same C11/gtest patterns as the library.

## Repository Structure

```
OFFS/
├── CMakeLists.txt          # Top-level build
├── deps/
│   ├── liboffs/            # git submodule → https://github.com/Prometheus-SCN/liboffs.git
│   └── poll-dancer/        # git submodule
├── src/
│   ├── offsd/
│   │   └── main.c          # Daemon entry point
│   └── offs/
│       ├── main.c          # CLI entry point, command dispatch
│       ├── client.h        # Unix socket client using client_api_wire
│       ├── client.c
│       ├── commands/
│       │   ├── put.c       # Import file
│       │   ├── get.c       # Export file
│       │   ├── block.c     # Block cache operations
│       │   ├── peer.c      # Peer management
│       │   ├── config.c    # Config management (get/set/add/remove/generate-auth/reload)
│       │   ├── friend.c    # Friend list management
│       │   ├── health.c    # Health/status
│       │   ├── start_stop.c # Daemon lifecycle (start/stop/restart)
│       │   └── status.c    # Daemon status
│       ├── l10n/
│       │   ├── en.h        # English strings
│       │   └── es.h        # Spanish strings (example)
│       └── help.c          # Multi-lingual help system
├── test/
│   ├── CMakeLists.txt
│   ├── test_main.cpp
│   ├── test_offs_parse.cpp
│   ├── test_offs_wire.cpp
│   ├── test_offs_l10n.cpp
│   ├── test_offsd_lifecycle.cpp
│   ├── test_offsd_put_get.cpp
│   ├── test_offsd_block.cpp
│   ├── test_offsd_config.cpp
│   ├── test_offsd_peer.cpp
│   └── test_offsd_e2e.cpp
└── docs/
    └── superpowers/
        └── specs/
```

## CMake Architecture

liboffs is consumed via `add_subdirectory(deps/liboffs)`. The liboffs CMakeLists.txt is modified with a guard so `project()` only runs when built standalone:

```cmake
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  project(offs C CXX)
endif()
```

`POLL_DANCER_ROOT` defaults to `deps/poll-dancer` when building as a subdirectory. liboffs gains `install()` rules for the static library, public headers, and CMake export targets so it can optionally be installed as a system package.

OFFS top-level CMakeLists.txt:
```cmake
project(OFFS C CXX)
add_subdirectory(deps/liboffs)
add_executable(offsd src/offsd/main.c)
target_link_libraries(offsd PRIVATE offs)
add_executable(offs src/offs/main.c src/offs/client.c ...)
target_link_libraries(offs PRIVATE offs)
add_subdirectory(test)
```

Submodules are initialized recursively: `git submodule update --init --recursive` on OFFS pulls liboffs, which pulls its own submodules (BLAKE3, xxHash, hashmap, libcbor, http-parser, msquic, googletest).

## offsd Daemon

Productionized version of the existing `examples/off_server/main.c`.

### Startup

```
offsd [--config /etc/offs/offsd.conf] [--foreground]
```

1. Parse config file (TOML or JSON, using existing `config_t`)
2. Daemonize unless `--foreground`: double-fork, detach terminal, write PID file
3. Auto-detect worker count from CPU cores (`sysconf(_SC_NPROCESSORS_ONLN)` / `GetActiveProcessorCount`)
4. Wire up component stack: scheduler_pool → timer_actor → block_cache → ofd_cache → tuple_cache → http_server → unix_transport → authority → network
5. Register route handlers (off, block, health, peer, config)
6. Listen on HTTP and Unix socket
7. Load peers, start network connections
8. Enter event loop with signal handlers for SIGINT/SIGTERM

### Shutdown

Graceful drain on SIGINT/SIGTERM: stop accepting connections, finish in-flight requests, save peers, teardown in reverse order.

### Config file (TOML)

```toml
[daemon]
pid-file = "/var/run/offsd.pid"
data-dir = "/var/lib/offs"

[network]
host = "0.0.0.0"
port = 23402

[unix]
socket-path = "/var/run/offs.sock"

[cache]
dir = "/var/lib/offs/cache"

[workers]
count = 0  # 0 = auto-detect
```

## Cross-Platform Paths

| Purpose | Linux | macOS | Windows |
|---------|-------|-------|---------|
| Data dir | `/var/lib/offs/` | `/Library/Application Support/offs/` | `C:\ProgramData\offs\` |
| Config | `/etc/offs/offsd.conf` | `/Library/Preferences/offs/offsd.conf` | `C:\ProgramData\offs\offsd.conf` |
| PID file | `/var/run/offsd.pid` | `/var/run/offsd.pid` | N/A (Windows services) |
| Socket | `/var/run/offs.sock` | `/var/run/offs.sock` | `\\.\pipe\offs` |

Worker count auto-detection:
- Linux/macOS: `sysconf(_SC_NPROCESSORS_ONLN)`
- Windows: `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)`

## offs CLI

Communicates with `offsd` over Unix socket (named pipe on Windows) using the existing CBOR wire protocol from `client_api_wire.h`.

### Commands

```
offs start [--config <path>] [--foreground]         Start the daemon
offs stop                                            Stop the running daemon
offs restart [--config <path>]                       Stop + start
offs put <file> [--temporary] [--recycler <url>]     Import a file
offs get <ori> [--output <path>]                     Export a file
offs block put <data> [--encoding base58|raw]        Store a raw block
offs block get <hash>                                Retrieve a raw block
offs block delete <hash>                             Delete a block
offs peer info                                       Show this node's peer info
offs peer list                                       List connected peers
offs peer connect <addr>                             Connect to a peer
offs friend add <data>                               Add a trusted friend
offs friend remove <node-id>                         Remove a friend
offs friend list                                     List friends
offs config show                                     Show all config
offs config get <key>                                Get a single setting
offs config set <key> <value>                        Set a scalar value
offs config add <key> <value>                        Append to a list value
offs config remove <key> <value>                     Remove from a list value
offs config generate-auth                            Generate a new auth token
offs config set-auth <token>                         Explicitly set the auth token
offs config reload                                   Signal daemon to reload config
offs health                                          Health check + stats
offs status                                          Daemon status (running, uptime)
offs version                                         Print version
offs help [command]                                  Help (localized)
```

### Protocol flow per command

1. Open Unix socket to daemon
2. Optionally send `CLIENT_API_AUTH_REQUEST` if auth configured
3. Encode request via `client_api_*_encode()` → serialize CBOR to socket
4. Read CBOR response, decode, display formatted result
5. Close socket

### Localization (l10n)

Both help text and error messages are localized. Strings are extern'd in `src/offs/l10n/` — one header per locale (`en.h`, `es.h`, etc.). Each header defines the same set of string keys with translated values.

Language selection (highest to lowest priority):

1. `--lang <code>` flag on any command (e.g., `offs --lang es config show`)
2. `$OFFS_LANG` environment variable
3. `$LANG` / `$LC_ALL` on POSIX, `GetUserDefaultLocaleName()` on Windows
4. Fallback to `en`

The language can also be persisted via `offs config set lang es`, which writes the preference for future commands. The `--lang` flag always overrides the persisted value.

### Command dispatch

Each command is a separate `.c` file under `src/offs/commands/`. A dispatch table in `main.c` maps command name → handler function.

## liboffs Changes

1. Guard `project()` in CMakeLists.txt for subdirectory use
2. Default `POLL_DANCER_ROOT` to `deps/poll-dancer` as subdirectory
3. Add `install()` rules for library, headers, and CMake export targets
4. New wire protocol messages in `client_api_wire.h`/`.c`:
   - `CLIENT_API_CONFIG_GET_REQUEST` / `RESPONSE`
   - `CLIENT_API_CONFIG_SET_REQUEST` / `RESPONSE` (operation: set/add/remove)
   - `CLIENT_API_CONFIG_RELOAD_REQUEST` / `RESPONSE`
   - `CLIENT_API_CONFIG_GENERATE_AUTH_REQUEST` / `RESPONSE`
   - `CLIENT_API_SHUTDOWN_REQUEST` — triggers graceful daemon shutdown
5. Config route handler updated with granular get/set/add/remove alongside existing reload

## Error Handling

- CLI exits non-zero with error to stderr when daemon unreachable or responds with error
- Daemon logs through existing `ERROR()` macro and `stream_notify(error_event, ...)`
- Wire errors use `CLIENT_API_ERROR` with existing status codes

## Testing

Same framework as liboffs: GoogleTest, C++17, `extern "C"` wrappers.

- **Unit tests**: CLI argument parsing, wire protocol encode/decode round-trips, locale detection
- **Integration tests**: Start `offsd --foreground` with temp config, run CLI commands against it, verify results
- **E2E tests** (optional): Two daemons peering and transferring files

Tests run under Valgrind to catch memory leaks. The de-wonk skill is applied before marking tasks complete.

## Build and Install

```bash
cmake -B build && cmake --build build
cmake --install build  # → bin/offs, bin/offsd
```

Config skeleton installed to platform-appropriate `/etc/` equivalent. systemd unit file and launchd plist as optional install components.
