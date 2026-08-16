# Streaming `offs put` and `offs get` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the single-shot `offs put` (loads entire file into one CBOR message, fails for files >64 MB) with streaming PUT_START + PUT_DATA + PUT_END, and fix `offs get`'s broken streaming loop (currently reads only GET_RESPONSE_START because `cli_client_send(client, NULL)` short-circuits).

**Architecture:** Add two low-level frame helpers to `src/offs/client.c` — `cli_client_send_frame` (write-only) and `cli_client_recv_frame` (read-only) — by splitting the existing `cli_client_send` in half. Refactor `cli_client_send` to call both. Rewrite `cmd_put` to stream the file in 63 MiB chunks. Replace the dead `cli_client_send(client, NULL)` loop in `cmd_get` with `cli_client_recv_frame(client)`. No liboffs changes — the server-side streaming protocol is already correct.

**Tech Stack:** C11, CMake, libcbor, liboffs (read-only here), platform_socket, isatty/STDERR_FILENO for TTY detection.

---

## File Structure

- **Modify** `src/offs/client.h` — declare `cli_client_send_frame` and `cli_client_recv_frame`.
- **Modify** `src/offs/client.c` — implement the two helpers; refactor `cli_client_send` to use them. Add `<unistd.h>` include for `isatty` (not needed in client.c itself, but documented here for the consumer).
- **Modify** `src/offs/commands/put.c` — rewrite `cmd_put` to stream. Add `<unistd.h>` include for `isatty(STDERR_FILENO)`.
- **Modify** `src/offs/commands/get.c` — replace `cli_client_send(client, NULL)` with `cli_client_recv_frame(client)` and change the initial send to `cli_client_send_frame`.

No new files. No liboffs changes.

---

## Task 1: Add frame helpers and refactor `cli_client_send`

**Files:**
- Modify: `src/offs/client.h`
- Modify: `src/offs/client.c:136-202` (split `cli_client_send` into two helpers + refactor)

- [ ] **Step 1: Declare the two new helpers in `client.h`**

Open `src/offs/client.h`. After the existing declaration of `cli_client_send` (currently the last declaration before `#endif`), add:

```c
/* Send a single CBOR frame without reading a response. Used by streaming
   commands (put, get) that send multiple frames before reading the final
   response. Returns 0 on success, -1 on error. The caller still owns
   *frame and must cbor_decref it. */
int cli_client_send_frame(cli_client_t* client, cbor_item_t* frame);

/* Read a single CBOR frame from the daemon (no request sent first). Used
   by streaming commands to read the server's response frames in a loop.
   Returns the parsed CBOR item (caller must cbor_decref), or NULL on error
   or connection close. */
cbor_item_t* cli_client_recv_frame(cli_client_t* client);
```

- [ ] **Step 2: Implement `cli_client_send_frame` in `client.c`**

In `src/offs/client.c`, immediately BEFORE the existing `cli_client_send` function (line 136), insert:

```c
int cli_client_send_frame(cli_client_t* client, cbor_item_t* frame) {
  if (client == NULL || !client->connected || frame == NULL || client->socket == NULL) {
    return -1;
  }

  unsigned char* cbor_buf = NULL;
  size_t cbor_len = 0;
  cbor_len = cbor_serialize_alloc(frame, &cbor_buf, &cbor_len);
  if (cbor_buf == NULL || cbor_len == 0) {
    free(cbor_buf);
    return -1;
  }

  size_t framed_len = 0;
  uint8_t* framed = stream_frame_encode(cbor_buf, cbor_len, &framed_len);
  free(cbor_buf);

  if (framed == NULL || framed_len == 0) {
    free(framed);
    return -1;
  }

  if (_send_all(client->socket, framed, framed_len) != 0) {
    free(framed);
    return -1;
  }
  free(framed);
  return 0;
}

cbor_item_t* cli_client_recv_frame(cli_client_t* client) {
  if (client == NULL || !client->connected || client->socket == NULL) {
    return NULL;
  }

  uint8_t length_buf[4];
  if (_recv_all(client->socket, length_buf, sizeof(length_buf)) != 0) {
    return NULL;
  }

  uint32_t response_len = ((uint32_t)length_buf[0] << 24) |
                          ((uint32_t)length_buf[1] << 16) |
                          ((uint32_t)length_buf[2] << 8) |
                          (uint32_t)length_buf[3];

  if (response_len == 0 || response_len > MAX_RESPONSE_SIZE) {
    return NULL;
  }

  uint8_t* response_data = get_memory(response_len);
  if (response_data == NULL) {
    return NULL;
  }
  if (_recv_all(client->socket, response_data, response_len) != 0) {
    free(response_data);
    return NULL;
  }

  struct cbor_load_result load_result;
  cbor_item_t* response = cbor_load(response_data, response_len, &load_result);
  free(response_data);

  if (response == NULL || load_result.error.code != CBOR_ERR_NONE) {
    if (response != NULL) {
      cbor_decref(&response);
    }
    return NULL;
  }

  return response;
}
```

