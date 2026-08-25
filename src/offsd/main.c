//
// offsd — OFF System daemon binary
//
// A productionized server built on liboffs that supports:
// - Config file parsing (JSON via cJSON)
// - Daemonization (double-fork unless --foreground)
// - PID file
// - Signal handlers for graceful shutdown (SIGINT/SIGTERM)
// - Auto-detect worker count from CPU cores
// - Cross-platform worker detection
// - Clean shutdown sequence in reverse order

#include "ClientAPI/HTTP/http_server.h"
#include "ClientAPI/HTTP/off_routes.h"
#include "ClientAPI/HTTP/block_routes.h"
#include "ClientAPI/HTTP/cors.h"
#include "ClientAPI/HTTP/http_request.h"
#include "ClientAPI/HTTP/http_response.h"
#include "ClientAPI/HTTP/http_headers.h"
#include "ClientAPI/Unix/unix_transport.h"
#include "ClientAPI/WS/ws_transport.h"
#include "ClientAPI/WT/wt_transport.h"
#include "ClientAPI/WT/webtransport_h3.h"
#include "ClientAPI/HTTP/health_routes.h"
#include "ClientAPI/health_handler.h"
#include "ClientAPI/HTTP/peer_routes.h"
#include "ClientAPI/HTTP/config_routes.h"
#include "Node/node.h"
#include "Network/authority.h"
#include "Network/network.h"
#include "Network/peer_verify.h"
#include "OFFStreams/tuple_cache.h"
#include "BlockCache/block_cache.h"
#include "OFFStreams/ofd_cache.h"
#include "Scheduler/scheduler.h"
#include "Timer/timer_actor.h"
#include "Configuration/config.h"
#include "Configuration/config_pending.h"
#include "Platform/platform.h"
#include "Platform/platform_random.h"
#include "Update/update_actor.h"
#include "Update/update_check.h"
#include "Version/version.h"
#include "ClientAPI/update_status_handler.h"
#include "Util/allocator.h"
#include "Util/log.h"
#include "Util/mkdir_p.h"
#include "Util/path_join.h"
#include "Util/bcrypt.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#endif

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Signal handling — store node pointer so the handler can set running=0
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static offs_node_t* g_node = NULL;
static update_actor_t* g_update_actor = NULL;

/* Set by the config reload handler (via _request_restart) to ask the main loop
   to perform an in-process restart. The reload RPC runs on a scheduler-pool
   worker, where offs_node_restart would self-deadlock (offs_node_stop waits for
   / joins that same shared pool) and destroy the pool the transport still uses;
   instead the worker just sets this flag and the main thread runs _shutdown +
   _startup with the pending config applied. */
static ATOMIC(uint8_t) g_restart_requested = 0;

static void _signal_handler(int sig) {
#ifndef _WIN32
  if (sig == SIGHUP && g_update_actor != NULL) {
    update_actor_check_now(g_update_actor);
    return;
  }
#else
  (void)sig;
#endif
  if (g_node != NULL) {
    ATOMIC_STORE(&g_node->running, 0);
  }
}

/* Reload trigger wired into the unix/HTTP config handlers. Runs on a pool
   worker thread; must not call offs_node_restart (self-deadlock + shared-pool
   destruction). Just sets the flag — the main loop does the restart. */
static void _request_restart(void* user_data) {
  (void)user_data;
  ATOMIC_STORE(&g_restart_requested, 1);
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Soft auth middleware — sets is_authenticated=1 on valid Bearer tokens but
 * does NOT reject requests without one. This lets /health and /offsystem stay
 * open (so the demo can upload without a key) while /peer/* routes — which
 * check is_authenticated themselves via _check_auth in peer_routes.c — can
 * require the key. The hard auth_middleware in auth_middleware.c rejects
 * unauthenticated requests entirely, which would break the demo.
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

typedef struct {
  char* bcrypt_hash;  /* $2b$... hash copied from config.api_key_hash */
} soft_auth_ctx_t;

static int _soft_auth_handler(http_request_t* request, http_response_t* response,
                              void* user_data) {
  (void)response;
  soft_auth_ctx_t* ctx = (soft_auth_ctx_t*)user_data;
  if (ctx == NULL || ctx->bcrypt_hash == NULL) return 0;

  const char* auth_header = http_headers_get(&request->headers, "Authorization");
  if (auth_header == NULL) return 0;
  if (strncmp(auth_header, "Bearer ", 7) != 0) return 0;

  const char* token = auth_header + 7;
  if (*token == '\0') return 0;

  if (bcrypt_check(token, ctx->bcrypt_hash) == 0) {
    request->is_authenticated = 1;
  }
  return 0;
}

static void _soft_auth_ctx_destroy(soft_auth_ctx_t* ctx) {
  if (ctx == NULL) return;
  if (ctx->bcrypt_hash != NULL) free(ctx->bcrypt_hash);
  free(ctx);
}

/* Generate a random 32-byte hex API key (64 chars + NUL). Returns a malloc'd
   string the caller owns, or NULL on failure. */
static char* _generate_random_api_key(void) {
  uint8_t bytes[32];
  if (platform_random_bytes(bytes, sizeof(bytes)) != 0) return NULL;
  char* key = (char*)malloc(sizeof(bytes) * 2 + 1);
  if (key == NULL) return NULL;
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(bytes); i++) {
    key[i * 2]     = hex[bytes[i] >> 4];
    key[i * 2 + 1] = hex[bytes[i] & 0x0f];
  }
  key[sizeof(bytes) * 2] = '\0';
  return key;
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * CLI argument structure — holds everything parsed from flags + config file
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

