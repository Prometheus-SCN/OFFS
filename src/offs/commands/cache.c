//
// Created by victor on 9/28/26.
//
// offs cache — fast block-cache management.
//
//   size <value>  Resize the cache. Applied live through the daemon when
//                 reachable; staged for the next start when not.
//   move <dest>   Relocate the cache directory, moving its storage.
//
// The move flow stops the daemon itself (auto stop + start), stages
// cache_dir into pending_config.json BEFORE moving anything so a failed
// move reverts cleanly, then copies each file with verify and deletes the
// source only after verification (--unsafe skips verification).

#include "../client.h"
#include "../cli_util.h"
#include "../l10n/en.h"
#include "ClientAPI/client_api_wire.h"
#include "Configuration/config_pending.h"
#include "Platform/platform_posix_compat.h"
#include "Util/file_copy.h"
#include "Util/mkdir_p.h"
#include "Util/rm_rf.h"
#include <cbor.h>
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Extern from commands/start_stop.c — the move flow owns its stop/start
   cycle and calls these directly (no client connection is used by them). */
int cmd_start(int argc, char** argv, cli_client_t* client);
int cmd_stop(int argc, char** argv, cli_client_t* client);

static void _print_cache_help(void) {
  printf("%s\n", L10N_CACHE_USAGE);
}

/* --- size value parsing ------------------------------------------------- */

/* Parse a byte count with optional unit suffix: plain digits are bytes;
   TiB/GiB/MiB/KiB are 1024-based, TB/GB/MB/KB are decimal 1000-based
   (case-insensitive). Rejects 0, signs, empty input, unknown/trailing
   text, and overflow. Returns 0 with *out filled, -1 on any problem. */
static int _parse_size_bytes(const char* text, uint64_t* out) {
  if (text == NULL || *text == '\0') return -1;
  if (*text == '-' || *text == '+') return -1;

  size_t i = 0;
  uint64_t value = 0;
  while (text[i] >= '0' && text[i] <= '9') {
    unsigned digit = (unsigned)(text[i] - '0');
    if (value > UINT64_MAX / 10) return -1;
    value *= 10;
    if (value > UINT64_MAX - digit) return -1;
    value += digit;
    i++;
  }
  if (i == 0) return -1;

  uint64_t multiplier = 1;
  if (text[i] != '\0') {
    static const struct {
      const char* suffix;
      uint64_t mult;
    } units[] = {
      {"TiB", 1024ULL * 1024 * 1024 * 1024},
      {"TB",  1000000000000ULL},
      {"GiB", 1024ULL * 1024 * 1024},
      {"GB",  1000000000ULL},
      {"MiB", 1024ULL * 1024},
      {"MB",  1000000ULL},
      {"KiB", 1024ULL},
      {"KB",  1000ULL},
    };
    const char* suffix = text + i;
    size_t suffix_len = strlen(suffix);
    size_t u = 0;
    while (u < sizeof(units) / sizeof(units[0])) {
      if (strlen(units[u].suffix) == suffix_len) {
        size_t s = 0;
        while (units[u].suffix[s] != '\0' &&
               tolower((unsigned char)units[u].suffix[s]) == tolower((unsigned char)suffix[s])) {
          s++;
        }
        if (units[u].suffix[s] == '\0') {
          multiplier = units[u].mult;
          break;
        }
      }
      u++;
    }
    if (u == sizeof(units) / sizeof(units[0])) return -1;  /* bad suffix */
  }

  if (value == 0 || value > UINT64_MAX / multiplier) return -1;
  *out = value * multiplier;
  return 0;
}

/* --- size: live apply through the daemon -------------------------------- */

/* Send the resize request to the daemon. Returns 0 when applied live,
   1 when the daemon rejected it or the exchange failed, -1 when the
   daemon is unreachable (caller falls back to staging). */
