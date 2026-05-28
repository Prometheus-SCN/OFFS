# OFFS — Owner Free File System

<p align="center">
  <img src="off-logo-lettered.svg" alt="OFFS Logo" width="400">
</p>

OFFS is a distributed, owner-free file system. Data is split into encrypted blocks,
erasure-coded for redundancy, and spread across a peer-to-peer network so that no
single node holds a complete file or knows what it stores.

This repository provides the node software: a daemon (`offsd`) and a CLI
administration tool (`offs`), modeled after Docker's daemon/client architecture.

## Architecture

```
┌──────────┐   Unix socket    ┌──────────┐
│  offs    │ ◄── CBOR/RPC ──► │  offsd   │
│  (CLI)   │                  │ (daemon) │
└──────────┘                  └────┬─────┘
                                   │
                            ┌──────┴──────┐
                            │   liboffs   │
                            │  (library)  │
                            └──────┬──────┘
                                   │
              ┌────────────────────┼────────────────────┐
              │                    │                    │
         Block Cache         P2P Network          HTTP / Unix
         (local store)     (QUIC + gossip)        (client API)
```

- **`offsd`** — Long-running daemon that manages block storage, peer connections,
  and client requests. Supports daemonization (double-fork), PID files, JSON
  config, and graceful shutdown via SIGINT/SIGTERM.
- **`offs`** — CLI tool that communicates with `offsd` over a Unix socket using
  CBOR-encoded wire protocol messages. Supports put/get/block/peer/config/friend
  operations.
- **`liboffs`** — Core library providing block caching, OFD/tuple streaming,
  scheduler, peer-to-peer networking (QUIC via msquic), and HTTP/Unix transport.

## Prerequisites

- **CMake** 3.22+
- **C11** compiler and **C++17** compiler (Clang or GCC)
- **OpenSSL** 3.x (libssl-dev, libcrypto-dev)
- **GTest** (libgtest-dev, for tests)
- **libqrencode** (optional, for QR code generation)

## Building

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/vijayee/OFFS.git
cd OFFS

# Build poll-dancer dependency (one-time)
cd deps/poll-dancer && mkdir build && cd build && cmake .. && make -j$(nproc)
cd ../../..

# Build OFFS
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Optionally, build with tests
cmake .. -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

The build produces two binaries in `build/`:
- **`offsd`** — The daemon
- **`offs`** — The CLI client (`offs_cli` target, renamed to `offs`)

## Quick Start

```bash
# Start the daemon in the foreground (development)
./offsd --foreground --cache-dir /tmp/offs-cache --data-dir /tmp/offs-data

# In another terminal, use the CLI
./offs health                    # Check daemon health
./offs status                    # Daemon status
./offs put ./README.md           # Import a file into the network
./offs get <ori-string>          # Retrieve a file by its ORI
./offs block put "hello world"   # Store a raw block
./offs peer info                 # Show local peer information
./offs peer list                 # List connected peers
./offs config show               # Display current configuration
./offs friend add <peer-data>    # Add a friend peer
./offs stop                      # Stop the daemon
```

## Daemon Options

| Option | Description | Default |
|--------|-------------|---------|
| `--config <path>` | JSON config file path | — |
| `--host <addr>` | HTTP bind address | `0.0.0.0` |
| `--port <port>` | HTTP port (0 = disable) | `23402` |
| `--unix <path>` | Unix socket path | — |
| `--cache-dir <dir>` | Block cache directory | `./offs_cache` |
| `--data-dir <dir>` | Persistent data directory | `.` |
| `--pid-file <path>` | PID file path | — |
| `--workers <n>` | Worker thread count (0 = auto) | `0` |
| `--foreground` | Run in foreground | off |

## CLI Commands

| Command | Description |
|---------|-------------|
| `start` | Start the daemon |
| `stop` | Stop the running daemon |
| `restart` | Stop and restart the daemon |
| `put <file>` | Import a file into the network |
| `get <ori>` | Export a file by ORI |
| `block put\|get\|delete` | Block cache operations |
| `peer info\|list\|connect` | Peer management |
| `config show\|get\|set\|add\|remove\|...` | Configuration management |
| `friend add\|remove\|list` | Friend list management |
| `health` | Health check and statistics |
| `status` | Daemon status |
| `version` | Print version |
| `help` | Show help |

Use `--lang <code>` after `offs` to switch the UI language (e.g., `offs --lang fr health`).

## Running Tests

```bash
# Configure with testing enabled
cmake .. -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

# Run OFFS tests
./test/testoffs

# Run with Valgrind (requires DWARF-4 on Valgrind < 3.19)
cmake .. -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-gdwarf-4" -DCMAKE_CXX_FLAGS="-gdwarf-4"
make -j$(nproc) testoffs
valgrind --leak-check=full ./test/testoffs --gtest_filter='-OffsdIntegrationTest.*'
```

## Project Structure

```
OFFS/
├── src/
│   ├── offsd/main.c             # Daemon entry point
│   └── offs/
│       ├── main.c               # CLI entry point
│       ├── client.{h,c}         # Unix socket CBOR client
│       ├── cli_util.{h,c}       # Command table, language detection, help
│       ├── l10n/en.h            # English localization strings
│       └── commands/
│           ├── start_stop.c     # start, stop, restart
│           ├── put.c            # put
│           ├── get.c            # get
│           ├── block.c          # block put/get/delete
│           ├── peer.c           # peer info/list/connect
│           ├── config.c         # config show/get/set/add/remove/...
│           ├── friend.c         # friend add/remove/list
│           ├── health.c         # health
│           └── status.c         # status, version
├── test/                        # Test suite
│   ├── test_main.cpp
│   ├── test_cli.cpp             # L10N, command table, language detection
│   ├── test_client.cpp          # Client socket transport
│   ├── test_offsd_integration.cpp  # Daemon lifecycle integration
│   └── test_cli_stubs.c         # Linker stubs for command handlers
├── deps/
│   ├── liboffs/                 # Core library (submodule)
│   └── poll-dancer/             # Event loop (submodule)
└── off-logo-lettered.svg
```

## Dependencies

| Dependency | Purpose |
|------------|---------|
| [liboffs](https://github.com/Prometheus-SCN/liboffs) | Core OFF System library |
| [poll-dancer](https://github.com/vijayee/poll-dancer) | Cross-platform async I/O |
| libcbor | CBOR serialization for wire protocol |
| cJSON | JSON parsing for config and health output |
| OpenSSL / QuicTLS | Cryptography and QUIC transport |
| msquic | QUIC protocol implementation |
| BLAKE3 | Content hashing |
| hashmap | Hash map data structure |
| Google Test | C++ test framework |