typedef struct {
  const char* config_path;
  const char* host;
  uint16_t    port;
  uint16_t    quic_port;
  const char* unix_path;
  const char* cache_dir;
  const char* data_dir;
  const char* pid_file;
  int         worker_count;
  int         foreground;
  const char* log_file;
  const char* log_level_str;
  int         log_structured;
  const char* metrics_server_url;
  const char* ca_cert_path;
  const char* node_cert_path;
  const char* node_key_path;
  const char* relay_url;
  size_t      max_capacity_bytes;
  const char* api_key;
  uint16_t    ws_port;
  uint16_t    wt_port;
  uint16_t    wt_h3_port;
  const char* ws_cert_path;
  const char* ws_key_path;
  const char* wt_cert_path;
  const char* wt_key_path;
  uint8_t     allow_secure;
} offsd_args_t;

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Usage text
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static void _print_usage(const char* program) {
  fprintf(stderr, "Usage: %s [options]\n", program);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  --config <path>      Config file path (JSON)\n");
  fprintf(stderr, "  --host <addr>        Bind address (default: 0.0.0.0)\n");
  fprintf(stderr, "  --port <port>        HTTP port, 0 to disable (default: 23402)\n");
  fprintf(stderr, "  --quic-port <port>   QUIC/P2P listener port, 0 to disable (default: 23401)\n");
  fprintf(stderr, "  --unix <path>        Unix socket path\n");
  fprintf(stderr, "  --cache-dir <dir>    Block cache directory\n");
  fprintf(stderr, "  --data-dir <dir>     Persistent data directory\n");
  fprintf(stderr, "  --pid-file <path>    PID file path\n");
  fprintf(stderr, "  --workers <n>        Worker count, 0=auto (default: 0)\n");
  fprintf(stderr, "  --foreground         Run in foreground (do not daemonize)\n");
  fprintf(stderr, "  --log-file <path>    Redirect daemonized stdout/stderr to this file\n");
  fprintf(stderr, "  --log-level <lvl>    Log level: trace, debug, info, warn, error, fatal (default: info)\n");
  fprintf(stderr, "  --log-structured     Enable key=value structured log output\n");
  fprintf(stderr, "  --metrics-server <url>  Metrics server URL for topology reports\n");
  fprintf(stderr, "  --ca-cert <path>      CA certificate PEM path\n");
  fprintf(stderr, "  --node-cert <path>    Node certificate PEM path\n");
  fprintf(stderr, "  --node-key <path>     Node private key PEM path\n");
  fprintf(stderr, "  --relay-url <url>     Relay server URL (host:port or offs://host:port)\n");
  fprintf(stderr, "  --max-capacity-bytes <n>  Block cache capacity in bytes (default: 5368709120 = 5 GiB)\n");
  fprintf(stderr, "  --api-key <key>       API key for /peer/* routes. If omitted, a random key\n");
  fprintf(stderr, "                       is generated and printed to stdout on startup.\n");
  fprintf(stderr, "  --ws-port <port>      WebSocket port, 0 to disable (default: 0)\n");
  fprintf(stderr, "  --wt-port <port>      WebTransport (custom QUIC) port, 0 to disable (default: 0)\n");
  fprintf(stderr, "  --wt-h3-port <port>  HTTP/3 WebTransport port, 0 to disable (default: 0)\n");
  fprintf(stderr, "  --ws-cert <path>      WebSocket TLS certificate PEM path\n");
  fprintf(stderr, "  --ws-key <path>       WebSocket TLS private key PEM path\n");
  fprintf(stderr, "  --wt-cert <path>      WebTransport TLS certificate PEM path\n");
  fprintf(stderr, "  --wt-key <path>       WebTransport TLS private key PEM path\n");
  fprintf(stderr, "  --allow-secure        Require CA validation for TLS transports\n");
  fprintf(stderr, "  --help               Show this help\n");
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Cross-platform worker count detection
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static int _get_worker_count(void) {
#ifdef _WIN32
  SYSTEM_INFO sysinfo;
  GetSystemInfo(&sysinfo);
  return sysinfo.dwNumberOfProcessors > 0 ? (int)sysinfo.dwNumberOfProcessors : 1;
#else
  long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  if (nprocs < 1) return 1;
  return (int)nprocs;
#endif
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * JSON config file parsing
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static int _parse_config_file(const char* path, offsd_args_t* args) {
  FILE* file = fopen(path, "r");
  if (file == NULL) {
    fprintf(stderr, "Failed to open config file: %s\n", path);
    return -1;
  }

  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  char* buffer = (char*)get_memory((size_t)file_size + 1);

  size_t bytes_read = fread(buffer, 1, (size_t)file_size, file);
  fclose(file);
  buffer[bytes_read] = '\0';

  cJSON* root = cJSON_Parse(buffer);
  free(buffer);

  if (root == NULL) {
    fprintf(stderr, "Failed to parse config file: %s\n",
            cJSON_GetErrorPtr() != NULL ? cJSON_GetErrorPtr() : "unknown error");
    return -1;
  }

  /* [daemon] section */
  cJSON* daemon = cJSON_GetObjectItem(root, "daemon");
  if (daemon != NULL) {
    cJSON* data_dir = cJSON_GetObjectItem(daemon, "data-dir");
    if (cJSON_IsString(data_dir) && args->data_dir == NULL) {
      args->data_dir = strdup(data_dir->valuestring);
      if (args->data_dir == NULL) { cJSON_Delete(root); return -1; }
    }
    cJSON* pid_file = cJSON_GetObjectItem(daemon, "pid-file");
    if (cJSON_IsString(pid_file) && args->pid_file == NULL) {
      args->pid_file = strdup(pid_file->valuestring);
      if (args->pid_file == NULL) { cJSON_Delete(root); return -1; }
    }
  }

  /* [network] section */
  cJSON* network = cJSON_GetObjectItem(root, "network");
  if (network != NULL) {
    cJSON* host = cJSON_GetObjectItem(network, "host");
    if (cJSON_IsString(host) && args->host == NULL) {
      args->host = strdup(host->valuestring);
      if (args->host == NULL) { cJSON_Delete(root); return -1; }
    }
    cJSON* port = cJSON_GetObjectItem(network, "port");
    if (cJSON_IsNumber(port) && args->port == 23402) {
      args->port = (uint16_t)port->valueint;
    }
    cJSON* relay_url = cJSON_GetObjectItem(network, "relay-url");
    if (cJSON_IsString(relay_url) && args->relay_url == NULL) {
      args->relay_url = strdup(relay_url->valuestring);
      if (args->relay_url == NULL) { cJSON_Delete(root); return -1; }
    }
    cJSON* max_cap = cJSON_GetObjectItem(network, "max-capacity-bytes");
    if (cJSON_IsNumber(max_cap) && args->max_capacity_bytes == 0) {
      args->max_capacity_bytes = (size_t)max_cap->valuedouble;
    }
  }

  /* [auth] section — explicit API key for /peer/* routes. If absent, offsd
     generates a random key on startup and prints it to stdout. */
  cJSON* auth_section = cJSON_GetObjectItem(root, "auth");
  if (auth_section != NULL) {
    cJSON* api_key = cJSON_GetObjectItem(auth_section, "api-key");
    if (cJSON_IsString(api_key) && args->api_key == NULL) {
      args->api_key = strdup(api_key->valuestring);
      if (args->api_key == NULL) { cJSON_Delete(root); return -1; }
    }
  }

  /* [unix] section */
  cJSON* unix_section = cJSON_GetObjectItem(root, "unix");
  if (unix_section != NULL) {
    cJSON* socket_path = cJSON_GetObjectItem(unix_section, "socket-path");
    if (cJSON_IsString(socket_path) && args->unix_path == NULL) {
      args->unix_path = strdup(socket_path->valuestring);
      if (args->unix_path == NULL) { cJSON_Delete(root); return -1; }
    }
  }

  /* [cache] section */
  cJSON* cache = cJSON_GetObjectItem(root, "cache");
  if (cache != NULL) {
    cJSON* cache_dir = cJSON_GetObjectItem(cache, "dir");
    if (cJSON_IsString(cache_dir) && args->cache_dir == NULL) {
      args->cache_dir = strdup(cache_dir->valuestring);
      if (args->cache_dir == NULL) { cJSON_Delete(root); return -1; }
    }
  }

  /* [workers] section */
  cJSON* workers = cJSON_GetObjectItem(root, "workers");
  if (workers != NULL) {
    cJSON* count = cJSON_GetObjectItem(workers, "count");
    if (cJSON_IsNumber(count) && args->worker_count == 0) {
      args->worker_count = count->valueint;
    }
  }

  /* [metrics] section */
  cJSON* metrics_section = cJSON_GetObjectItem(root, "metrics");
  if (metrics_section != NULL) {
    cJSON* server_url = cJSON_GetObjectItem(metrics_section, "server-url");
    if (cJSON_IsString(server_url) && args->metrics_server_url == NULL) {
      args->metrics_server_url = strdup(server_url->valuestring);
      if (args->metrics_server_url == NULL) { cJSON_Delete(root); return -1; }
    }
  }

  /* [tls] section */
  cJSON* tls_section = cJSON_GetObjectItem(root, "tls");
  if (tls_section != NULL) {
    cJSON* ca_cert = cJSON_GetObjectItem(tls_section, "ca-cert");
    if (cJSON_IsString(ca_cert) && args->ca_cert_path == NULL) {
      args->ca_cert_path = strdup(ca_cert->valuestring);
      if (args->ca_cert_path == NULL) { cJSON_Delete(root); return -1; }
    }
    cJSON* node_cert = cJSON_GetObjectItem(tls_section, "node-cert");
    if (cJSON_IsString(node_cert) && args->node_cert_path == NULL) {
      args->node_cert_path = strdup(node_cert->valuestring);
      if (args->node_cert_path == NULL) { cJSON_Delete(root); return -1; }
    }
    cJSON* node_key = cJSON_GetObjectItem(tls_section, "node-key");
    if (cJSON_IsString(node_key) && args->node_key_path == NULL) {
      args->node_key_path = strdup(node_key->valuestring);
      if (args->node_key_path == NULL) { cJSON_Delete(root); return -1; }
    }
  }

  /* [websocket] section */
  cJSON* ws_section = cJSON_GetObjectItem(root, "websocket");
  if (ws_section != NULL) {
    cJSON* ws_port = cJSON_GetObjectItem(ws_section, "port");
    if (cJSON_IsNumber(ws_port) && args->ws_port == 0) {
      args->ws_port = (uint16_t)ws_port->valueint;
    }
    cJSON* ws_cert = cJSON_GetObjectItem(ws_section, "cert");
    if (cJSON_IsString(ws_cert) && args->ws_cert_path == NULL) {
      args->ws_cert_path = strdup(ws_cert->valuestring);
      if (args->ws_cert_path == NULL) { cJSON_Delete(root); return -1; }
    }
    cJSON* ws_key = cJSON_GetObjectItem(ws_section, "key");
    if (cJSON_IsString(ws_key) && args->ws_key_path == NULL) {
      args->ws_key_path = strdup(ws_key->valuestring);
      if (args->ws_key_path == NULL) { cJSON_Delete(root); return -1; }
    }
  }

  /* [webtransport] section */
  cJSON* wt_section = cJSON_GetObjectItem(root, "webtransport");
  if (wt_section != NULL) {
    cJSON* wt_port = cJSON_GetObjectItem(wt_section, "port");
    if (cJSON_IsNumber(wt_port) && args->wt_port == 0) {
      args->wt_port = (uint16_t)wt_port->valueint;
    }
    cJSON* wt_cert = cJSON_GetObjectItem(wt_section, "cert");
    if (cJSON_IsString(wt_cert) && args->wt_cert_path == NULL) {
      args->wt_cert_path = strdup(wt_cert->valuestring);
      if (args->wt_cert_path == NULL) { cJSON_Delete(root); return -1; }
    }
    cJSON* wt_key = cJSON_GetObjectItem(wt_section, "key");
    if (cJSON_IsString(wt_key) && args->wt_key_path == NULL) {
      args->wt_key_path = strdup(wt_key->valuestring);
      if (args->wt_key_path == NULL) { cJSON_Delete(root); return -1; }
    }
  }

  /* [security] section */
  cJSON* security_section = cJSON_GetObjectItem(root, "security");
  if (security_section != NULL) {
    cJSON* allow_secure = cJSON_GetObjectItem(security_section, "allow-secure");
    if (cJSON_IsBool(allow_secure) && !args->allow_secure) {
      args->allow_secure = cJSON_IsTrue(allow_secure) ? 1 : 0;
    }
  }

  cJSON_Delete(root);
  return 0;
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * CLI argument parsing
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

/* Replace *field with a strdup'd copy of value, freeing the previous content.
 * Returns 0 on success, -1 on allocation failure (field left NULL). */
static int _arg_string_set(char** field, const char* value) {
  char* copy = strdup(value);
  if (copy == NULL) {
    free(*field);
    *field = NULL;
    return -1;
  }
  free(*field);
  *field = copy;
  return 0;
}

static int _parse_args(int argc, char** argv, offsd_args_t* args) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      args->config_path = argv[++i];
    } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->host, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      args->port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--quic-port") == 0 && i + 1 < argc) {
      args->quic_port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--unix") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->unix_path, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->cache_dir, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->data_dir, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--pid-file") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->pid_file, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
      args->worker_count = atoi(argv[++i]);
      if (args->worker_count < 0) args->worker_count = 0;
    } else if (strcmp(argv[i], "--foreground") == 0) {
      args->foreground = 1;
    } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
      args->log_file = argv[++i];
    } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
      args->log_level_str = argv[++i];
    } else if (strcmp(argv[i], "--log-structured") == 0) {
      args->log_structured = 1;
    } else if (strcmp(argv[i], "--metrics-server") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->metrics_server_url, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--ca-cert") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->ca_cert_path, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--node-cert") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->node_cert_path, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--node-key") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->node_key_path, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--relay-url") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->relay_url, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--max-capacity-bytes") == 0 && i + 1 < argc) {
      args->max_capacity_bytes = (size_t)strtoull(argv[++i], NULL, 10);
    } else if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->api_key, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--ws-port") == 0 && i + 1 < argc) {
      args->ws_port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--wt-port") == 0 && i + 1 < argc) {
      args->wt_port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--wt-h3-port") == 0 && i + 1 < argc) {
      args->wt_h3_port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--ws-cert") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->ws_cert_path, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--ws-key") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->ws_key_path, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--wt-cert") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->wt_cert_path, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--wt-key") == 0 && i + 1 < argc) {
      if (_arg_string_set(&args->wt_key_path, argv[++i]) != 0) return -1;
    } else if (strcmp(argv[i], "--allow-secure") == 0) {
      args->allow_secure = 1;
    } else if (strcmp(argv[i], "--help") == 0) {
      _print_usage(argv[0]);
      return 1;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      _print_usage(argv[0]);
      return -1;
    }
  }

  if (args->config_path != NULL) {
    if (_parse_config_file(args->config_path, args) != 0) {
      return -1;
    }
  }

  if (args->worker_count == 0) {
    args->worker_count = _get_worker_count();
  }
  if (args->worker_count < 1) {
    args->worker_count = 1;
  }

  return 0;
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * PID file management
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static int _write_pid_file(const char* path) {
  if (path == NULL) return 0;

  FILE* file = fopen(path, "w");
  if (file == NULL) {
    fprintf(stderr, "Failed to open PID file %s: %s\n", path, strerror(errno));
    return -1;
  }

  fprintf(file, "%d\n",
#ifdef _WIN32
          platform_getpid()
#else
          getpid()
#endif
          );
  fclose(file);
  return 0;
}