- [ ] **Step 3: Refactor `cli_client_send` to use both helpers**

Replace the entire body of `cli_client_send` (currently lines 136-201) with:

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

(The `request == NULL` short-circuit is preserved so existing callers that pass NULL on error paths still get NULL back without sending garbage.)

- [ ] **Step 4: Build**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
cmake --build build --target offs_cli -j$(nproc) 2>&1 | tail -5
cmake --build build-release --target offs_cli -j$(nproc) 2>&1 | tail -5
```

Expected: both exit 0, both produce `Built target offs_cli`.

- [ ] **Step 5: Smoke-test non-streaming commands still work**

Start the daemon in one terminal (if not already running):

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offsd --foreground --unix /tmp/offs.sock \
  --cache-dir /tmp/offs-cache --data-dir /tmp/offs-data
```

In another terminal, verify the refactored `cli_client_send` still works for synchronous request/response commands:

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offs --socket /tmp/offs.sock health
./build-release/offs --socket /tmp/offs.sock status
./build-release/offs --socket /tmp/offs.sock peer info
./build-release/offs --socket /tmp/offs.sock config show
```

Expected: each command prints a normal response (not "Cannot connect to daemon"). The exact output varies — what matters is that none of them regress to "Cannot connect" or empty output. If any does, the refactor broke `cli_client_send` — re-check Step 3.

- [ ] **Step 6: Commit**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add src/offs/client.h src/offs/client.c
git commit -m "refactor(offs-cli): split cli_client_send into send_frame and recv_frame helpers"
```

---

## Task 2: Rewrite `cmd_put` to stream the file

**Files:**
- Modify: `src/offs/commands/put.c` (entire `cmd_put` body + includes)

- [ ] **Step 1: Update includes in `put.c`**

Open `src/offs/commands/put.c`. The existing includes block (lines 1-12) is:

```c
#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include "Util/allocator.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
```

Add `<unistd.h>` (for `isatty`) and `<errno.h>` (for `strerror`), so the block becomes:

```c
#include "../client.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include "Util/allocator.h"
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
```

- [ ] **Step 2: Replace `cmd_put` body with the streaming implementation**

Replace the entire `cmd_put` function (currently the only function in the file, lines 14-89) with:

```c
#define PUT_CHUNK_SIZE (63 * 1024 * 1024)  /* just under 64 MB OFFS_MAX_CBOR_MESSAGE_SIZE */

int cmd_put(int argc, char** argv, cli_client_t* client) {
  if (argc < 1) {
    fprintf(stderr, "%s\n", L10N_PUT_USAGE);
    return 1;
  }

  const char* file_path = argv[0];
  uint8_t temporary = 0;
  char* recycler_url = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--temporary") == 0) {
      temporary = 1;
    } else if (strcmp(argv[i], "--recycler") == 0 && i + 1 < argc) {
      recycler_url = argv[++i];
    } else if (strcmp(argv[i], "--help") == 0) {
      printf("%s\n", L10N_PUT_USAGE);
      return 0;
    }
  }

  /* Open file and determine size */
  FILE* file = fopen(file_path, "rb");
  if (file == NULL) {
    perror("fopen");
    return 1;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    perror("fseek");
    fclose(file);
    return 1;
  }
  long file_size_l = ftell(file);
  if (file_size_l < 0) {
    perror("ftell");
    fclose(file);
    return 1;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    perror("fseek");
    fclose(file);
    return 1;
  }
  size_t file_size = (size_t)file_size_l;
  if (file_size == 0) {
    fprintf(stderr, "Error: empty file\n");
    fclose(file);
    return 1;
  }

  /* Send PUT_START frame: data = NULL, stream_length = total size */
  client_api_put_request_t put_req;
  memset(&put_req, 0, sizeof(put_req));
  put_req.content_type = (char*)"application/octet-stream";
  put_req.file_name = (char*)file_path;
  put_req.stream_length = file_size;
  put_req.data = NULL;
  put_req.data_size = 0;
  put_req.temporary = temporary;

  char* recycler_arr[1] = {recycler_url};
  if (recycler_url != NULL) {
    put_req.recycler_urls = recycler_arr;
    put_req.recycler_count = 1;
  }

  cbor_item_t* start_frame = client_api_put_request_encode(&put_req);
  if (start_frame == NULL) {
    fprintf(stderr, "Error: failed to encode PUT_START frame\n");
    fclose(file);
    return 1;
  }
  int send_rc = cli_client_send_frame(client, start_frame);
  cbor_decref(&start_frame);
  if (send_rc != 0) {
    fprintf(stderr, "Error: failed to send PUT_START frame: %s\n", strerror(errno));
    fclose(file);
    return 1;
  }

  /* Stream PUT_DATA frames */
  uint8_t* chunk = (uint8_t*)get_memory(PUT_CHUNK_SIZE);
  if (chunk == NULL) {
    fprintf(stderr, "Error: failed to allocate chunk buffer\n");
    fclose(file);
    return 1;
  }

  int progress_to_stderr = isatty(STDERR_FILENO);
  size_t bytes_sent = 0;
  int had_error = 0;

  while (bytes_sent < file_size) {
    size_t to_read = file_size - bytes_sent;
    if (to_read > PUT_CHUNK_SIZE) {
      to_read = PUT_CHUNK_SIZE;
    }
    size_t n = fread(chunk, 1, to_read, file);
    if (n == 0) {
      if (ferror(file)) {
        perror("fread");
        had_error = 1;
      }
      break;
    }

    client_api_put_data_t data_msg;
    memset(&data_msg, 0, sizeof(data_msg));
    data_msg.data = chunk;
    data_msg.data_size = n;

    cbor_item_t* data_frame = client_api_put_data_encode(&data_msg);
    if (data_frame == NULL) {
      fprintf(stderr, "Error: failed to encode PUT_DATA frame\n");
      had_error = 1;
      break;
    }
    send_rc = cli_client_send_frame(client, data_frame);
    cbor_decref(&data_frame);
    if (send_rc != 0) {
      fprintf(stderr, "Error: failed to send PUT_DATA frame: %s\n", strerror(errno));
      had_error = 1;
      break;
    }

    bytes_sent += n;
    if (progress_to_stderr) {
      double pct = file_size > 0 ? (100.0 * (double)bytes_sent / (double)file_size) : 100.0;
      fprintf(stderr, "\rPutting %s: %zu/%zu bytes (%.1f%%)",
              file_path, bytes_sent, file_size, pct);
      fflush(stderr);
    }
  }

  if (progress_to_stderr && bytes_sent > 0) {
    fputc('\n', stderr);
  }

  free(chunk);
  fclose(file);

  if (had_error) {
    return 1;
  }

  /* Send PUT_END frame */
  cbor_item_t* end_frame = client_api_put_end_encode();
  if (end_frame == NULL) {
    fprintf(stderr, "Error: failed to encode PUT_END frame\n");
    return 1;
  }
  send_rc = cli_client_send_frame(client, end_frame);
  cbor_decref(&end_frame);
  if (send_rc != 0) {
    fprintf(stderr, "Error: failed to send PUT_END frame: %s\n", strerror(errno));
    return 1;
  }

  /* Read the PUT_RESPONSE (or ERROR) frame from the server */
  cbor_item_t* response = cli_client_recv_frame(client);
  if (response == NULL) {
    fprintf(stderr, "Error: daemon closed connection without responding\n");
    return 1;
  }

  int result = 1;
  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_PUT_RESPONSE) {
    client_api_put_response_t put_resp;
    memset(&put_resp, 0, sizeof(put_resp));
    if (client_api_put_response_decode(response, &put_resp) == 0) {
      printf(L10N_PUT_IMPORTED "\n", put_resp.ori_string);
      client_api_put_response_destroy(&put_resp);
      result = 0;
    } else {
      fprintf(stderr, "Error: failed to decode PUT_RESPONSE\n");
    }
  } else if (type == CLIENT_API_ERROR) {
    client_api_error_t err_msg;
    memset(&err_msg, 0, sizeof(err_msg));
    if (client_api_error_decode(response, &err_msg) == 0) {
      fprintf(stderr, "%s: %s\n", L10N_ERROR, err_msg.message);
      client_api_error_destroy(&err_msg);
    } else {
      fprintf(stderr, "Error: undecodable error frame\n");
    }
  } else {
    fprintf(stderr, "Error: unexpected response type %u\n", type);
  }

  cbor_decref(&response);
  return result;
}
```

