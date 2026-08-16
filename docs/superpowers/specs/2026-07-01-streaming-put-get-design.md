# Streaming `offs put` and `offs get` — Design

## Problem

`offs put` loads the entire file into a single CBOR message and sends it as one
`CLIENT_API_PUT_REQUEST` frame. The daemon rejects any single CBOR message
larger than `OFFS_MAX_CBOR_MESSAGE_SIZE = 64 MB`
(`deps/liboffs/src/Util/validation.h:13`) with `Invalid PUT request` before
any data is processed. A 347 MB file (the case that surfaced this) fails
immediately. The single-shot path also forces the full file into RAM, which is
wasteful even for files under the cap.

`offs get` already has a streaming loop in its source
(`src/offs/commands/get.c:60-89`), but it calls
`cli_client_send(client, NULL)` to "read the next frame." `cli_client_send`
short-circuits on `request == NULL`
(`src/offs/client.c:138-140`), so the loop body never executes and `offs get`
only reads the `GET_RESPONSE_START` frame, silently bailing without fetching
the actual file content. The command has been broken since it was written.

## Precedent

The streaming PUT protocol is already implemented and used end-to-end:

- **Wire format** (`deps/liboffs/src/ClientAPI/client_api_wire.h:55-77`):
  `PUT_REQUEST` with `data = NULL` and `stream_length = total_size` initiates
  the stream; `PUT_DATA` frames carry chunks; `PUT_END` finalizes.
- **Server-side** (`deps/liboffs/src/ClientAPI/Unix/unix_connection.c:431-565`):
  `_unix_handle_put` accepts `data == NULL` as a streaming PUT and stores the
  open stream on the connection; `_unix_handle_put_data` appends chunks via
  `writeable_off_stream_write`; `_unix_handle_put_end` calls
  `writeable_off_stream_finalize`, which triggers the close-event subscriber
  that sends `PUT_RESPONSE` with the ORI string back to the client.
- **Reference client** (`deps/liboffs/src/ClientLibs/c/offs_client.c:1602-1670`):
  `offs_client_put_stream_start_ex`, `offs_client_put_stream_data`,
  `offs_client_put_stream_end` already expose the streaming API to library
  consumers — but the OFFS CLI does not use them.
- **Reference test** (`deps/liboffs/test/test_tcp_transport.cpp:275-310`):
  exercises the full PUT_START → PUT_DATA → PUT_END → PUT_RESPONSE round-trip
  on a raw socket.

The server side needs no changes. This is purely a client-side fix in
`src/offs/`.

## Design

### Architecture

The change is isolated to the OFFS repo's `src/offs/` directory and touches
four files:

- `src/offs/client.h` — declare two new helpers.
- `src/offs/client.c` — implement the helpers; refactor `cli_client_send`
  to use them.
- `src/offs/commands/put.c` — replace single-shot buffered PUT with streaming.
- `src/offs/commands/get.c` — replace the dead
  `cli_client_send(client, NULL)` loop with the new read-only helper.

No changes to liboffs. No changes to other CLI commands (block, peer, config,
friend, start/stop/restart, status, health, version).

### New client helpers

Two functions added to `src/offs/client.c`, exposing the two halves of the
existing `cli_client_send` separately:

```c
/* Write-only: serialize frame, 4-byte length-prefix, send. Does NOT read
 * a response. Returns 0 on success, -1 on error. */
int cli_client_send_frame(cli_client_t* client, cbor_item_t* frame);

/* Read-only: read 4-byte length, read CBOR payload, parse. Returns the
 * cbor_item_t* (caller must cbor_decref), or NULL on error / EOF. */
cbor_item_t* cli_client_recv_frame(cli_client_t* client);
```

`cli_client_send` is refactored to:

```c
cbor_item_t* cli_client_send(cli_client_t* client, cbor_item_t* request) {
  if (client == NULL || !client->connected || request == NULL || client->socket == NULL) {
    return NULL;
  }
  if (cli_client_send_frame(client, request) != 0) {
    return NULL;
  }
  return cli_client_recv_frame(client);
}
```

Existing callers (config, peer, status, health, friend, block, etc.) are
unchanged — they still use `cli_client_send` for synchronous request/response.

### `offs put` — streaming upload

`CHUNK_SIZE = 63 * 1024 * 1024` (63 MiB — just under the 64 MiB server cap
with headroom for CBOR bytestring framing overhead, which is a few bytes per
chunk).

`cmd_put` flow:

```
1. Parse args (--temporary, --recycler as today; file_path = argv[0]).
2. fopen(file_path, "rb"); fseek end; file_size = ftell; fseek set.
3. send_frame(PUT_REQUEST) with:
     content_type   = "application/octet-stream"
     file_name      = file_path
     stream_length  = file_size
     data           = NULL, data_size = 0
4. bytes_sent = 0
   while ((n = fread(buf, 1, CHUNK_SIZE, file)) > 0):
     send_frame(PUT_DATA) with data = buf, data_size = n
     bytes_sent += n
     if isatty(STDERR_FILENO): print progress to stderr
5. send_frame(PUT_END)
6. response = recv_frame()
   if response == NULL: error "daemon closed connection", exit 1
   if type == PUT_RESPONSE: print ori_string to stdout, exit 0
   if type == ERROR: print "Error: <message>" to stderr, exit 1
7. fclose(file).
```