static void _remove_pid_file(const char* path) {
  if (path != NULL) {
    unlink(path);
  }
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Free args allocated by config file parsing
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static void _free_args(offsd_args_t* args) {
  free((void*)args->data_dir);
  free((void*)args->pid_file);
  free((void*)args->host);
  free((void*)args->unix_path);
  free((void*)args->cache_dir);
  free((void*)args->metrics_server_url);
  free((void*)args->ca_cert_path);
  free((void*)args->node_cert_path);
  free((void*)args->node_key_path);
  free((void*)args->relay_url);
  free((void*)args->api_key);
  free((void*)args->ws_cert_path);
  free((void*)args->ws_key_path);
  free((void*)args->wt_cert_path);
  free((void*)args->wt_key_path);
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Daemonization (double-fork)
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static int _daemonize(const char* log_file) {
#ifdef _WIN32
  /* Windows services are managed by the SCM (service_windows.c); the daemon
   * binary runs in the foreground under the service host. No fork/setsid. */
  return 0;
#else
  pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "First fork failed: %s\n", strerror(errno));
    return -1;
  }
  if (pid > 0) {
    _exit(0);
  }

  if (setsid() < 0) {
    fprintf(stderr, "setsid failed: %s\n", strerror(errno));
    return -1;
  }

  pid = fork();
  if (pid < 0) {
    fprintf(stderr, "Second fork failed: %s\n", strerror(errno));
    return -1;
  }
  if (pid > 0) {
    _exit(0);
  }

  umask(0);

  if (chdir("/") < 0) {
    fprintf(stderr, "chdir(/) failed: %s\n", strerror(errno));
    return -1;
  }

  /* Redirect stdin to /dev/null always */
  int dev_null = open("/dev/null", O_RDWR);
  if (dev_null < 0) {
    fprintf(stderr, "Failed to open /dev/null: %s\n", strerror(errno));
    return -1;
  }
  dup2(dev_null, STDIN_FILENO);

  /* Redirect stdout + stderr: to log file if given, else /dev/null */
  if (log_file != NULL) {
    int log_fd = open(log_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
      /* Log file failed — fall back to /dev/null so we don't block */
      dup2(dev_null, STDOUT_FILENO);
      dup2(dev_null, STDERR_FILENO);
    } else {
      dup2(log_fd, STDOUT_FILENO);
      dup2(log_fd, STDERR_FILENO);
      close(log_fd);
    }
  } else {
    dup2(dev_null, STDOUT_FILENO);
    dup2(dev_null, STDERR_FILENO);
  }
  close(dev_null);
  if (dev_null > STDERR_FILENO) {
    close(dev_null);
  }

  return 0;
#endif
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Startup: create all components
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

typedef struct {
  scheduler_pool_t* pool;
  timer_actor_t*    timer;
  config_t          config;
  block_cache_t*    block_cache;
  ofd_cache_t*      ofd_cache;
  tuple_cache_t*    tuple_cache;
  http_server_t*    http_server;
  authority_t*      authority;
  network_t*        network;
  offs_node_t       node;
  health_context_t  health_ctx;
  uint8_t           running_val;
  uint8_t           draining_val;
  uint64_t          start_time_ms;
  unix_transport_t* unix_transport;
  ws_transport_t*   ws_transport;
  wt_transport_t*   wt_transport;
  webtransport_h3_t* wt_h3_transport;
  update_actor_t*    update_actor;
  ATOMIC(uint32_t)  open_stream_count;
  update_status_context_t update_status_ctx;
} offsd_server_t;

static void _init_health_context(offsd_server_t* server, block_cache_t* bc) {
  memset(&server->health_ctx, 0, sizeof(server->health_ctx));
  server->health_ctx.block_cache = bc;
  server->health_ctx.start_time_ms = &server->start_time_ms;
  server->health_ctx.running = &server->running_val;
  server->health_ctx.draining = &server->draining_val;
}

static int _startup(offsd_server_t* server, const offsd_args_t* args,
                    config_t* override_config) {
  memset(server, 0, sizeof(*server));

  /* Ensure cache and data directories exist before subsystems use them.
   * config_pending_save writes to {data_dir}/pending_config.json; the block
   * cache writes section files under {cache_dir}. Without this, a missing
   * data_dir surfaces as a cryptic "failed to write pending config" on the
   * first config set, and a missing cache_dir fails block_cache_create. */
  if (args->cache_dir != NULL && mkdir_p((char*)args->cache_dir) != 0) {
    fprintf(stderr, "Failed to create cache directory: %s\n", args->cache_dir);
    if (override_config != NULL) {
      config_free_members(override_config);
      free(override_config);
    }
    return -1;
  }
  if (args->data_dir != NULL && mkdir_p((char*)args->data_dir) != 0) {
    fprintf(stderr, "Failed to create data directory: %s\n", args->data_dir);
    if (override_config != NULL) {
      config_free_members(override_config);
      free(override_config);
    }
    return -1;
  }

  /* Thread setup */
  platform_thread_setup_stack();

  /* Scheduler pool */
  server->pool = scheduler_pool_create(args->worker_count);
  if (server->pool == NULL) {
    fprintf(stderr, "Failed to create scheduler pool\n");
    /* override_config is owned by the caller until _startup accepts it below;
       free it on this early-failure path. */
    if (override_config != NULL) {
      config_free_members(override_config);
      free(override_config);
    }
    return -1;
  }
  scheduler_pool_start(server->pool);

  /* Timer actor */
  server->timer = timer_actor_create(server->pool);

  /* Configuration: apply a pending-config override if supplied (reload path),
     otherwise defaults. _startup takes ownership of override_config's members
     (struct copy into the embedded server->config) and frees the shell. */
  server->config = override_config ? *override_config : config_default();
  if (override_config != NULL) {
    free(override_config);
  }

  /* CLI --max-capacity-bytes overrides the config file / pending config so the
   * block cache capacity is taken from the flag when present. */
  if (args->max_capacity_bytes > 0) {
    server->config.max_capacity_bytes = args->max_capacity_bytes;
  }

  /* API key for /peer/* routes. If --api-key was not provided (and no [auth]
     section set it), generate a random one and print it to stdout. Either way,
     hash the plaintext key with bcrypt into config.api_key_hash so the soft
     auth middleware can validate incoming Bearer tokens. */
  char* api_key_plaintext = NULL;
  if (args->api_key != NULL) {
    api_key_plaintext = strdup(args->api_key);
    if (api_key_plaintext == NULL) {
      fprintf(stderr, "Out of memory copying api_key\n");
      timer_actor_destroy(server->timer);
      scheduler_pool_stop(server->pool);
      scheduler_pool_destroy(server->pool);
      return -1;
    }
  } else {
    api_key_plaintext = _generate_random_api_key();
    if (api_key_plaintext == NULL) {
      fprintf(stderr, "Failed to generate random API key\n");
      timer_actor_destroy(server->timer);
      scheduler_pool_stop(server->pool);
      scheduler_pool_destroy(server->pool);
      return -1;
    }
  }
  char bcrypt_hash[64];
  if (bcrypt_generate(api_key_plaintext, 12, bcrypt_hash, sizeof(bcrypt_hash)) != 0) {
    fprintf(stderr, "Failed to hash API key\n");
    free(api_key_plaintext);
    timer_actor_destroy(server->timer);
    scheduler_pool_stop(server->pool);
    scheduler_pool_destroy(server->pool);
    return -1;
  }
  if (server->config.api_key_hash != NULL) {
    free(server->config.api_key_hash);
  }
  server->config.api_key_hash = strdup(bcrypt_hash);
  if (server->config.api_key_hash == NULL) {
    fprintf(stderr, "Out of memory storing api_key_hash\n");
    free(api_key_plaintext);
    timer_actor_destroy(server->timer);
    scheduler_pool_stop(server->pool);
    scheduler_pool_destroy(server->pool);
    return -1;
  }
  if (args->api_key == NULL) {
    printf("Generated API key for /peer/* routes: %s\n", api_key_plaintext);
    printf("Pass this key to clients as: Authorization: Bearer %s\n", api_key_plaintext);
  } else {
    printf("Using configured API key for /peer/* routes\n");
  }
  fflush(stdout);
  /* The plaintext is no longer needed after printing — the soft-auth middleware
     validates incoming Bearer tokens against the bcrypt hash in
     config.api_key_hash. Zero and free it now. */
  memset(api_key_plaintext, 0, strlen(api_key_plaintext));
  free(api_key_plaintext);

  /* Block cache — wire config.max_capacity_bytes so the cache is actually
     bounded. Passing 0 here (the old behavior) left the cache unbounded. */
  server->block_cache = block_cache_create(server->config,
      (char*)args->cache_dir, standard, server->timer,
      server->pool, NULL, server->config.max_capacity_bytes);
  if (server->block_cache == NULL) {
    fprintf(stderr, "Failed to create block cache\n");
    timer_actor_destroy(server->timer);
    scheduler_pool_stop(server->pool);
    scheduler_pool_destroy(server->pool);
    return -1;
  }

  /* OFD cache */
  server->ofd_cache = ofd_cache_create(server->pool, server->block_cache, 300000);

  /* Tuple cache */
  server->tuple_cache = tuple_cache_create(100, server->pool);

  /* HTTP server (skip if port is 0) */
  if (args->port != 0) {
    server->http_server = http_server_create(server->pool, args->host, args->port);
    if (server->http_server == NULL) {
      fprintf(stderr, "Failed to create HTTP server on %s:%u\n",
              args->host, args->port);
      tuple_cache_destroy(server->tuple_cache);
      ofd_cache_destroy(server->ofd_cache);
      block_cache_destroy(server->block_cache);
      timer_actor_destroy(server->timer);
      scheduler_pool_stop(server->pool);
      scheduler_pool_destroy(server->pool);
      return -1;
    }
    /* Wire the HTTP idle/hard timeouts from the config (slowloris defense). */
    http_server_set_timeouts(server->http_server,
                             server->config.http_idle_timeout_ms,
                             server->config.http_hard_timeout_ms);
  }

  /* Start time tracking */
  {
#ifdef _WIN32
    server->start_time_ms = platform_monotonic_ns() / 1000000;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    server->start_time_ms = (uint64_t)now.tv_sec * 1000
                          + (uint64_t)now.tv_nsec / 1000000;
#endif
  }
  server->running_val = 1;
  server->draining_val = 0;

  /* Health context */
  _init_health_context(server, server->block_cache);

  /* Authority */
  server->authority = authority_create(&server->config);
  if (server->authority == NULL) {
    fprintf(stderr, "Failed to create authority\n");
    if (server->http_server != NULL) http_server_destroy(server->http_server);
    tuple_cache_destroy(server->tuple_cache);
    ofd_cache_destroy(server->ofd_cache);
    block_cache_destroy(server->block_cache);
    timer_actor_destroy(server->timer);
    scheduler_pool_stop(server->pool);
    scheduler_pool_destroy(server->pool);
    return -1;
  }

  /* Wire TLS cert paths into authority before init */
  if (args->ca_cert_path != NULL) {
    if (authority_load_ca_cert(server->authority, args->ca_cert_path) != 0) {
      fprintf(stderr, "Failed to load CA certificate: %s\n", args->ca_cert_path);
      authority_destroy(server->authority);
      if (server->http_server != NULL) http_server_destroy(server->http_server);
      tuple_cache_destroy(server->tuple_cache);
      ofd_cache_destroy(server->ofd_cache);
      block_cache_destroy(server->block_cache);
      timer_actor_destroy(server->timer);
      scheduler_pool_stop(server->pool);
      scheduler_pool_destroy(server->pool);
      return -1;
    }
  }
  if (args->node_cert_path != NULL) {
    server->authority->node_cert_path = strdup(args->node_cert_path);
    if (server->authority->node_cert_path == NULL) {
      fprintf(stderr, "Out of memory copying node-cert path\n");
      authority_destroy(server->authority);
      if (server->http_server != NULL) http_server_destroy(server->http_server);
      tuple_cache_destroy(server->tuple_cache);
      ofd_cache_destroy(server->ofd_cache);
      block_cache_destroy(server->block_cache);
      timer_actor_destroy(server->timer);
      scheduler_pool_stop(server->pool);
      scheduler_pool_destroy(server->pool);
      return -1;
    }
  }
  if (args->node_key_path != NULL) {
    server->authority->node_key_path = strdup(args->node_key_path);
    if (server->authority->node_key_path == NULL) {
      fprintf(stderr, "Out of memory copying node-key path\n");
      authority_destroy(server->authority);
      if (server->http_server != NULL) http_server_destroy(server->http_server);
      tuple_cache_destroy(server->tuple_cache);
      ofd_cache_destroy(server->ofd_cache);
      block_cache_destroy(server->block_cache);
      timer_actor_destroy(server->timer);
      scheduler_pool_stop(server->pool);
      scheduler_pool_destroy(server->pool);
      return -1;
    }
  }

  /* Persist peer state (node ID, friends, hebbian weights, ring peers) across
     restarts. The peer store path lives under the daemon's data directory so it
     is backed up alongside pending config and other node-local state. */
  if (args->data_dir != NULL) {
    server->authority->peer_store_path = path_join(args->data_dir, "peer_store.cbor");
  }

  authority_init_local_id(server->authority);

  /* Network */
  server->network = network_create(server->authority, server->block_cache,
                                    server->timer, server->pool,
                                    &server->config);
  if (server->network == NULL) {
    fprintf(stderr, "Failed to create network\n");
    authority_destroy(server->authority);
    if (server->http_server != NULL) http_server_destroy(server->http_server);
    tuple_cache_destroy(server->tuple_cache);
    ofd_cache_destroy(server->ofd_cache);
    block_cache_destroy(server->block_cache);
    timer_actor_destroy(server->timer);
    scheduler_pool_stop(server->pool);
    scheduler_pool_destroy(server->pool);
    return -1;
  }

  /* Node object */
  server->node.config = &server->config;
  server->node.authority = server->authority;
  server->node.network = server->network;
  server->node.block_cache = server->block_cache;
  server->node.http_server = server->http_server;
  server->node.scheduler = server->pool;
  server->node.timer = server->timer;
  ATOMIC_STORE(&server->node.running, 1);
  ATOMIC_STORE(&server->node.draining, 0);
  server->node.start_time_ms = server->start_time_ms;

  /* Route registration (HTTP only if enabled) */
  if (server->http_server != NULL) {
    /* Install soft-auth middleware that sets is_authenticated=1 on valid
       Bearer tokens but does NOT reject unauthenticated requests. This lets
       /health and /offsystem stay open (demo uploads work) while /peer/* routes
       require the key via their own _check_auth. */
    soft_auth_ctx_t* soft_auth = (soft_auth_ctx_t*)malloc(sizeof(soft_auth_ctx_t));
    if (soft_auth != NULL) {
      soft_auth->bcrypt_hash = strdup(server->config.api_key_hash);
      if (soft_auth->bcrypt_hash != NULL) {
        http_server_use(server->http_server, _soft_auth_handler, soft_auth,
                        (void (*)(void*))_soft_auth_ctx_destroy);
      } else {
        free(soft_auth);
      }
    }

    off_routes_register(server->http_server, server->pool,
                        server->block_cache, server->ofd_cache,
                        server->tuple_cache, server->network, NULL, NULL,
                        &server->open_stream_count);
    block_routes_register(server->http_server, server->pool,
                          server->block_cache, NULL, NULL);
    health_routes_register(server->http_server, &server->health_ctx);
    peer_routes_register(server->http_server, &server->node,
                         &server->config, "enabled");
    config_routes_register(server->http_server, &server->node,
                           &server->config, args->data_dir,
                           _request_restart, NULL);
  }

  /* WebSocket transport */
  server->ws_transport = NULL;
  if (args->ws_port != 0) {
    server->ws_transport = ws_transport_create(
        server->pool, server->block_cache, server->ofd_cache,
        server->tuple_cache, args->host, args->ws_port,
        args->ws_cert_path, args->ws_key_path, 0, NULL,
        &server->health_ctx);
    if (server->ws_transport == NULL) {
      fprintf(stderr, "Failed to create WebSocket transport on %s:%u\n",
              args->host, args->ws_port);
      authority_save_peers(server->authority, server->network);
      network_destroy(server->network);
      authority_destroy(server->authority);
      if (server->http_server != NULL) http_server_destroy(server->http_server);
      tuple_cache_destroy(server->tuple_cache);
      ofd_cache_destroy(server->ofd_cache);
      block_cache_destroy(server->block_cache);
      timer_actor_destroy(server->timer);
      scheduler_pool_stop(server->pool);
      scheduler_pool_destroy(server->pool);
      return -1;
    }
  }

  /* WebTransport transport */
  server->wt_transport = NULL;
  if (args->wt_port != 0) {
    server->wt_transport = wt_transport_create(
        server->pool, server->block_cache, server->ofd_cache,
        server->tuple_cache, args->host, args->wt_port,
        args->wt_cert_path, args->wt_key_path, args->ca_cert_path,
        args->allow_secure != 0, 0, NULL, &server->health_ctx);
    if (server->wt_transport == NULL) {
      fprintf(stderr, "Failed to create WebTransport transport on %s:%u\n",
              args->host, args->wt_port);
      if (server->ws_transport != NULL) {
        ws_transport_destroy(server->ws_transport);
      }
      authority_save_peers(server->authority, server->network);
      network_destroy(server->network);
      authority_destroy(server->authority);
      if (server->http_server != NULL) http_server_destroy(server->http_server);
      tuple_cache_destroy(server->tuple_cache);
      ofd_cache_destroy(server->ofd_cache);
      block_cache_destroy(server->block_cache);
      timer_actor_destroy(server->timer);
      scheduler_pool_stop(server->pool);
      scheduler_pool_destroy(server->pool);
      return -1;
    }
  }

  /* HTTP/3 WebTransport transport */
  server->wt_h3_transport = NULL;
  if (args->wt_h3_port != 0) {
    server->wt_h3_transport = webtransport_h3_create(
        server->pool, server->block_cache, server->ofd_cache,
        server->tuple_cache, args->host, args->wt_h3_port,
        args->wt_cert_path, args->wt_key_path, args->ca_cert_path,
        args->allow_secure != 0, NULL, &server->health_ctx);
    if (server->wt_h3_transport == NULL) {
      fprintf(stderr, "Failed to create WebTransport H3 transport on %s:%u\n",
              args->host, args->wt_h3_port);
      if (server->wt_transport != NULL) {
        wt_transport_destroy(server->wt_transport);
      }
      if (server->ws_transport != NULL) {
        ws_transport_destroy(server->ws_transport);
      }
      authority_save_peers(server->authority, server->network);
      network_destroy(server->network);
      authority_destroy(server->authority);
      if (server->http_server != NULL) http_server_destroy(server->http_server);
      tuple_cache_destroy(server->tuple_cache);
      ofd_cache_destroy(server->ofd_cache);
      block_cache_destroy(server->block_cache);
      timer_actor_destroy(server->timer);
      scheduler_pool_stop(server->pool);
      scheduler_pool_destroy(server->pool);
      return -1;
    }
  }

  /* Unix transport */
  server->unix_transport = NULL;
  if (args->unix_path != NULL) {
    server->unix_transport = unix_transport_create(
        server->pool, server->block_cache, server->ofd_cache,
        server->tuple_cache, args->unix_path, NULL,
        &server->health_ctx);
    if (server->unix_transport == NULL) {
      fprintf(stderr, "Failed to create Unix transport on %s\n",
              args->unix_path);
      if (server->wt_transport != NULL) {
        wt_transport_destroy(server->wt_transport);
      }
      if (server->wt_h3_transport != NULL) {
        webtransport_h3_destroy(server->wt_h3_transport);
      }
      if (server->ws_transport != NULL) {
        ws_transport_destroy(server->ws_transport);
      }
      authority_save_peers(server->authority, server->network);
      network_destroy(server->network);
      authority_destroy(server->authority);
      if (server->http_server != NULL) http_server_destroy(server->http_server);
      tuple_cache_destroy(server->tuple_cache);
      ofd_cache_destroy(server->ofd_cache);
      block_cache_destroy(server->block_cache);
      timer_actor_destroy(server->timer);
      scheduler_pool_stop(server->pool);
      scheduler_pool_destroy(server->pool);
      return -1;
    }
    /* Wire config management onto the local socket so `offs config show/set/
       generate-auth/reload` reach the node + pending-config store. node is
       borrowed (owned by server); data_dir is copied by the setter. The restart
       trigger hands `config reload` to the main loop instead of running
       offs_node_restart on this pool worker. */
    unix_transport_set_config_ctx(server->unix_transport, &server->node,
                                  args->data_dir, _request_restart, NULL);
  }

  /* Update actor — auto-update checks.
   * The Update module (fork/execlp-based self-update) is excluded from liboffs
   * on Windows, so the actor is only created on POSIX. On Windows the update
   * status context stays disabled. */
#ifndef _WIN32
  {
    update_check_config_t update_config;
    memset(&update_config, 0, sizeof(update_config));
    snprintf(update_config.github_repo, sizeof(update_config.github_repo),
             "%s", "Prometheus-SCN/OFFS");
    snprintf(update_config.github_api_url, sizeof(update_config.github_api_url),
             "%s", "https://api.github.com");
    update_config.channel = channel_stable;
    update_config.check_interval_hours = 6;

    // Read GITHUB_TOKEN from environment
    const char* token = getenv("GITHUB_TOKEN");
    if (token != NULL) {
      snprintf(update_config.github_token, sizeof(update_config.github_token),
               "%s", token);
    }

    /* Populate status context values not managed by the actor */
    server->update_status_ctx.enabled = 1;
    snprintf(server->update_status_ctx.channel,
             sizeof(server->update_status_ctx.channel), "%s",
             channel_to_string(update_config.channel));
    server->update_status_ctx.check_interval_hours = update_config.check_interval_hours;

    server->update_actor = update_actor_create(
      server->pool, server->timer, &update_config,
      "/var/lib/offs/updates", "/usr/bin", "/var/lib/offs/backup",
      &server->draining_val, &server->open_stream_count,
      &server->update_status_ctx);
    g_update_actor = server->update_actor;

    if (server->unix_transport != NULL) {
      unix_transport_set_update_status_ctx(server->unix_transport,
                                            &server->update_status_ctx);
    }
  }
#endif

  return 0;
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Load pending config as an override for the next _startup (returns a heap
 * config_t* the caller passes to _startup, or NULL if none/invalid). Marks the
 * pending file applied so a subsequent reload re-stages it. Replaces the old
 * _apply_pending_config, which ran offs_node_restart from the main thread after
 * _startup had already created (but not started) the transport — destroying the
 * shared server->pool and dangling transport->pool. Folding the pending config
 * into the initial _startup avoids that.
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static config_t* _load_pending_override(const char* data_dir) {
  if (data_dir == NULL) return NULL;
  if (config_pending_exists(data_dir) != 1) return NULL;
  config_t* cfg = config_pending_load(data_dir);
  if (cfg != NULL) {
    config_pending_mark_applied(data_dir);
  }
  return cfg;
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Start listening
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static void _start_listening(offsd_server_t* server,
                             const offsd_args_t* args) {
  if (server->http_server != NULL) {
    http_server_listen(server->http_server);
  }

  /* Start the QUIC/P2P listener so the node can accept incoming direct
     peer connections (same-LAN fast path, cross-NAT hole-punching).
     Without this, peer_info_from_node has no HOST candidates (no listen
     port) and no incoming connections can be accepted. See audit #18. */
  if (args->quic_port > 0 && server->network != NULL && server->network->quic_listener != NULL) {
    if (quic_listener_start(server->network->quic_listener, args->host, args->quic_port) == 0) {
      printf("Listening on quic://%s:%u\n", args->host, args->quic_port);
    } else {
      fprintf(stderr, "Warning: failed to start QUIC listener on %s:%u\n", args->host, args->quic_port);
    }
  }

  if (server->ws_transport != NULL) {
    ws_transport_start(server->ws_transport);
    printf("Listening on ws://%s:%u\n", args->host, args->ws_port);
    if (args->ws_cert_path != NULL && args->ws_key_path != NULL) {
      printf("Listening on wss://%s:%u\n", args->host, args->ws_port);
    }
  }
  if (server->wt_transport != NULL) {
    wt_transport_start(server->wt_transport);
    printf("Listening on wt://%s:%u\n", args->host, args->wt_port);
    if (args->wt_cert_path != NULL && args->wt_key_path != NULL) {
      printf("Listening on wts://%s:%u\n", args->host, args->wt_port);
    }
  }
  if (server->wt_h3_transport != NULL) {
    webtransport_h3_start(server->wt_h3_transport);
    printf("Listening on wt://%s:%u (HTTP/3)\n", args->host, args->wt_h3_port);
    if (args->wt_cert_path != NULL && args->wt_key_path != NULL) {
      printf("Listening on wts://%s:%u (HTTP/3)\n", args->host, args->wt_h3_port);
    }
  }
  if (server->unix_transport != NULL) {
    unix_transport_start(server->unix_transport);
    printf("Listening on unix://%s\n", args->unix_path);
  }

  /* Connect to the relay server for NAT traversal and server-reflexive address
     discovery. The relay_url is "host:port" (optionally "offs://host:port").
     The relay provides server-reflexive address discovery and forwards opaque
     WIRE_RELAY_SEND envelopes between peers behind NAT. After relay-mediated
     rendezvous, peers attempt UDP hole punching to establish a direct QUIC
     path. See src/Network/relay_client.c. */
  if (args->relay_url != NULL && server->network != NULL) {
    const char* url = args->relay_url;
    if (strncmp(url, "offs://", 7) == 0) url += 7;
    const char* colon = strrchr(url, ':');
    if (colon != NULL) {
      char* host = strndup(url, (size_t)(colon - url));
      if (host != NULL) {
        uint16_t relay_port = (uint16_t)atoi(colon + 1);
        if (relay_port > 0) {
          if (network_connect_relay(server->network, host, relay_port) == 0) {
            printf("Connected to relay %s:%u\n", host, relay_port);
          } else {
            fprintf(stderr, "Warning: failed to connect to relay %s:%u\n",
                    host, relay_port);
          }
        }
        free(host);
      }
    }
  }

  authority_load_peers(server->authority, server->network);
  network_start_connections(server->network);

  if (server->http_server != NULL) {
    printf("Listening on http://%s:%u\n", args->host, args->port);
  }
  printf("Press Ctrl+C to stop\n");
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Graceful shutdown — reverse order
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static void _shutdown(offsd_server_t* server, const char* pid_file) {
  printf("Shutting down...\n");

  /* 1. Stop Unix transport */
  if (server->unix_transport != NULL) {
    unix_transport_stop(server->unix_transport);
  }

  /* 2. Stop WebSocket and WebTransport transports */
  if (server->ws_transport != NULL) {
    ws_transport_stop(server->ws_transport);
  }
  if (server->wt_transport != NULL) {
    wt_transport_stop(server->wt_transport);
  }
  if (server->wt_h3_transport != NULL) {
    webtransport_h3_stop(server->wt_h3_transport);
  }

  /* 3. Save peers and stop network connections */
  if (server->network != NULL) {
    authority_save_peers(server->authority, server->network);
    ATOMIC_STORE(&server->network->running, 0);
    network_shutdown_connections(server->network);
  }

  /* 3. Stop HTTP server, then scheduler pool */
  if (server->http_server != NULL) {
    http_server_stop(server->http_server);
  }
  scheduler_pool_stop(server->pool);

  /* 4. Destroy in reverse order */
  if (server->unix_transport != NULL) {
    unix_transport_destroy(server->unix_transport);
  }
  if (server->ws_transport != NULL) {
    ws_transport_destroy(server->ws_transport);
  }
  if (server->wt_transport != NULL) {
    wt_transport_destroy(server->wt_transport);
  }
  if (server->wt_h3_transport != NULL) {
    webtransport_h3_destroy(server->wt_h3_transport);
  }
  if (server->http_server != NULL) {
    http_server_destroy(server->http_server);
  }
  if (server->network != NULL) {
    network_destroy(server->network);
  }
  if (server->tuple_cache != NULL) {
    tuple_cache_destroy(server->tuple_cache);
  }
  if (server->ofd_cache != NULL) {
    ofd_cache_destroy(server->ofd_cache);
  }
  if (server->block_cache != NULL) {
    block_cache_destroy(server->block_cache);
  }
#ifndef _WIN32
  if (server->update_actor != NULL) {
    update_actor_destroy(server->update_actor);
    g_update_actor = NULL;
  }
#endif
  if (server->timer != NULL) {
    timer_actor_destroy(server->timer);
  }
  if (server->pool != NULL) {
    scheduler_pool_destroy(server->pool);
  }
  if (server->authority != NULL) {
    authority_destroy(server->authority);
  }

  /* Free the embedded config's owning char* members. server->config is an
     embedded value (not heap), so config_free() must NOT be used here (it would
     free() the surrounding struct). Subsystems above already copied/strdup'd
     what they needed (authority cert paths, etc.), so these members are now
     unreferenced. Also fixes a pre-existing shutdown leak and prevents leaks
     across restart cycles. */
  config_free_members(&server->config);

  /* Unlink PID file */
  _remove_pid_file(pid_file);

  printf("Server stopped\n");
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * main
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

int main(int argc, char** argv) {
  /* Default arguments — string fields are heap-allocated so _free_args can
   * free them uniformly. _arg_string_set frees the prior (NULL after memset)
   * value and strdups the new one. */
  offsd_args_t args;
  memset(&args, 0, sizeof(args));
  if (_arg_string_set(&args.host, "0.0.0.0") != 0 ||
      _arg_string_set(&args.cache_dir, "./offs_cache") != 0 ||
      _arg_string_set(&args.data_dir, ".") != 0) {
    fprintf(stderr, "Error: allocation failure during startup\n");
    _free_args(&args);
    return 1;
  }
  args.port = 23402;
  args.quic_port = 23401;
  args.worker_count = 0;
  args.foreground = 0;

  /* Parse CLI flags and config file */
  int parse_result = _parse_args(argc, argv, &args);
  if (parse_result != 0) {
    return parse_result > 0 ? 0 : 1;
  }

  /* Apply log configuration */
  if (args.log_level_str != NULL) {
    log_set_level(log_level_from_string(args.log_level_str));
  }
  if (args.log_structured) {
    log_set_structured(true);
  }

  /* Print banner (before daemonization so it's visible) */
  printf("OFF System Daemon (offsd)\n");
  printf("  Host: %s\n", args.host);
  if (args.port == 0) {
    printf("  HTTP: disabled\n");
  } else {
    printf("  Port: %u\n", args.port);
  }
  if (args.quic_port == 0) {
    printf("  QUIC: disabled\n");
  } else {
    printf("  QUIC: %u\n", args.quic_port);
  }
  printf("  Cache: %s\n", args.cache_dir);
  printf("  Data: %s\n", args.data_dir);
  printf("  Workers: %d\n", args.worker_count);
  if (args.ws_port != 0) {
    printf("  WebSocket: %u\n", args.ws_port);
  }
  if (args.wt_port != 0) {
    printf("  WebTransport: %u\n", args.wt_port);
  }
  if (args.unix_path != NULL) {
    printf("  Unix: %s\n", args.unix_path);
  }
  if (args.pid_file != NULL) {
    printf("  PID file: %s\n", args.pid_file);
  }

  /* Daemonize unless --foreground (once — not per restart cycle) */
  if (!args.foreground) {
    if (_daemonize(args.log_file) != 0) {
      return 1;
    }
  }

  /* If a pending config was staged before this start, apply it as the initial
     config (folded into the first _startup) instead of running offs_node_restart
     after _startup — the old _apply_pending_config destroyed the shared pool the
     transport had already borrowed. */
  config_t* cfg = _load_pending_override(args.data_dir);

  offsd_server_t server;
  for (;;) {
    /* Write PID file each cycle — _shutdown removes it on teardown. */
    if (_write_pid_file(args.pid_file) != 0) {
      if (cfg != NULL) { config_free_members(cfg); free(cfg); cfg = NULL; }
      return 1;
    }

    /* Startup — _startup consumes cfg (steals its members, frees the shell). */
    if (_startup(&server, &args, cfg) != 0) {
      cfg = NULL;  /* _startup freed it even on its failure paths */
      _remove_pid_file(args.pid_file);
      return 1;
    }
    cfg = NULL;

    /* Register signal handlers — must be after the node is populated. */
    g_node = &server.node;
    signal(SIGINT, _signal_handler);
#ifndef _WIN32
    signal(SIGTERM, _signal_handler);
    signal(SIGHUP, _signal_handler);
    signal(SIGPIPE, SIG_IGN);
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    /* Apply metrics server URL from CLI/config file */
    if (args.metrics_server_url != NULL) {
      server.node.authority->metrics_server_url = (char*)args.metrics_server_url;
    }

    /* Start listening */
    _start_listening(&server, &args);

    /* Main loop — wait until a signal clears running (shutdown) or a reload
       RPC sets g_restart_requested. Poll lightly (200 ms) so both flags are
       observed on every platform; a signal interrupts the sleep early. */
    while (ATOMIC_LOAD(&server.node.running) &&
           !ATOMIC_LOAD(&g_restart_requested)) {
      platform_sleep_ms(200);
    }

    /* Graceful shutdown — tears down every subsystem (including the shared
       pool, which is safe here because this runs on the main thread, not a
       pool worker). */
    server.running_val = 0;
    server.draining_val = 1;
    _shutdown(&server, args.pid_file);

    /* If a reload was requested, load the pending config and restart in-place;
       otherwise this was a real shutdown. */
    if (!ATOMIC_LOAD(&g_restart_requested)) {
      break;
    }
    ATOMIC_STORE(&g_restart_requested, 0);
    cfg = _load_pending_override(args.data_dir);
  }

  _free_args(&args);
  return 0;
}