Notes for the implementer:
- `client_api_put_request_encode`, `client_api_put_data_encode`, `client_api_put_end_encode`, `client_api_put_response_decode`, `client_api_error_decode`, and `client_api_wire_get_type` are all declared in `deps/liboffs/src/ClientAPI/client_api_wire.h` (already transitively included via `"ClientAPI/client_api_wire.h"`).
- The single `chunk` buffer is allocated once at `PUT_CHUNK_SIZE` and reused for every chunk — peak client RSS is `PUT_CHUNK_SIZE` (63 MiB) regardless of file size.
- `isatty(STDERR_FILENO)` disables progress when stderr is redirected.
- `client_api_put_data_t.data` is `uint8_t*` non-owning — `client_api_put_data_encode` copies the bytes into the CBOR bytestring, so reusing `chunk` for the next fread is safe after the encode returns.

- [ ] **Step 3: Build**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
cmake --build build --target offs_cli -j$(nproc) 2>&1 | tail -5
cmake --build build-release --target offs_cli -j$(nproc) 2>&1 | tail -5
```

Expected: both exit 0, both produce `Built target offs_cli`.

- [ ] **Step 4: Smoke-test a tiny file (1 PUT_DATA frame)**

Start `offsd` if not already running (see Task 1 Step 5). Then:

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offs --socket /tmp/offs.sock put /etc/hostname
```

