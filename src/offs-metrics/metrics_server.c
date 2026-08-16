/*
 * offs-metrics — Topology metrics aggregation server
 *
 * Receives CBOR-encoded topology reports from offsd nodes via HTTP POST,
 * stores them in memory, and exposes a JSON query API at GET /status.
 *
 * Usage: offs-metrics [--port PORT] [--host HOST]
 */

#include "ClientAPI/HTTP/http_server.h"
#include "ClientAPI/HTTP/http_request.h"
#include "ClientAPI/HTTP/http_response.h"
#include "ClientAPI/HTTP/http_status.h"
#include "Scheduler/scheduler.h"
#include "Network/topology_report.h"
#include "Network/topology_metrics.h"
#include "Network/node_id.h"
#include "Util/log.h"
#include <cJSON.h>
#include <cbor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static HANDLE g_shutdown_event = NULL;
static BOOL WINAPI _console_ctrl_handler(DWORD ctrl_type) {
  switch (ctrl_type) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
      if (g_shutdown_event != NULL) {
        SetEvent(g_shutdown_event);
      }
      return TRUE;
  }
  return FALSE;
}
#endif

#define MAX_NODES 256
#define MAX_HISTORY_PER_NODE 10

typedef struct {
  node_id_t node_id;
  uint64_t last_seen_ms;
  topology_metrics_t latest;
  int report_count;
} node_report_t;

static node_report_t g_nodes[MAX_NODES];
static int g_node_count = 0;

/* Find or create a node entry */
static node_report_t* _find_or_create_node(const node_id_t* node_id) {
  for (int index = 0; index < g_node_count; index++) {
    if (node_id_equals(&g_nodes[index].node_id, node_id)) {
      return &g_nodes[index];
    }
  }
  if (g_node_count >= MAX_NODES) return NULL;
  node_report_t* entry = &g_nodes[g_node_count];
  memset(entry, 0, sizeof(*entry));
  entry->node_id = *node_id;
  g_node_count++;
  return entry;
}

/* POST /report — receives a CBOR topology report */
static int _handle_report(http_request_t* request, http_response_t* response,
                          void* user_data) {
  (void)user_data;
  if (request->method != HTTP_POST ||
      strcmp(request->path, "/report") != 0) {
    return 0; /* let other handlers try */
  }

  /* Parse CBOR payload */
  if (request->body == NULL || request->body->size == 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_write(response, "empty body", 10);
    http_response_end(response);
    return 1;
  }

  struct cbor_load_result load_result;
  cbor_item_t* item = cbor_load(request->body->data,
                                request->body->size, &load_result);
  if (item == NULL) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_write(response, "invalid CBOR", 12);
    http_response_end(response);
    return 1;
  }

  node_id_t reporter_id;
  uint64_t timestamp_ms = 0;
  topology_metrics_t metrics;
  memset(&metrics, 0, sizeof(metrics));

  int decode_result = topology_report_decode(item, &reporter_id,
                                              &timestamp_ms, &metrics);
  cbor_decref(&item);

  if (decode_result != 0) {
    http_response_set_status(response, HTTP_STATUS_BAD_REQUEST);
    http_response_write(response, "invalid report format", 20);
    http_response_end(response);
    return 1;
  }

  node_report_t* entry = _find_or_create_node(&reporter_id);
  if (entry == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_write(response, "node table full", 15);
    http_response_end(response);
    return 1;
  }

  /* Free previous snapshot arrays if they exist */
  free(entry->latest.peer_snapshots);
  free(entry->latest.ring_entries);
  entry->latest = metrics;
  entry->last_seen_ms = timestamp_ms;
  entry->report_count++;

  log_info("metrics_server: received report from node %s (#%d, %zu peers)",
           reporter_id.str, entry->report_count,
           metrics.peer_snapshot_count);

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_write(response, "{\"status\":\"ok\"}", 15);
  http_response_end(response);
  return 1;
}