static int _cache_size_apply(uint64_t bytes) {
  cli_client_t* client = cli_client_create(cli_socket_path());
  if (client == NULL) return -1;
  int rc = -1;

  if (cli_client_connect(client) != 0) goto out;

  client_api_cache_resize_request_t req;
  memset(&req, 0, sizeof(req));
  req.capacity_bytes = bytes;
  cbor_item_t* request = client_api_cache_resize_request_encode(&req);
  if (request == NULL) {
    fprintf(stderr, "%s\n", L10N_CACHE_SIZE_ENCODE);
    rc = 1;
    goto out;
  }

  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  if (response == NULL) goto out;  /* unreachable mid-exchange */

  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_ERROR) {
    cli_print_error_if(response);
    cbor_decref(&response);
    rc = 1;
    goto out;
  }
  if (type != CLIENT_API_CACHE_RESIZE_RESPONSE) {
    fprintf(stderr, L10N_REP_UNEXPECTED_TYPE, type);
    cbor_decref(&response);
    rc = 1;
    goto out;
  }

  client_api_cache_resize_response_t rep;
  memset(&rep, 0, sizeof(rep));
  if (client_api_cache_resize_response_decode(response, &rep) != 0) {
    fprintf(stderr, "%s\n", L10N_CACHE_SIZE_DECODE);
    cbor_decref(&response);
    rc = 1;
    goto out;
  }
  cbor_decref(&response);

  if (rep.status != 0) {
    fprintf(stderr, L10N_CACHE_SIZE_REJECTED, rep.status);
    rc = 1;
    goto out;
  }
  printf(L10N_CACHE_SIZE_SET,
         (unsigned long long)rep.max_capacity_bytes,
         (unsigned long long)rep.current_bytes);
  rc = 0;

out:
  cli_client_destroy(client);
  return rc;
}

/* --- pending-config staging --------------------------------------------- */

/* Stage {"field": number} into {config_dir}/pending_config.json (merged
   with whatever is already pending). Returns 0 / -1. */
static int _stage_number(const char* config_dir, const char* field, uint64_t value) {
  cJSON* obj = cJSON_CreateObject();
  if (obj == NULL) return -1;
  cJSON_AddNumberToObject(obj, field, (double)value);
  char* text = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  if (text == NULL) return -1;
  int rc = config_pending_save(config_dir, text, strlen(text));
  free(text);
  return rc;
}

/* Stage {"field": string-or-null}. A NULL value reverts the field to its
   default (used to roll back a failed move). Returns 0 / -1. */
static int _stage_string_or_null(const char* config_dir, const char* field,
                                 const char* value) {
  cJSON* obj = cJSON_CreateObject();
  if (obj == NULL) return -1;
  if (value != NULL) {
    cJSON_AddStringToObject(obj, field, value);
  } else {
    cJSON_AddNullToObject(obj, field);
  }
  char* text = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  if (text == NULL) return -1;
  int rc = config_pending_save(config_dir, text, strlen(text));
  free(text);
  return rc;
}

/* --- size subcommand ----------------------------------------------------- */

static int _cmd_cache_size(int argc, char** argv) {
  const char* value_text = NULL;
  const char* config_dir_flag = NULL;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "help") == 0) {
      printf("%s\n", L10N_CACHE_SIZE_USAGE);
      return 0;
    }
    if (strcmp(argv[i], "--config-dir") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s\n", L10N_CACHE_SIZE_USAGE);
        return 1;
      }
      config_dir_flag = argv[++i];
      continue;
    }
    if (argv[i][0] == '-' || value_text != NULL) {
      fprintf(stderr, "%s\n", L10N_CACHE_SIZE_USAGE);
      return 1;
    }
    value_text = argv[i];
  }

  uint64_t bytes = 0;
  if (value_text == NULL || _parse_size_bytes(value_text, &bytes) != 0) {
    fprintf(stderr, "%s\n", L10N_CACHE_SIZE_USAGE);
    return 1;
  }

  int applied = _cache_size_apply(bytes);
  if (applied == 0) return 0;
  if (applied == 1) return 1;

  /* Daemon unreachable: stage the size for the next start. */
  char config_dir[1024];
  if (cli_cache_resolve_config_dir(config_dir_flag, config_dir, sizeof(config_dir)) != 0 ||
      _stage_number(config_dir, "max_capacity_bytes", bytes) != 0) {
    fprintf(stderr, "%s\n", L10N_CACHE_STAGE_FAILED);
    return 1;
  }
  printf(L10N_CACHE_SIZE_STAGED, (unsigned long long)bytes);
  return 0;
}