Expected: prints `File imported: <ori_string>` and exits 0. No progress bar (file is tiny, fits in one chunk — progress would still print once on stderr, which is the terminal, so you'll see one `\rPutting...` line).

- [ ] **Step 5: Test the original failing case (347 MB, multi-chunk)**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offs --socket /tmp/offs.sock put "/home/victor/Videos/Supacell.S01E01.720p.NF.WEBRip.x264[EZTVx.to].mkv"
```

Expected: a progress line on stderr updating in place (`\rPutting ...: N/364464450 bytes (X.X%)`), ending with `File imported: <ori_string>` on stdout, exit 0. The 347 MB file is split across 6 chunks (5 × 63 MiB + 1 × ~52 MiB).

If it still fails with `Invalid PUT request`, the server is rejecting the `PUT_START` frame — re-check that `put_req.data = NULL` and `put_req.data_size = 0` (NOT `data = chunk` with the whole file).

- [ ] **Step 6: Verify peak RSS is bounded by chunk size**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
/usr/bin/time -v ./build-release/offs --socket /tmp/offs.sock put "/home/victor/Videos/Supacell.S01E01.720p.NF.WEBRip.x264[EZTVx.to].mkv" 2>&1 | grep "Maximum resident"
```

Expected: roughly 70-100 MiB (63 MiB chunk + CBOR serialization overhead + libc). NOT 347 MiB. If it's near 347 MiB, the buffering isn't streaming — re-check that the chunk buffer is reused, not reallocated per chunk.

- [ ] **Step 7: Negative test — nonexistent file**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offs --socket /tmp/offs.sock put /nonexistent/path
```

Expected: `fopen: No such file or directory` (from `perror`), exit 1.

- [ ] **Step 8: Negative test — server down**

In the daemon terminal, press `Ctrl+C` to stop `offsd`. Then:

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offs --socket /tmp/offs.sock put /etc/hostname
```

Expected: an error message (either `Cannot connect to daemon` from `cli_client_connect` failing earlier in main, or `Error: failed to send PUT_START frame: ...` from the new code path), exit 1. Restart `offsd` after this step.

- [ ] **Step 9: Commit**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add src/offs/commands/put.c
git commit -m "feat(offs-cli): stream offs put in 63MB chunks instead of loading whole file"
```

---

## Task 3: Fix `cmd_get` to actually stream

**Files:**
- Modify: `src/offs/commands/get.c` (replace the initial `cli_client_send` call and the dead `cli_client_send(client, NULL)` loop)

- [ ] **Step 1: Replace the initial send and the dead loop**

Open `src/offs/commands/get.c`. The current body of `cmd_get` (lines 13-95) has the structure:

```c
int cmd_get(int argc, char** argv, cli_client_t* client) {
  // ... arg parsing (lines 14-30) ...
  // ... memset + encode get_req (lines 32-35) ...

  cbor_item_t* request = client_api_get_request_encode(&get_req);
  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);

  if (response == NULL) {
    fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
    return 1;
  }

  // ... type check + decode GET_RESPONSE_START (lines 40-66) ...
  // ... fopen output (line 64) ...

  /* Read data frames until GET_END */
  while ((response = cli_client_send(client, NULL)) != NULL) {
    // ... GET_DATA / GET_END / ERROR handling ...
  }

  // ... fclose + return (lines 92-95) ...
}
```

Make two replacements:

**Replacement A** — change the initial send from `cli_client_send` (which waits for a response that won't come yet) to `cli_client_send_frame` (write-only), then read GET_RESPONSE_START separately:

Find:

```c
cbor_item_t* request = client_api_get_request_encode(&get_req);
cbor_item_t* response = cli_client_send(client, request);
cbor_decref(&request);

if (response == NULL) {
  fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
  return 1;
}
```

Replace with:

```c
cbor_item_t* request = client_api_get_request_encode(&get_req);
if (request == NULL) {
  fprintf(stderr, "Error: failed to encode GET_REQUEST frame\n");
  return 1;
}
int send_rc = cli_client_send_frame(client, request);
cbor_decref(&request);
if (send_rc != 0) {
  fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
  return 1;
}

