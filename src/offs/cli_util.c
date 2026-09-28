//
// Created by victor on 5/28/26.
//

#include "cli_util.h"
#include "l10n/en.h"
#include "../Service/local_socket.h"
#include "ClientAPI/client_api_wire.h"
#include "Configuration/config.h"
#include "Configuration/config_pending.h"
#include "Platform/platform_dirs.h"
#include "Util/file_copy.h"
#include "Util/mkdir_p.h"
#include "Util/rm_rf.h"
#include <cJSON.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

/* Forward declarations for command handlers (implemented in commands/) */
int cmd_put(int argc, char** argv, cli_client_t* client);
int cmd_get(int argc, char** argv, cli_client_t* client);
int cmd_load(int argc, char** argv, cli_client_t* client);
int cmd_block(int argc, char** argv, cli_client_t* client);
int cmd_ephemeral(int argc, char** argv, cli_client_t* client);
int cmd_pin(int argc, char** argv, cli_client_t* client);
int cmd_unpin(int argc, char** argv, cli_client_t* client);
int cmd_peer(int argc, char** argv, cli_client_t* client);
int cmd_config(int argc, char** argv, cli_client_t* client);
int cmd_friend(int argc, char** argv, cli_client_t* client);
int cmd_bootstrap(int argc, char** argv, cli_client_t* client);
int cmd_health(int argc, char** argv, cli_client_t* client);
int cmd_status(int argc, char** argv, cli_client_t* client);
int cmd_version(int argc, char** argv, cli_client_t* client);
int cmd_start(int argc, char** argv, cli_client_t* client);
int cmd_stop(int argc, char** argv, cli_client_t* client);
int cmd_restart(int argc, char** argv, cli_client_t* client);
int cmd_cache(int argc, char** argv, cli_client_t* client);

static cli_command_t g_commands[] = {
  {"start",   L10N_START_DESC,   cmd_start},
  {"stop",    L10N_STOP_DESC,    cmd_stop},
  {"restart", L10N_RESTART_DESC, cmd_restart},
  {"put",     L10N_PUT_DESC,     cmd_put},
  {"get",     L10N_GET_DESC,     cmd_get},
  {"load",    L10N_LOAD_DESC,    cmd_load},
  {"block",   L10N_BLOCK_DESC,   cmd_block},
  {"ephemeral", L10N_EPHEMERAL_DESC, cmd_ephemeral},
  {"pin",     L10N_PIN_DESC,     cmd_pin},
  {"unpin",   L10N_UNPIN_DESC,   cmd_unpin},
  {"peer",    L10N_PEER_DESC,    cmd_peer},
  {"config",  L10N_CONFIG_DESC,  cmd_config},
  {"cache",   L10N_CACHE_DESC,   cmd_cache},
  {"friend",  L10N_FRIEND_DESC,  cmd_friend},
  {"health",  L10N_HEALTH_DESC,  cmd_health},
  {"status",  L10N_STATUS_DESC,  cmd_status},
  {"version", L10N_VERSION_DESC, cmd_version},
  {"bootstrap", L10N_BOOTSTRAP_DESC, cmd_bootstrap},
  {"help",    L10N_HELP_DESC,    NULL},
  {NULL, NULL, NULL}
};

/* The daemon's local socket path. Lives here (not in main.c) so commands
   that self-connect — cache size's unreachable fallback path — still
   honor --socket. */
static const char* g_socket_path = OFFS_LOCAL_SOCKET_PATH;

const char* cli_socket_path(void) {
  return g_socket_path;
}

void cli_set_socket_path(const char* path) {
  if (path != NULL) g_socket_path = path;
}

const cli_command_t* cli_command_table(void) {
  return g_commands;
}

const char* cli_detect_lang(void) {
  const char* env_lang = getenv("OFFS_LANG");
  if (env_lang != NULL) {
    return env_lang;
  }
  const char* sys_lang = getenv("LANG");
  if (sys_lang != NULL) {
    static char lang_buf[8];
    strncpy(lang_buf, sys_lang, sizeof(lang_buf) - 1);
    lang_buf[sizeof(lang_buf) - 1] = '\0';
    char* dot = strchr(lang_buf, '.');
    if (dot != NULL) *dot = '\0';
    return lang_buf;
  }
  return "en";
}