The `--temporary` and `--recycler` flags remain on the `PUT_REQUEST` frame
exactly as today (they're request-level options, not per-chunk). The single
heap buffer (`buf`) is allocated once at `CHUNK_SIZE` and reused across all
chunks — peak client memory is `CHUNK_SIZE` regardless of file size, replacing
the previous `file_size` peak.

### `offs get` — streaming download

The existing structure in `src/offs/commands/get.c:13-89` is correct; only the
loop callsite and the initial send need to change:

```c
// OLD:
cbor_item_t* request = client_api_get_request_encode(&get_req);
cbor_item_t* response = cli_client_send(client, request);  // waits for response
cbor_decref(&request);
// ... decode GET_RESPONSE_START ...
while ((response = cli_client_send(client, NULL)) != NULL) {  // BROKEN — returns NULL
  ...
}

// NEW:
cbor_item_t* request = client_api_get_request_encode(&get_req);
if (cli_client_send_frame(client, request) != 0) {  // write-only
  cbor_decref(&request);
  fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
  return 1;
}
cbor_decref(&request);
cbor_item_t* response = cli_client_recv_frame(client);  // read GET_RESPONSE_START
// ... decode GET_RESPONSE_START (unchanged) ...
while ((response = cli_client_recv_frame(client)) != NULL) {  // loop read frames
  ... GET_DATA -> fwrite, GET_END -> break, ERROR -> print+break ...
}
```

The GET_RESPONSE_START decode logic (lines 49-66) is preserved unchanged.
The `output_path` / `--output` handling (lines 30-37, 64) is preserved.

### Error handling

- **File errors** (fopen, fseek, fread): `perror`, exit 1. No server state
  touched yet.
- **`cli_client_send_frame` returns -1** (mid-stream socket error): print
  `Failed to send frame: <strerror(errno)>` to stderr, exit 1. Server-side
  cleanup of the in-progress stream happens automatically on socket disconnect
  (`unix_connection.c:1104` resets `connection->put_streaming = 0` on
  destroy).
- **`cli_client_recv_frame` returns NULL** (server closed connection without
  sending PUT_RESPONSE): print `Daemon closed connection` to stderr, exit 1.
- **Server-sent ERROR frame** at any point: print `Error: <message>` to
  stderr, exit 1. The server may send ERROR instead of PUT_RESPONSE if the
  upload fails server-side.
- **Out-of-scope**: partial-upload cleanup (deleting orphaned blocks on the
  server) — server already handles this on connection drop; resumable uploads
  — the streaming protocol is non-resumable by design.

### Progress reporting

Enabled only when stderr is a TTY (`isatty(STDERR_FILENO)`). Format:

```
\rPutting <file_path>: <bytes_sent>/<file_size> bytes (<pct>%)
```

Final newline after the last chunk. Disabled for redirected stderr (so
`offs put file > log.txt` doesn't litter progress bytes into the log).
No flag to force-enable — keeps the CLI surface area unchanged.

### Testing

Build both Debug (`build/`) and Release (`build-release/`):

```
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
cmake --build build --target offsd offs_cli -j$(nproc)
cmake --build build-release --target offsd offs_cli -j$(nproc)
```

Start daemon:

```
./build-release/offsd --foreground --unix /tmp/offs.sock \
  --cache-dir /tmp/offs-cache --data-dir /tmp/offs-data
```

Test matrix (from another shell):

| Test | Command | Expected |
|------|---------|----------|
| Tiny file (1 chunk) | `offs --socket /tmp/offs.sock put /etc/hostname` | ORI printed, exit 0 |
| Medium file (1 chunk, under cap) | `offs put ./README.md` | ORI printed, exit 0 |
| Large file (multiple chunks) | `offs put <100MB file>` | Progress shown, ORI printed, exit 0 |
| The original failing case | `offs put /home/victor/Videos/Supacell...mkv` (347 MB) | Progress shown, ORI printed, exit 0 |
| Round-trip small | `offs get <ori_from_README> --output /tmp/out` then `diff` | Files identical |
| Round-trip large | `offs get <ori_from_Supacell> --output /tmp/out.mkv` then `sha256sum` | Hashes match |
| Nonexistent file | `offs put /nonexistent` | `perror` message, exit 1 |
| Server-down mid-stream | stop `offsd` during large `put` | `Daemon closed connection` or `Failed to send frame`, exit 1 |
| Non-streaming regression | `offs config show` | Still works (proves `cli_client_send` refactor didn't break) |
| Non-streaming regression | `offs peer info` | Still works |
| Non-streaming regression | `offs status` | Still works |
| Non-streaming regression | `offs health` | Still works |

Memory check: peak RSS of `offs put` on the 347 MB file should be roughly
`CHUNK_SIZE` (63 MiB) plus the runtime, not 347 MiB. Verify with
`/usr/bin/time -v offs put <big-file>` and inspect "Maximum resident set
size".

## Out of scope

- No changes to liboffs (server-side streaming is correct).
- No changes to other CLI commands.
- No CLI flag for chunk size (hardcoded 63 MiB; can add later if needed).
- No resumable uploads (streaming protocol is non-resumable).
- No partial-upload cleanup on the server side (handled by existing
  connection-destroy logic).
- No fix to `offs get`'s `--range` support beyond what's already there — only
  the streaming loop callsite is being fixed.
- No changes to the existing `offs put` flag surface (`--temporary`,
  `--recycler`, `--help`) — semantics preserved.