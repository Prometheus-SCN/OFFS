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
#include "ClientAPI/Unix/unix_transport.h"
#include "ClientAPI/HTTP/health_routes.h"
#include "ClientAPI/health_handler.h"
#include "ClientAPI/HTTP/peer_routes.h"
#include "ClientAPI/HTTP/config_routes.h"
#include "Node/node.h"
#include "Network/authority.h"
#include "Network/network.h"
#include "OFFStreams/tuple_cache.h"
#include "BlockCache/block_cache.h"
#include "OFFStreams/ofd_cache.h"
#include "Scheduler/scheduler.h"
#include "Timer/timer_actor.h"
#include "Configuration/config.h"
#include "Configuration/config_pending.h"
#include "Platform/platform.h"
#include "Update/update_actor.h"
#include "Update/update_check.h"
#include "Version/version.h"
#include "ClientAPI/update_status_handler.h"
#include "Util/allocator.h"
#include "Util/log.h"
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

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * CLI argument structure — holds everything parsed from flags + config file
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

typedef struct {
  const char* config_path;
  const char* host;
  uint16_t    port;
  const char* unix_path;
  const char* cache_dir;
  const char* data_dir;
  const char* pid_file;
  int         worker_count;
  int         foreground;
  const char* log_level_str;
  int         log_structured;
  const char* metrics_server_url;
  const char* ca_cert_path;
  const char* node_cert_path;
  const char* node_key_path;
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
  fprintf(stderr, "  --unix <path>        Unix socket path\n");
  fprintf(stderr, "  --cache-dir <dir>    Block cache directory\n");
  fprintf(stderr, "  --data-dir <dir>     Persistent data directory\n");
  fprintf(stderr, "  --pid-file <path>    PID file path\n");
  fprintf(stderr, "  --workers <n>        Worker count, 0=auto (default: 0)\n");
  fprintf(stderr, "  --foreground         Run in foreground (do not daemonize)\n");
  fprintf(stderr, "  --log-level <lvl>    Log level: trace, debug, info, warn, error, fatal (default: info)\n");
  fprintf(stderr, "  --log-structured     Enable key=value structured log output\n");
  fprintf(stderr, "  --metrics-server <url>  Metrics server URL for topology reports\n");
  fprintf(stderr, "  --ca-cert <path>      CA certificate PEM path\n");
  fprintf(stderr, "  --node-cert <path>    Node certificate PEM path\n");
  fprintf(stderr, "  --node-key <path>     Node private key PEM path\n");
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

  cJSON_Delete(root);
  return 0;
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * CLI argument parsing
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static int _parse_args(int argc, char** argv, offsd_args_t* args) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      args->config_path = argv[++i];
    } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      args->host = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      args->port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--unix") == 0 && i + 1 < argc) {
      args->unix_path = argv[++i];
    } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
      args->cache_dir = argv[++i];
    } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
      args->data_dir = argv[++i];
    } else if (strcmp(argv[i], "--pid-file") == 0 && i + 1 < argc) {
      args->pid_file = argv[++i];
    } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
      args->worker_count = atoi(argv[++i]);
      if (args->worker_count < 0) args->worker_count = 0;
    } else if (strcmp(argv[i], "--foreground") == 0) {
      args->foreground = 1;
    } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
      args->log_level_str = argv[++i];
    } else if (strcmp(argv[i], "--log-structured") == 0) {
      args->log_structured = 1;
    } else if (strcmp(argv[i], "--metrics-server") == 0 && i + 1 < argc) {
      args->metrics_server_url = argv[++i];
    } else if (strcmp(argv[i], "--ca-cert") == 0 && i + 1 < argc) {
      args->ca_cert_path = argv[++i];
    } else if (strcmp(argv[i], "--node-cert") == 0 && i + 1 < argc) {
      args->node_cert_path = argv[++i];
    } else if (strcmp(argv[i], "--node-key") == 0 && i + 1 < argc) {
      args->node_key_path = argv[++i];
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
  if (args->data_dir != NULL)    free((void*)args->data_dir);
  if (args->pid_file != NULL)    free((void*)args->pid_file);
  if (args->host != NULL)        free((void*)args->host);
  if (args->unix_path != NULL)   free((void*)args->unix_path);
  if (args->cache_dir != NULL)    free((void*)args->cache_dir);
  if (args->metrics_server_url != NULL) free((void*)args->metrics_server_url);
  if (args->ca_cert_path != NULL)     free((void*)args->ca_cert_path);
  if (args->node_cert_path != NULL)   free((void*)args->node_cert_path);
  if (args->node_key_path != NULL)    free((void*)args->node_key_path);
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Daemonization (double-fork)
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static int _daemonize(void) {
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

  int dev_null = open("/dev/null", O_RDWR);
  if (dev_null < 0) {
    fprintf(stderr, "Failed to open /dev/null: %s\n", strerror(errno));
    return -1;
  }
  dup2(dev_null, STDIN_FILENO);
  dup2(dev_null, STDOUT_FILENO);
  dup2(dev_null, STDERR_FILENO);
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

static int _startup(offsd_server_t* server, const offsd_args_t* args) {
  memset(server, 0, sizeof(*server));

  /* Thread setup */
  platform_thread_setup_stack();

  /* Scheduler pool */
  server->pool = scheduler_pool_create(args->worker_count);
  if (server->pool == NULL) {
    fprintf(stderr, "Failed to create scheduler pool\n");
    return -1;
  }
  scheduler_pool_start(server->pool);

  /* Timer actor */
  server->timer = timer_actor_create(server->pool);

  /* Configuration */
  server->config = config_default();

  /* Block cache */
  server->block_cache = block_cache_create(server->config,
      (char*)args->cache_dir, standard, server->timer,
      server->pool, NULL, 0);
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
    off_routes_register(server->http_server, server->pool,
                        server->block_cache, server->ofd_cache,
                        server->tuple_cache, NULL, NULL,
                        &server->open_stream_count);
    block_routes_register(server->http_server, server->pool,
                          server->block_cache, NULL, NULL);
    health_routes_register(server->http_server, &server->health_ctx);
    peer_routes_register(server->http_server, &server->node,
                         &server->config, NULL);
    config_routes_register(server->http_server, &server->node,
                           &server->config, args->data_dir);
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
       borrowed (owned by server); data_dir is copied by the setter. */
    unix_transport_set_config_ctx(server->unix_transport, &server->node,
                                  args->data_dir);
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
 * Apply pending config (if any)
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static void _apply_pending_config(offsd_server_t* server, const char* data_dir) {
  if (data_dir == NULL) return;
  if (config_pending_exists(data_dir) == 1) {
    offs_node_restart(&server->node, data_dir);
  }
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * Start listening
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

static void _start_listening(offsd_server_t* server,
                             const char* host, uint16_t port,
                             const char* unix_path) {
  if (server->http_server != NULL) {
    http_server_listen(server->http_server);
  }

  if (server->unix_transport != NULL) {
    unix_transport_start(server->unix_transport);
    printf("Listening on unix://%s\n", unix_path);
  }

  authority_load_peers(server->authority, server->network);
  network_start_connections(server->network);

  if (server->http_server != NULL) {
    printf("Listening on http://%s:%u\n", host, port);
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

  /* 2. Save peers and stop network connections */
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

  /* Unlink PID file */
  _remove_pid_file(pid_file);

  printf("Server stopped\n");
}

/*━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 * main
 *━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━*/

int main(int argc, char** argv) {
  /* Default arguments */
  offsd_args_t args;
  memset(&args, 0, sizeof(args));
  args.host = "0.0.0.0";
  args.port = 23402;
  args.cache_dir = "./offs_cache";
  args.data_dir = ".";
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
  printf("  Cache: %s\n", args.cache_dir);
  printf("  Data: %s\n", args.data_dir);
  printf("  Workers: %d\n", args.worker_count);
  if (args.unix_path != NULL) {
    printf("  Unix: %s\n", args.unix_path);
  }
  if (args.pid_file != NULL) {
    printf("  PID file: %s\n", args.pid_file);
  }

  /* Daemonize unless --foreground */
  if (!args.foreground) {
    if (_daemonize() != 0) {
      return 1;
    }
  }

  /* Write PID file */
  if (_write_pid_file(args.pid_file) != 0) {
    return 1;
  }

  /* Startup */
  offsd_server_t server;
  if (_startup(&server, &args) != 0) {
    _remove_pid_file(args.pid_file);
    return 1;
  }

  /* Register signal handlers — must be after node_obj is populated */
  g_node = &server.node;
  signal(SIGINT, _signal_handler);
#ifndef _WIN32
  signal(SIGTERM, _signal_handler);
  signal(SIGHUP, _signal_handler);
#endif

  /* Apply any pending config from a previous shutdown */
  _apply_pending_config(&server, args.data_dir);

  /* Apply metrics server URL from CLI/config file */
  if (args.metrics_server_url != NULL) {
    server.node.authority->metrics_server_url = (char*)args.metrics_server_url;
  }

  /* Start listening */
  _start_listening(&server, args.host, args.port, args.unix_path);

  /* Main loop — wait until signal sets running=0 */
  while (ATOMIC_LOAD(&server.node.running)) {
#ifdef _WIN32
    /* Windows has no pause(); the console control handler (SIGINT) clears the
     * running flag. Poll lightly so the loop observes the flag change. */
    platform_sleep_ms(1000);
#else
    pause();
#endif
  }

  /* Graceful shutdown */
  server.running_val = 0;
  server.draining_val = 1;
  _shutdown(&server, args.pid_file);

  _free_args(&args);
  return 0;
}