void cli_print_help(const char* command_name) {
  if (command_name != NULL) {
    for (int i = 0; g_commands[i].name != NULL; i++) {
      if (strcmp(g_commands[i].name, command_name) == 0) {
        printf("%s - %s\n", g_commands[i].name, g_commands[i].description);
        printf("  %s %s --help\n", L10N_USAGE, g_commands[i].name);
        return;
      }
    }
    printf("%s '%s'\n", L10N_UNKNOWN_COMMAND, command_name);
    return;
  }

  printf("OFFS - %s\n", L10N_CLI_DESCRIPTION);
  printf("\n%s\n", L10N_COMMANDS);
  for (int i = 0; g_commands[i].name != NULL; i++) {
    printf("  %-12s %s\n", g_commands[i].name, g_commands[i].description);
  }
}

/* Print an error frame if the response is one. Returns 1 if it was an error
   frame (already printed), 0 otherwise. */
int cli_print_error_if(cbor_item_t* response) {
  if (client_api_wire_get_type(response) != CLIENT_API_ERROR) return 0;
  client_api_error_t err;
  memset(&err, 0, sizeof(err));
  if (client_api_error_decode(response, &err) == 0) {
    fprintf(stderr, "%s: %s\n", L10N_ERROR, err.message);
    client_api_error_destroy(&err);
  }
  return 1;
}

/* Replace both separators with the platform's, collapse duplicates and
   trailing separators. Returns 0 when the path does not fit out_size. */
static int _normalize_path(const char* src, char* out, size_t out_size) {
#ifdef _WIN32
  const char sep = '\\';
#else
  const char sep = '/';
#endif
  size_t o = 0;
  for (size_t i = 0; src[i] != '\0'; i++) {
    char c = src[i];
    if (c == '/' || c == '\\') {
      if (o > 0 && out[o - 1] == sep) continue;  /* collapse duplicates */
      c = sep;
    }
    if (o + 1 >= out_size) return 0;
    out[o++] = c;
  }
  while (o > 1 && out[o - 1] == sep) o--;  /* strip trailing (keep "C:\") */
  out[o] = '\0';
  return 1;
}

/* --- cache paths: 1 = same directory, 2 = one inside the other, 0 = disjoint --- */

int cli_cache_paths_conflict(const char* a, const char* b) {
#ifdef _WIN32
  const char sep = '\\';
#else
  const char sep = '/';
#endif
  char na[1024], nb[1024];
  if (!_normalize_path(a, na, sizeof(na)) || !_normalize_path(b, nb, sizeof(nb))) {
    return 2;  /* unrepresentable path — refuse rather than guess */
  }
  size_t i = 0;
  while (na[i] != '\0' && nb[i] != '\0') {
#ifdef _WIN32
    if (tolower((unsigned char)na[i]) != tolower((unsigned char)nb[i])) return 0;
#else
    if (na[i] != nb[i]) return 0;
#endif
    i++;
  }
  if (na[i] == '\0' && nb[i] == '\0') return 1;  /* identical */
  /* One is a prefix of the other: a conflict only when the longer path's
     next character is a separator ("C:\offs" vs "C:\offs\sub" yes,
     "C:\offs" vs "C:\offs2" no). */
  const char* rest = (na[i] != '\0') ? na + i : nb + i;
  return rest[0] == sep ? 2 : 0;
}

/* Read the daemon config file's [daemon].config-dir and [cache].dir. */
int cli_cache_read_config_file_dirs(const char* config_path,
                                    char* config_dir_out, size_t config_dir_size,
                                    char* cache_dir_out, size_t cache_dir_size) {
  if (config_dir_out && config_dir_size) config_dir_out[0] = '\0';
  if (cache_dir_out && cache_dir_size) cache_dir_out[0] = '\0';

  FILE* file = fopen(config_path, "rb");
  if (file == NULL) return -1;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size < 0) { fclose(file); return -1; }
  char* buffer = (char*)malloc((size_t)size + 1);
  if (buffer == NULL) { fclose(file); return -1; }
  size_t read_len = fread(buffer, 1, (size_t)size, file);
  fclose(file);
  buffer[read_len] = '\0';

  cJSON* root = cJSON_Parse(buffer);
  free(buffer);
  if (root == NULL) return -1;

  cJSON* daemon = cJSON_GetObjectItem(root, "daemon");
  if (daemon != NULL) {
    cJSON* value = cJSON_GetObjectItem(daemon, "config-dir");
    if (cJSON_IsString(value) && config_dir_out && config_dir_size) {
      snprintf(config_dir_out, config_dir_size, "%s", value->valuestring);
    }
  }
  cJSON* cache = cJSON_GetObjectItem(root, "cache");
  if (cache != NULL) {
    cJSON* value = cJSON_GetObjectItem(cache, "dir");
    if (cJSON_IsString(value) && cache_dir_out && cache_dir_size) {
      snprintf(cache_dir_out, cache_dir_size, "%s", value->valuestring);
    }
  }
  cJSON_Delete(root);
  return 0;
}