/* --- move subcommand ------------------------------------------------------ */

/* Make path absolute against the process cwd when it is relative, so a
   staged cache_dir does not depend on whatever cwd the daemon later
   starts with. Returns 0 with out filled, -1 when it does not fit. */
static int _absolutize(const char* path, char* out, size_t out_size) {
#ifdef _WIN32
  DWORD len = GetFullPathNameA(path, (DWORD)out_size, out, NULL);
  return (len > 0 && len < out_size) ? 0 : -1;
#else
  if (path[0] == '/') {
    if (snprintf(out, out_size, "%s", path) >= (int)out_size) return -1;
    return 0;
  }
  char cwd[512];
  if (getcwd(cwd, sizeof(cwd)) == NULL) return -1;
  if (snprintf(out, out_size, "%s/%s", cwd, path) >= (int)out_size) return -1;
  return 0;
#endif
}

static int _cmd_cache_move(int argc, char** argv) {
  const char* dest_text = NULL;
  const char* from_flag = NULL;
  const char* config_dir_flag = NULL;
  int unsafe = 0;
  int keep = 0;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "help") == 0) {
      printf("%s\n", L10N_CACHE_MOVE_USAGE);
      return 0;
    }
    if (strcmp(argv[i], "--from") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s\n", L10N_CACHE_MOVE_USAGE);
        return 1;
      }
      from_flag = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "--config-dir") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s\n", L10N_CACHE_MOVE_USAGE);
        return 1;
      }
      config_dir_flag = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "--unsafe") == 0) { unsafe = 1; continue; }
    if (strcmp(argv[i], "--keep") == 0) { keep = 1; continue; }
    if (argv[i][0] == '-' || dest_text != NULL) {
      fprintf(stderr, "%s\n", L10N_CACHE_MOVE_USAGE);
      return 1;
    }
    dest_text = argv[i];
  }
  if (dest_text == NULL) {
    fprintf(stderr, "%s\n", L10N_CACHE_MOVE_USAGE);
    return 1;
  }

  char dest[1024];
  if (_absolutize(dest_text, dest, sizeof(dest)) != 0) {
    fprintf(stderr, L10N_CACHE_DEST_NOT_DIR, dest_text);
    return 1;
  }

  if (unsafe) {
    fprintf(stderr, "%s\n", L10N_CACHE_UNSAFE_WARNING);
  }

  /* Validate the destination BEFORE touching the daemon. */
  struct stat st;
  if (stat(dest, &st) == 0) {
    if (!S_ISDIR(st.st_mode)) {
      fprintf(stderr, L10N_CACHE_DEST_NOT_DIR, dest);
      return 1;
    }
    if (dir_is_empty(dest) == 0) {
      fprintf(stderr, L10N_CACHE_DEST_NOT_EMPTY, dest);
      return 1;
    }
  } else if (mkdir_p(dest) != 0) {
    fprintf(stderr, L10N_CACHE_DEST_NOT_DIR, dest);
    return 1;
  }

  char config_dir[1024];
  if (cli_cache_resolve_config_dir(config_dir_flag, config_dir, sizeof(config_dir)) != 0) {
    fprintf(stderr, "%s\n", L10N_CACHE_STAGE_FAILED);
    return 1;
  }
  char resolved_src[1024];
  if (cli_cache_resolve_current(from_flag, config_dir, resolved_src,
                                sizeof(resolved_src)) != 0) {
    fprintf(stderr, "%s\n", L10N_CACHE_SRC_UNRESOLVED);
    return 1;
  }
  /* The config file or a flag may name the source relative to the caller's
     cwd; the staged cache_dir and the move must agree on the same place the
     daemon will see. */
  char src[1024];
  if (_absolutize(resolved_src, src, sizeof(src)) != 0) {
    fprintf(stderr, "%s\n", L10N_CACHE_SRC_UNRESOLVED);
    return 1;
  }

  int conflict = cli_cache_paths_conflict(src, dest);
  if (conflict == 1) {
    fprintf(stderr, "%s\n", L10N_CACHE_DEST_SAME);
    return 1;
  }
  if (conflict == 2) {
    fprintf(stderr, "%s\n", L10N_CACHE_DEST_NESTED);
    return 1;
  }

  /* Source missing or empty: nothing to move — stage the new location
     and report, without touching the daemon. */
  int src_state = dir_is_empty(src);  /* 1 empty, 0 nonempty, -1 missing */
  if (src_state != 0) {
    if (_stage_string_or_null(config_dir, "cache_dir", dest) != 0) {
      fprintf(stderr, "%s\n", L10N_CACHE_STAGE_FAILED);
      return 1;
    }
    printf(L10N_CACHE_NO_CONTENT, src);
    return 0;
  }

  /* --keep stages the move for a later restart; never stop the daemon. */
  if (keep) {
    if (_stage_string_or_null(config_dir, "cache_dir", dest) != 0) {
      fprintf(stderr, "%s\n", L10N_CACHE_STAGE_FAILED);
      return 1;
    }
    printf(L10N_CACHE_KEEP_STAGED, dest);
    return 0;
  }

  int was_running = cli_daemon_is_running();
  if (was_running) {
    if (cmd_stop(0, NULL, NULL) != 0 ||
        cli_daemon_wait_stopped(10000) != 0 ||
        cli_daemon_is_running()) {
      fprintf(stderr, "%s\n", L10N_CACHE_STOP_FAILED);
      return 1;
    }
  }

  /* Stage BEFORE moving so a failed move reverts by staging null. */
  if (_stage_string_or_null(config_dir, "cache_dir", dest) != 0) {
    fprintf(stderr, "%s\n", L10N_CACHE_STAGE_FAILED);
    if (was_running) cmd_start(0, NULL, NULL);
    return 1;
  }

  char failed_path[1024];
  int verify_mode = unsafe ? FILE_VERIFY_NONE : FILE_VERIFY_SHA256;
  if (cli_cache_move_tree(src, dest, verify_mode, failed_path, sizeof(failed_path)) != 0) {
    /* Revert staging FIRST so the daemon stays on the old location; files
       that moved before the failure stay at dest and the nonempty-dest
       check rejects a retried move until they are cleared. */
    int reverted = (_stage_string_or_null(config_dir, "cache_dir", NULL) == 0);
    fprintf(stderr, L10N_CACHE_MOVE_FAILED, failed_path, dest);
    if (!reverted) {
      fprintf(stderr, L10N_CACHE_REVERT_FAILED, dest);
    }
    if (was_running) cmd_start(0, NULL, NULL);
    return 1;
  }

  /* The move emptied the tree; remove the source directory shell. A
     leftover shell is harmless — the daemon already runs from dest. */
  if (rm_rf(src) != 0) {
    fprintf(stderr, L10N_CACHE_SRC_SHELL_LEFT, src);
  }
  printf(L10N_CACHE_MOVED, src, dest);
  if (was_running) cmd_start(0, NULL, NULL);
  return 0;
}

