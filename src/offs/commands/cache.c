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
  if (request == NULL) { rc = 1; goto out; }

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
  snprintf(dest, sizeof(dest), "%s", dest_text);

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
  char src[1024];
  if (cli_cache_resolve_current(from_flag, config_dir, src, sizeof(src)) != 0) {
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
    fprintf(stderr, L10N_CACHE_MOVE_FAILED, failed_path, dest);
    /* Revert staging so the daemon stays on the old location. Files that
       moved before the failure stay at dest; the nonempty-dest check
       rejects a retried move until they are cleared. */
    _stage_string_or_null(config_dir, "cache_dir", NULL);
    if (was_running) cmd_start(0, NULL, NULL);
    return 1;
  }

  /* The move emptied the tree; remove the source directory shell. */
  rm_rf(src);
  printf(L10N_CACHE_MOVED, src, dest);
  if (was_running) cmd_start(0, NULL, NULL);
  return 0;
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
  fprintf(stderr, "%s\n", L10N_CACHE_USAGE);
  return 1;
}