/* Record the first failing path. A deeper recursion level reports first, so
   the guard keeps the leaf-most (most precise) report. */
static void _report_failed_path(char* failed_path, size_t failed_len,
                                const char* path) {
  if (failed_path != NULL && failed_len > 0 && failed_path[0] == '\0') {
    snprintf(failed_path, failed_len, "%s", path);
  }
}

/* Recursive core of cli_cache_move_tree. Returns 0 on success, -1 on the
   first failure. src_child lives at function scope so the failure report
   can name the entry that failed. */
static int _move_tree(const char* src, const char* dst, int verify_mode,
                      char* failed_path, size_t failed_len) {
  char src_child[1024], dst_child[1024];
  int failed = 0;
#ifdef _WIN32
  char pattern[1024];
  WIN32_FIND_DATAA fd;
  HANDLE handle = INVALID_HANDLE_VALUE;
  if (snprintf(pattern, sizeof(pattern), "%s\\*", src) >= (int)sizeof(pattern) ||
      (handle = FindFirstFileA(pattern, &fd)) == INVALID_HANDLE_VALUE) {
    failed = 1;
    snprintf(src_child, sizeof(src_child), "%s", src);
  } else {
    do {
      const char* name = fd.cFileName;
      if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
      if (snprintf(src_child, sizeof(src_child), "%s\\%s", src, name) >= (int)sizeof(src_child) ||
          snprintf(dst_child, sizeof(dst_child), "%s\\%s", dst, name) >= (int)sizeof(dst_child)) {
        failed = 1;
        break;
      }
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (mkdir_p(dst_child) != 0) {
          failed = 1;
          _report_failed_path(failed_path, failed_len, dst_child);
          break;
        }
        if (_move_tree(src_child, dst_child, verify_mode, failed_path, failed_len) != 0) {
          failed = 1;  /* the deeper level already reported its failing path */
          break;
        }
        /* The recursion moved the child's contents out; remove its shell. */
        if (rm_rf(src_child) != 0) {
          failed = 1;
          _report_failed_path(failed_path, failed_len, src_child);
          break;
        }
      } else if (file_move_verified(src_child, dst_child, verify_mode) != 0) {
        failed = 1;
        _report_failed_path(failed_path, failed_len, src_child);
        break;
      }
    } while (FindNextFileA(handle, &fd));
    /* FindNextFileA returning FALSE for a reason other than "no more
       files" is an enumeration failure mid-list. */
    if (!failed && GetLastError() != ERROR_NO_MORE_FILES) failed = 1;
  }
  if (handle != INVALID_HANDLE_VALUE) FindClose(handle);
#else
  DIR* dir = opendir(src);
  if (dir == NULL) {
    failed = 1;
    snprintf(src_child, sizeof(src_child), "%s", src);
  } else {
    struct dirent* entry;
    while (!failed && (entry = readdir(dir)) != NULL) {
      const char* name = entry->d_name;
      if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
      if (snprintf(src_child, sizeof(src_child), "%s/%s", src, name) >= (int)sizeof(src_child) ||
          snprintf(dst_child, sizeof(dst_child), "%s/%s", dst, name) >= (int)sizeof(dst_child)) {
        failed = 1;
        break;
      }
      struct stat child_st;
      if (stat(src_child, &child_st) != 0) {
        failed = 1;
        _report_failed_path(failed_path, failed_len, src_child);
        break;
      }
      if (S_ISDIR(child_st.st_mode)) {
        if (mkdir_p(dst_child) != 0) {
          failed = 1;
          _report_failed_path(failed_path, failed_len, dst_child);
          break;
        }
        if (_move_tree(src_child, dst_child, verify_mode, failed_path, failed_len) != 0) {
          failed = 1;  /* the deeper level already reported its failing path */
          break;
        }
        if (rm_rf(src_child) != 0) {
          failed = 1;
          _report_failed_path(failed_path, failed_len, src_child);
          break;
        }
      } else if (file_move_verified(src_child, dst_child, verify_mode) != 0) {
        failed = 1;
        _report_failed_path(failed_path, failed_len, src_child);
        break;
      }
    }
    closedir(dir);
  }