/* --- gc subcommand -------------------------------------------------------- */

/* Keep-list failure reasons (liboffs BlockCache/block_gc.h GC_LINE_*) —
   mirrored here as literals because the CLI does not link the daemon-side
   BlockCache headers. The HTTP route answers with the same names. */
static const char* _gc_reason_name(uint8_t reason) {
  switch (reason) {
    case 1: return "malformed_url";
    case 2: return "missing_descriptor";
    case 3: return "malformed_descriptor";
    case 4: return "cycle";
    default: return "unknown";
  }
}

/* Read the keep-list file into a NUL-terminated buffer capped at
   CLIENT_API_GC_MAX_URLS_TEXT (the wire pair's own cap; the daemon rejects
   anything larger on decode, so gate it here with a specific message).
   Returns 0 with *out set (caller frees) on success, -1 on an I/O error
   (already perror'd), 1 when the file is empty, 2 when it exceeds the cap. */
static int _read_gc_keep_file(const char* path, char** out) {
  FILE* file = fopen(path, "rb");
  if (file == NULL) {
    perror(path);
    return -1;
  }
  const long cap = (long)CLIENT_API_GC_MAX_URLS_TEXT;
  long file_length = -1;
  if (fseek(file, 0, SEEK_END) == 0) {
    file_length = ftell(file);
  }
  if (file_length < 0) {
    perror("fseek");
    fclose(file);
    return -1;
  }
  if (file_length == 0) {
    fclose(file);
    return 1;
  }
  if (file_length > cap) {
    fclose(file);
    return 2;
  }
  rewind(file);
  char* text = (char*)malloc((size_t)file_length + 1);
  if (text == NULL) {
    perror("malloc");
    fclose(file);
    return -1;
  }
  if (fread(text, 1, (size_t)file_length, file) != (size_t)file_length) {
    perror("fread");
    free(text);
    fclose(file);
    return -1;
  }
  text[file_length] = '\0';
  fclose(file);
  *out = text;
  return 0;
}

