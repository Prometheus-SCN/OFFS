//
// Created by victor on 5/28/26.
//

#ifndef OFFS_CLI_UTIL_H
#define OFFS_CLI_UTIL_H

#include "client.h"

typedef struct {
  const char* name;
  const char* description;
  int (*handler)(int argc, char** argv, cli_client_t* client);
} cli_command_t;

/* Returns a pointer to the internal command table (NULL-terminated). */
const cli_command_t* cli_command_table(void);

/* The daemon's local socket path: OFFS_LOCAL_SOCKET_PATH unless --socket
 * was passed (main lifts --socket out of argv and stores it here). The
 * cache command self-connects through this so --socket is honored. */
const char* cli_socket_path(void);
void cli_set_socket_path(const char* path);

/* Detect language from OFFS_LANG or LANG environment variables. */
const char* cli_detect_lang(void);

/* Print help for a specific command or all commands if command_name is NULL. */
void cli_print_help(const char* command_name);

/* Print an error frame if the response is one. Returns 1 if it was an error
   frame (already printed), 0 otherwise. */
int cli_print_error_if(cbor_item_t* response);

/* 1 when the offsd process (or the offs-daemon service on Windows) is
   running, 0 otherwise. Wraps start_stop's daemon probe so other
   commands can coordinate around stop/start cycles. */
int cli_daemon_is_running(void);

/* Poll cli_daemon_is_running until it reports stopped or timeout_ms
   elapses. Returns 0 once stopped, -1 if still running after the
   timeout. */
int cli_daemon_wait_stopped(int timeout_ms);

/* --- cache move helpers (pure, exported for testoffs) --- */

/* 1 when the two paths name the same directory, 2 when one is inside the
   other, 0 when they are disjoint. Both separators are equivalent,
   duplicates and trailing separators are ignored, and the compare is
   case-insensitive on Windows. Paths too long to normalize count as
   nested (refuse rather than guess). */
int cli_cache_paths_conflict(const char* a, const char* b);

/* Move every entry under src (recursively) into dst, preserving the
   relative layout, then remove the emptied source subdirectories. dst
   must already exist (created by the caller). verify_mode selects
   per-file move verification (FILE_VERIFY_SHA256 / FILE_VERIFY_NONE).
   Returns 0 on success; on the first failure -1 with the failing source
   path copied into failed_path (may be NULL). Files moved before the
   failure stay moved; entries after it are left in src. */
int cli_cache_move_tree(const char* src, const char* dst, int verify_mode,
                        char* failed_path, size_t failed_len);

/* Read the daemon config file's [daemon].config-dir and [cache].dir.
   Outputs are always NUL-terminated, empty when the key is absent.
   Returns 0 if the file was parsed, -1 if unreadable or not JSON. */
int cli_cache_read_config_file_dirs(const char* config_path,
                                    char* config_dir_out, size_t config_dir_size,
                                    char* cache_dir_out, size_t cache_dir_size);

/* Resolve the daemon's config directory. Precedence: flag (--config-dir) >
   [daemon].config-dir in the daemon config file (honoring $OFFS_CONFIG) >
   platform default. Returns 0 with out filled, -1 when nothing resolves. */
int cli_cache_resolve_config_dir(const char* flag, char* out, size_t out_size);

/* Resolve the cache directory currently in effect. Precedence:
   from_flag (--from) > [cache].dir in the daemon config file
   (offs_default_config_path_get, honoring $OFFS_CONFIG) > pending
   cache_dir in {config_dir}/pending_config.json > platform default.
   config_dir may be NULL to resolve it the same way the pending write
   does. Returns 0 with out filled, -1 when nothing resolves. */
int cli_cache_resolve_current(const char* from_flag, const char* config_dir,
                              char* out, size_t out_size);

#endif /* OFFS_CLI_UTIL_H */