/* GET /status — returns JSON with all tracked nodes */
static int _handle_status(http_request_t* request, http_response_t* response,
                          void* user_data) {
  (void)user_data;
  if (request->method != HTTP_GET ||
      strcmp(request->path, "/status") != 0) {
    return 0;
  }

  cJSON* root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "node_count", g_node_count);
  cJSON_AddNumberToObject(root, "timestamp_ms",
                          (double)((uint64_t)time(NULL) * 1000));

  cJSON* nodes = cJSON_CreateArray();
  for (int index = 0; index < g_node_count; index++) {
    node_report_t* entry = &g_nodes[index];
    cJSON* node_json = cJSON_CreateObject();
    cJSON_AddStringToObject(node_json, "node_id", entry->node_id.str);
    cJSON_AddNumberToObject(node_json, "last_seen_ms",
                            (double)entry->last_seen_ms);
    cJSON_AddNumberToObject(node_json, "report_count", entry->report_count);
    cJSON_AddNumberToObject(node_json, "peer_count",
                            (double)entry->latest.peer_snapshot_count);
    cJSON_AddNumberToObject(node_json, "ring_entry_count",
                            (double)entry->latest.ring_entry_count);
    cJSON_AddNumberToObject(node_json, "total_connections",
                            (double)entry->latest.total_connections);
    cJSON_AddNumberToObject(node_json, "avg_hebbian_weight",
                            entry->latest.avg_hebbian_weight);

    /* Per-peer summary */
    if (entry->latest.peer_snapshot_count > 0) {
      cJSON* peers = cJSON_CreateArray();
      for (size_t peer_index = 0;
           peer_index < entry->latest.peer_snapshot_count &&
           peer_index < 20;
           peer_index++) {
        const peer_metrics_snapshot_t* snap =
          &entry->latest.peer_snapshots[peer_index];
        cJSON* peer_json = cJSON_CreateObject();
        cJSON_AddStringToObject(peer_json, "node_id", snap->node_id.str);
        cJSON_AddNumberToObject(peer_json, "hebbian_weight",
                                snap->hebbian_weight);
        cJSON_AddNumberToObject(peer_json, "rtt_ewma_ms", snap->rtt_ewma_ms);
        cJSON_AddBoolToObject(peer_json, "connected", snap->connected);
        /* Sum RPC calls across first 5 types */
        uint64_t total_rpc = 0;
        for (int rpc_index = 0; rpc_index < 5; rpc_index++) {
          total_rpc += snap->rpc_count[rpc_index];
        }
        cJSON_AddNumberToObject(peer_json, "total_rpc_calls",
                                (double)total_rpc);
        cJSON_AddItemToArray(peers, peer_json);
      }
      cJSON_AddItemToObject(node_json, "peers", peers);
    }

    cJSON_AddItemToArray(nodes, node_json);
  }
  cJSON_AddItemToObject(root, "nodes", nodes);

  char* json_str = cJSON_Print(root);
  cJSON_Delete(root);

  if (json_str == NULL) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return 1;
  }

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json_str, strlen(json_str));
  http_response_end(response);
  free(json_str);
  return 1;
}

/* GET /metrics — emits Prometheus text exposition format aggregating
 * the latest reported metrics from every tracked node. */
static int _handle_metrics(http_request_t* request, http_response_t* response,
                           void* user_data) {
  (void)user_data;
  if (request->method != HTTP_GET ||
      strcmp(request->path, "/metrics") != 0) {
    return 0;
  }

  /* Aggregate the latest snapshots across all tracked nodes. */
  size_t total_peers = 0;
  size_t total_ring_entries = 0;
  size_t total_connections = 0;
  double hebbian_sum = 0.0;
  size_t hebbian_samples = 0;
  uint64_t total_rpc_calls = 0;
  uint64_t total_rpc_success = 0;
  uint64_t total_rpc_failure = 0;
  uint64_t total_rate_limit_accepted = 0;
  uint64_t total_rate_limit_rejected = 0;

  for (int index = 0; index < g_node_count; index++) {
    node_report_t* entry = &g_nodes[index];
    total_peers += entry->latest.peer_snapshot_count;
    total_ring_entries += entry->latest.ring_entry_count;
    total_connections += entry->latest.total_connections;
    hebbian_sum += (double)entry->latest.avg_hebbian_weight;
    hebbian_samples++;
    for (int rpc_index = 0; rpc_index < PEER_RPC_TYPE_COUNT; rpc_index++) {
      /* rpc_count/rpc_success/rpc_failure live on peer snapshots, but
       * topology_metrics_t also aggregates total_rpc_calls per type. */
      total_rpc_calls += entry->latest.total_rpc_calls[rpc_index];
    }
    for (int rpc_index = 0; rpc_index < RPC_TYPE_COUNT; rpc_index++) {
      total_rate_limit_accepted +=
        entry->latest.total_rate_limit_accepted[rpc_index];
      total_rate_limit_rejected +=
        entry->latest.total_rate_limit_rejected[rpc_index];
    }
    for (size_t peer_index = 0;
         peer_index < entry->latest.peer_snapshot_count;
         peer_index++) {
      const peer_metrics_snapshot_t* snap =
        &entry->latest.peer_snapshots[peer_index];
      for (int rpc_index = 0; rpc_index < PEER_RPC_TYPE_COUNT; rpc_index++) {
        total_rpc_success += snap->rpc_success[rpc_index];
        total_rpc_failure += snap->rpc_failure[rpc_index];
      }
    }
  }

  double avg_hebbian = (hebbian_samples > 0)
    ? (hebbian_sum / (double)hebbian_samples)
    : 0.0;

  /* Emit Prometheus text exposition format. */
  char body[4096];
  int length = snprintf(body, sizeof(body),
    "# HELP offs_nodes_total Total number of tracked nodes reporting metrics\n"
    "# TYPE offs_nodes_total gauge\n"
    "offs_nodes_total %d\n"
    "# HELP offs_peers_total Total number of known peers across all nodes\n"
    "# TYPE offs_peers_total gauge\n"
    "offs_peers_total %zu\n"
    "# HELP offs_ring_entries Total ring set entries across all nodes\n"
    "# TYPE offs_ring_entries gauge\n"
    "offs_ring_entries %zu\n"
    "# HELP offs_connections_total Total active connections across all nodes\n"
    "# TYPE offs_connections_total gauge\n"
    "offs_connections_total %zu\n"
    "# HELP offs_avg_hebbian_weight Average Hebbian weight across nodes\n"
    "# TYPE offs_avg_hebbian_weight gauge\n"
    "offs_avg_hebbian_weight %f\n"
    "# HELP offs_rpc_calls_total Total RPC calls across all nodes\n"
    "# TYPE offs_rpc_calls_total counter\n"
    "offs_rpc_calls_total %llu\n"
    "# HELP offs_rpc_success_total Total successful RPC calls across all nodes\n"
    "# TYPE offs_rpc_success_total counter\n"
    "offs_rpc_success_total %llu\n"
    "# HELP offs_rpc_failure_total Total failed RPC calls across all nodes\n"
    "# TYPE offs_rpc_failure_total counter\n"
    "offs_rpc_failure_total %llu\n"
    "# HELP offs_rate_limit_accepted_total Total rate-limited accepted requests\n"
    "# TYPE offs_rate_limit_accepted_total counter\n"
    "offs_rate_limit_accepted_total %llu\n"
    "# HELP offs_rate_limit_rejected_total Total rate-limited rejected requests\n"
    "# TYPE offs_rate_limit_rejected_total counter\n"
    "offs_rate_limit_rejected_total %llu\n",
    g_node_count,
    total_peers,
    total_ring_entries,
    total_connections,
    avg_hebbian,
    (unsigned long long)total_rpc_calls,
    (unsigned long long)total_rpc_success,
    (unsigned long long)total_rpc_failure,
    (unsigned long long)total_rate_limit_accepted,
    (unsigned long long)total_rate_limit_rejected);

  if (length < 0 || (size_t)length >= sizeof(body)) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return 1;
  }

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type",
                           "text/plain; version=0.0.4");
  http_response_write(response, body, (size_t)length);
  http_response_end(response);
  return 1;
}