cbor_item_t* response = cli_client_recv_frame(client);
if (response == NULL) {
  fprintf(stderr, "%s\n", L10N_DAEMON_UNREACHABLE);
  return 1;
}
```

**Replacement B** — change the loop from the broken `cli_client_send(client, NULL)` (which returns NULL immediately, so the loop never executes) to `cli_client_recv_frame(client)`:

Find:

```c
  /* Read data frames until GET_END */
  while ((response = cli_client_send(client, NULL)) != NULL) {
```

Replace with:

```c
  /* Read data frames until GET_END */
  while ((response = cli_client_recv_frame(client)) != NULL) {
```

Leave the loop body (the `if (type == CLIENT_API_GET_DATA)` / `else if (type == CLIENT_API_GET_END)` / `else if (type == CLIENT_API_ERROR)` branches) unchanged — it's already correct.

Pre-existing issue (out of scope, do not fix in this task): the function falls through to `return 0;` after the loop, even if the loop broke on `CLIENT_API_ERROR`. That's a separate bug — the ERROR branch should `return 1`. The de-wonk audit in Task 4 Step 6 may flag this; treat it as pre-existing and out of scope, since the spec scoped this task to "the streaming loop callsite" only.

- [ ] **Step 2: Build**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
cmake --build build --target offs_cli -j$(nproc) 2>&1 | tail -5
cmake --build build-release --target offs_cli -j$(nproc) 2>&1 | tail -5
```

Expected: both exit 0.

- [ ] **Step 3: Smoke-test round-trip with a small file**

If `offsd` is not running, start it (Task 1 Step 5). Then put a small file (if not already put in Task 2):

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offs --socket /tmp/offs.sock put /etc/hostname
# Note the printed ORI string, e.g. "off://..."
./build-release/offs --socket /tmp/offs.sock get "<ori_from_above>" --output /tmp/hostname.out
diff /etc/hostname /tmp/hostname.out && echo "MATCH"
```

Expected: `MATCH` printed, exit 0. If the diff shows differences or `offs get` produces an empty file, the streaming loop isn't decoding frames correctly — re-check Replacement B.

- [ ] **Step 4: Round-trip the 347 MB file**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
# Use the ORI from Task 2 Step 5's put
./build-release/offs --socket /tmp/offs.sock get "<ori_from_supacell_put>" --output /tmp/supacell.out.mkv
sha256sum "/home/victor/Videos/Supacell.S01E01.720p.NF.WEBRip.x264[EZTVx.to].mkv" /tmp/supacell.out.mkv
```

Expected: both sha256 sums match. If they differ, the GET_DATA frames aren't being written in order or one is being dropped — re-check the loop body and the `fwrite(get_data.data, 1, get_data.data_size, output)` call.

- [ ] **Step 5: Negative test — bad ORI**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offs --socket /tmp/offs.sock get "off://invalid/not-a-real-ori" --output /tmp/should-not-exist
```

Expected: `Error: <message>` (from the server's `CLIENT_API_ERROR` frame), exit 1. The output file may or may not be created depending on where the server rejects — what matters is the non-zero exit and the error message.

- [ ] **Step 6: Commit**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
git add src/offs/commands/get.c
git commit -m "fix(offs-cli): use cli_client_recv_frame in offs get streaming loop"
```

---

## Task 4: Final regression verification

**Files:**
- None (verification only)

- [ ] **Step 1: Clean rebuild of both targets in both configurations**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
cmake --build build --target offsd offs_cli -j$(nproc) 2>&1 | tail -3
cmake --build build-release --target offsd offs_cli -j$(nproc) 2>&1 | tail -3
```

Expected: both exit 0, `Built target offs_cli` and `Built target offsd` present.

- [ ] **Step 2: Restart daemon with a clean cache**

```bash
# In the daemon terminal, Ctrl+C the old offsd if running
rm -rf /tmp/offs-cache /tmp/offs-data
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
./build-release/offsd --foreground --unix /tmp/offs.sock \
  --cache-dir /tmp/offs-cache --data-dir /tmp/offs-data
```

- [ ] **Step 3: Run the full test matrix**

In another terminal:

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
SOCK="--socket /tmp/offs.sock"

# Non-streaming regression
./build-release/offs $SOCK health
./build-release/offs $SOCK status
./build-release/offs $SOCK peer info
./build-release/offs $SOCK config show

# Streaming put — tiny
./build-release/offs $SOCK put /etc/hostname

# Streaming put — medium (under cap, single chunk)
./build-release/offs $SOCK put ./README.md

# Streaming put — large (multi-chunk, the original failing case)
./build-release/offs $SOCK put "/home/victor/Videos/Supacell.S01E01.720p.NF.WEBRip.x264[EZTVx.to].mkv"

# Round-trip — verify each put can be gotten back identically
for ORI in $(./build-release/offs $SOCK health | grep -o 'off://[^"]*' | head -3); do
  ./build-release/offs $SOCK get "$ORI" --output /tmp/rt.out
  # manual: compare /tmp/rt.out to the original
done

# Negative: nonexistent file
./build-release/offs $SOCK put /nonexistent && echo "BUG: should have failed" || echo "OK"

# Negative: bad ORI
./build-release/offs $SOCK get "off://invalid" --output /tmp/no.out && echo "BUG" || echo "OK"
```

Expected: every command succeeds with the expected output, every `put` prints `File imported: <ori>`, every round-trip matches, and the two negative cases exit non-zero.

- [ ] **Step 4: Verify peak RSS on the 347 MB upload is bounded**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
/usr/bin/time -v ./build-release/offs --socket /tmp/offs.sock put "/home/victor/Videos/Supacell.S01E01.720p.NF.WEBRip.x264[EZTVx.to].mkv" 2>&1 | grep -E "Maximum resident|Elapsed"
```

Expected: `Maximum resident set size` is roughly 70-100 MiB (not 347 MiB). If it's near 347 MiB, the streaming isn't bounding memory — re-check Task 2.

- [ ] **Step 5: Confirm no `cli_client_send(client, NULL)` calls remain**

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
grep -rn "cli_client_send(client, NULL)" src/offs/
```

Expected: no output. If anything prints, there's another broken callsite to fix — investigate.

- [ ] **Step 6: Run de-wonk audit**

Use the de-wonk skill to scan the three changed files (`src/offs/client.c`, `src/offs/commands/put.c`, `src/offs/commands/get.c`) for unimplemented, stubbed, disabled, broken, or weird code introduced by this implementation.

- [ ] **Step 7: Check for memory leaks**

Per project convention, run valgrind on the CLI doing a small upload+download round trip:

```bash
cd /home/victor/Workspace/src/github.com/vijayee/OFFS
# Need a Debug build with DWARF-4 for valgrind < 3.19 compatibility
cmake -S . -B build-gdwarf4 -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-gdwarf-4" -DCMAKE_CXX_FLAGS="-gdwarf-4"
cmake --build build-gdwarf4 --target offsd offs_cli -j$(nproc)
# Start build-gdwarf4/offsd in a separate terminal with --unix /tmp/offs-gd.sock
valgrind --leak-check=full --error-exitcode=1 \
  ./build-gdwarf4/offs --socket /tmp/offs-gd.sock put /etc/hostname 2>&1 | tail -10
```

Expected: `All heap blocks were freed -- no leaks are possible` and `ERROR SUMMARY: 0 errors`. If valgrind isn't available or `build-gdwarf4` is too costly to set up, skip this step and note it in the wrap-up — the CLI is a short-lived process, leaks are less critical than for the long-running daemon.

---

## Notes for the implementer

- The `cli_client_send` short-circuit on `request == NULL` is **preserved** in the refactored version (Task 1 Step 3) — it's an error-path guard, not part of the streaming protocol. Don't remove it.
- `client_api_put_data_t.data` is a non-owning `uint8_t*` — `client_api_put_data_encode` copies the bytes into a CBOR bytestring, so reusing the `chunk` buffer for the next `fread` is safe.
- `cli_client_send_frame` does NOT consume the `cbor_item_t*` — caller must `cbor_decref` after the call, matching the existing `cli_client_send` convention.
- The 4-byte big-endian length prefix is the same framing the server uses (`stream_frame_encode` / `stream_framer_*` in liboffs) — no protocol change here, just exposing the two halves separately.
- The server sends `PUT_RESPONSE` only after `PUT_END` (when `writeable_off_stream_finalize` fires the close-event subscriber that calls `_unix_connection_send_frame`). Don't try to read a response between PUT_DATA frames — there isn't one.
- For `offs get`, the server pushes `GET_DATA` frames asynchronously as the readable stream produces data. The loop in `cmd_get` just reads them one at a time until `GET_END` or `ERROR`. There's no flow control on the client side — if the daemon produces data faster than `fwrite` can absorb, the kernel socket buffer handles backpressure.
- If a `put` or `get` fails mid-stream with a socket error, the daemon cleans up its in-progress stream automatically on connection drop (`deps/liboffs/src/ClientAPI/Unix/unix_connection.c:1104` resets `put_streaming = 0` on destroy). No explicit cancel frame is needed.
- The `--lang` global flag (per `src/offs/main.c:24-27`) is handled before any command runs; the changes here don't interact with it.