#endif

  if (failed) {
    _report_failed_path(failed_path, failed_len, src_child);
    return -1;
  }
  return 0;
}

int cli_cache_move_tree(const char* src, const char* dst, int verify_mode,
                        char* failed_path, size_t failed_len) {
  if (mkdir_p((char*)dst) != 0) {
    _report_failed_path(failed_path, failed_len, dst);
    return -1;
  }
  return _move_tree(src, dst, verify_mode, failed_path, failed_len);
}

/* Resolve the daemon's config directory. Precedence: flag (--config-dir) >
   [daemon].config-dir in the daemon config file (offs_default_config_path_get,
   honoring $OFFS_CONFIG) > platform default. Returns 0 with out filled,
   -1 when nothing resolves. */
int cli_cache_resolve_config_dir(const char* flag, char* out, size_t out_size) {
  if (flag != NULL && *flag != '\0') {
    snprintf(out, out_size, "%s", flag);
    return 0;
  }

  char config_path[1024], file_cfg_dir[1024], file_cache_dir[1024];
  if (offs_default_config_path_get(config_path, sizeof(config_path)) == 0 &&
      cli_cache_read_config_file_dirs(config_path, file_cfg_dir, sizeof(file_cfg_dir),
                                      file_cache_dir, sizeof(file_cache_dir)) == 0 &&
      file_cfg_dir[0] != '\0') {
    snprintf(out, out_size, "%s", file_cfg_dir);
    return 0;
  }

  offs_default_dirs_t dirs;
  if (offs_default_dirs_get(&dirs) == 0) {
    snprintf(out, out_size, "%s", dirs.config_dir);
    return 0;
  }
  return -1;
}

int cli_cache_resolve_current(const char* from_flag, const char* config_dir,
                              char* out, size_t out_size) {
  if (from_flag != NULL && *from_flag != '\0') {
    snprintf(out, out_size, "%s", from_flag);
    return 0;
  }

  /* [cache].dir from the daemon config file (honors $OFFS_CONFIG). */
  char config_path[1024];
  if (offs_default_config_path_get(config_path, sizeof(config_path)) == 0) {
    char file_cfg_dir[1024], file_cache_dir[1024];
    if (cli_cache_read_config_file_dirs(config_path, file_cfg_dir, sizeof(file_cfg_dir),
                                        file_cache_dir, sizeof(file_cache_dir)) == 0 &&
        file_cache_dir[0] != '\0') {
      snprintf(out, out_size, "%s", file_cache_dir);
      return 0;
    }
  }

  /* pending cache_dir from {config_dir}/pending_config.json. */
  const char* resolved_cfg_dir = config_dir;
  char cfg_dir_buf[1024];
  if (resolved_cfg_dir == NULL || *resolved_cfg_dir == '\0') {
    if (cli_cache_resolve_config_dir(NULL, cfg_dir_buf, sizeof(cfg_dir_buf)) != 0) {
      resolved_cfg_dir = NULL;
    } else {
      resolved_cfg_dir = cfg_dir_buf;
    }
  }
  if (resolved_cfg_dir != NULL) {
    config_t* pending = config_pending_load(resolved_cfg_dir);
    if (pending != NULL) {
      if (pending->cache_dir != NULL) {
        snprintf(out, out_size, "%s", pending->cache_dir);
        config_free(pending);
        return 0;
      }
      config_free(pending);
    }
  }

  /* Platform default. */
  offs_default_dirs_t dirs;
  if (offs_default_dirs_get(&dirs) == 0) {
    snprintf(out, out_size, "%s", dirs.cache_dir);
    return 0;
  }
  return -1;
}