/* Send the keep-list sweep to the daemon. Returns 0 when the sweep completed
   (failed lines still reported as warnings), 1 when it was refused or the
   exchange failed, -1 when the daemon is unreachable. */
static int _cache_gc_apply(const char* urls_text, uint8_t force, uint8_t defrag) {
  cli_client_t* client = cli_client_create(cli_socket_path());
  if (client == NULL) return -1;
  int rc = -1;

  if (cli_client_connect(client) != 0) goto out;

  client_api_gc_request_t req;
  memset(&req, 0, sizeof(req));
  req.urls = (char*)urls_text;  /* borrowed for the encode call */
  req.force = force;
  req.defrag = defrag;
  cbor_item_t* request = client_api_gc_request_encode(&req);
  if (request == NULL) {
    fprintf(stderr, "%s\n", L10N_CACHE_GC_ENCODE);
    rc = 1;
    goto out;
  }

  cbor_item_t* response = cli_client_send(client, request);
  cbor_decref(&request);
  if (response == NULL) goto out;  /* unreachable mid-exchange */

  uint8_t type = client_api_wire_get_type(response);
  if (type == CLIENT_API_ERROR) {
    cli_print_error_if(response);
    cbor_decref(&response);
    rc = 1;
    goto out;
  }
  if (type != CLIENT_API_GC_RESPONSE) {
    fprintf(stderr, L10N_REP_UNEXPECTED_TYPE, type);
    cbor_decref(&response);
    rc = 1;
    goto out;
  }

  client_api_gc_response_t rep;
  memset(&rep, 0, sizeof(rep));
  if (client_api_gc_response_decode(response, &rep) != 0) {
    fprintf(stderr, "%s\n", L10N_CACHE_GC_DECODE);
    cbor_decref(&response);
    rc = 1;
    goto out;
  }
  cbor_decref(&response);

  if (rep.status != 0) {
    fprintf(stderr, L10N_CACHE_GC_REJECTED "\n", rep.status);
  } else if (rep.urls_request > rep.urls_collected) {
    /* The sweep ran but some lines failed; per-line warnings come below. */
    fprintf(stderr, L10N_CACHE_GC_PARTIAL "\n");
  }
  printf(L10N_CACHE_GC_SUMMARY,
         (unsigned long long)rep.urls_collected,
         (unsigned long long)rep.urls_request,
         (unsigned long long)rep.blocks_deleted,
         (unsigned long long)rep.blocks_kept);
  printf(L10N_CACHE_GC_SKIPPED,
         (unsigned long long)rep.skipped_pinned,
         (unsigned long long)rep.skipped_claimed);
  if (rep.failed != NULL) {
    size_t failed_count = cbor_array_size(rep.failed);
    for (size_t row_index = 0; row_index < failed_count; row_index++) {
      cbor_item_t* row = cbor_array_get(rep.failed, row_index);
      cbor_item_t* line_item = cbor_array_size(row) >= 1 ? cbor_array_get(row, 0) : NULL;
      cbor_item_t* reason_item = cbor_array_size(row) >= 2 ? cbor_array_get(row, 1) : NULL;
      cbor_item_t* text_item = cbor_array_size(row) >= 3 ? cbor_array_get(row, 2) : NULL;
      if (line_item != NULL && cbor_isa_uint(line_item) &&
          reason_item != NULL && cbor_isa_uint(reason_item) &&
          text_item != NULL && cbor_isa_string(text_item)) {
        fprintf(stderr, L10N_CACHE_GC_FAILED_LINE,
                (unsigned long long)cbor_get_uint64(line_item),
                _gc_reason_name((uint8_t)cbor_get_uint64(reason_item)),
                (const char*)cbor_string_handle(text_item));
      }
      if (line_item != NULL) cbor_decref(&line_item);
      if (reason_item != NULL) cbor_decref(&reason_item);
      if (text_item != NULL) cbor_decref(&text_item);
      cbor_decref(&row);
    }
  }
  if (rep.defrag_applied) {
    printf(L10N_CACHE_GC_DEFRAG,
           (unsigned long long)rep.defrag_sections,
           (unsigned long long)rep.defrag_blocks_relocated);
  }

  rc = (rep.status == 0) ? 0 : 1;
  client_api_gc_response_destroy(&rep);

out:
  cli_client_destroy(client);
  return rc;
}