int main(int argc, char** argv) {
  uint16_t port = 9090;
  const char* host = "0.0.0.0";

  for (int index = 1; index < argc; index++) {
    if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
      port = (uint16_t)atoi(argv[++index]);
    } else if (strcmp(argv[index], "--host") == 0 && index + 1 < argc) {
      host = argv[++index];
    } else if (strcmp(argv[index], "--help") == 0) {
      printf("Usage: offs-metrics [--port PORT] [--host HOST]\n");
      printf("  --port PORT   HTTP listen port (default: 9090)\n");
      printf("  --host HOST   Bind host (default: 0.0.0.0)\n");
      return 0;
    }
  }

  log_set_level(LOG_INFO);
  printf("offs-metrics: starting on %s:%u\n", host, port);

  scheduler_pool_t* pool = scheduler_pool_create(2);
  if (pool == NULL) {
    fprintf(stderr, "offs-metrics: failed to create scheduler pool\n");
    return 1;
  }
  scheduler_pool_start(pool);

  http_server_t* server = http_server_create(pool, host, port);
  if (server == NULL) {
    fprintf(stderr, "offs-metrics: failed to create HTTP server on %s:%u\n",
            host, port);
    scheduler_pool_stop(pool);
    scheduler_pool_destroy(pool);
    return 1;
  }

  http_server_use(server, _handle_report, NULL, NULL);
  http_server_use(server, _handle_status, NULL, NULL);
  http_server_use(server, _handle_metrics, NULL, NULL);

  http_server_listen(server);

  printf("offs-metrics: listening on %s:%u\n", host, port);

  /* Wait for SIGINT/SIGTERM */
#ifdef _WIN32
  g_shutdown_event = CreateEventW(NULL, TRUE, FALSE, NULL);
  SetConsoleCtrlHandler(_console_ctrl_handler, TRUE);
  WaitForSingleObject(g_shutdown_event, INFINITE);
  SetConsoleCtrlHandler(_console_ctrl_handler, FALSE);
#else
  sigset_t signal_set;
  sigemptyset(&signal_set);
  sigaddset(&signal_set, SIGINT);
  sigaddset(&signal_set, SIGTERM);
  int caught = 0;
  sigwait(&signal_set, &caught);
#endif

  printf("offs-metrics: shutting down...\n");
  http_server_stop(server);
  http_server_destroy(server);
  scheduler_pool_stop(pool);
  scheduler_pool_destroy(pool);

  /* Free stored reports */
  for (int index = 0; index < g_node_count; index++) {
    free(g_nodes[index].latest.peer_snapshots);
    free(g_nodes[index].latest.ring_entries);
  }

  printf("offs-metrics: stopped\n");
  return 0;
}