static int _cmd_cache_gc(int argc, char** argv) {
  const char* from_text = NULL;
  int force = 0;
  int defrag = 0;
  for (int i = 0; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "help") == 0) {
      printf("%s\n", L10N_CACHE_GC_USAGE);
      return 0;
    }
    if (strcmp(argv[i], "--from") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s\n", L10N_CACHE_GC_USAGE);
        return 1;
      }
      from_text = argv[++i];
      continue;
    }
    if (strcmp(argv[i], "--force") == 0) { force = 1; continue; }
    if (strcmp(argv[i], "--defrag") == 0) { defrag = 1; continue; }
    /* A bare positional has no meaning here: the keep list comes only from
       --from. Reject anything else instead of sweeping with the wrong data. */
    fprintf(stderr, "%s\n", L10N_CACHE_GC_USAGE);
    return 1;
  }
  if (from_text == NULL) {
    fprintf(stderr, "%s\n", L10N_CACHE_GC_FROM_REQUIRED);
    return 1;
  }

  char* urls_text = NULL;
  int read_rc = _read_gc_keep_file(from_text, &urls_text);
  if (read_rc == 1) {
    fprintf(stderr, "%s\n", L10N_CACHE_GC_EMPTY_FILE);
    return 1;
  }
  if (read_rc == 2) {
    fprintf(stderr, "%s\n", L10N_CACHE_GC_TOO_LARGE);
    return 1;
  }
  if (read_rc != 0) {
    return 1;  /* details already reported via perror */
  }

  int applied = _cache_gc_apply(urls_text, (uint8_t)force, (uint8_t)defrag);
  free(urls_text);
  if (applied == -1) {
    fprintf(stderr, "%s\n", L10N_CACHE_GC_UNREACHABLE);
    return 1;
  }
  return applied;
}

/* --- dispatch ------------------------------------------------------------- */

int cmd_cache(int argc, char** argv, cli_client_t* client) {
  (void)client;
  if (argc < 1 || strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "help") == 0) {
    _print_cache_help();
    return 0;
  }
  if (strcmp(argv[0], "size") == 0) return _cmd_cache_size(argc - 1, argv + 1);
  if (strcmp(argv[0], "move") == 0) return _cmd_cache_move(argc - 1, argv + 1);
  if (strcmp(argv[0], "gc") == 0) return _cmd_cache_gc(argc - 1, argv + 1);
  fprintf(stderr, "%s\n", L10N_CACHE_USAGE);
  return 1;
}