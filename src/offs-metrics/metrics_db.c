/*
 * metrics_db.c — WaveDB + GraphQL layer for metrics persistence
 */

#include "metrics_db.h"

/*
 * Include WaveDB headers before any liboffs headers that might
 * conflict. WaveDB's Time/wheel.h defines struct timing_wheel_t
 * which clashes with liboffs's Network/timing_wheel.h.
 *
 * We do NOT include Network/topology_metrics.h here because it
 * transitively includes Network/timing_wheel.h. Instead, the
 * store_report function takes primitive values extracted by the
 * caller.
 */
#include "Layers/graphql/graphql.h"
#include "Database/database.h"
#include "Workers/pool.h"
#include "Util/log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* SDL schema matching the topology report CBOR wire format */
static const char* METRICS_SDL =
  "type NodeReport {"
  "  nodeId: ID!"
  "  timestamp: Int!"
  "  totalConnections: Int"
  "  avgHebbianWeight: Float"
  "}"
  ""
  "type PeerSnapshot {"
  "  nodeId: ID!"
  "  hebbianWeight: Float"
  "  rttEwmaMs: Int"
  "  connected: Boolean"
  "  connectedAtMs: Int"
  "}"
  ""
  "type RingEntry {"
  "  nodeId: ID!"
  "  ringLevel: Int"
  "  rttMs: Float"
  "  capacity: Float"
  "  isActive: Boolean"
  "}"
  ""
  "type RateLimitStats {"
  "  accepted: Int"
  "  rejected: Int"
  "  avgTokens: Float"
  "  effectiveRate: Float"
  "}";

metrics_db_t* metrics_db_create(const char* data_dir,
                                 uint64_t retention_ms,
                                 scheduler_pool_t* pool) {
  if (data_dir == NULL) {
    log_error("metrics_db: data_dir is required");
    return NULL;
  }

  metrics_db_t* database = calloc(1, sizeof(metrics_db_t));
  if (database == NULL) {
    log_error("metrics_db: calloc failed");
    return NULL;
  }

  database->data_dir = strdup(data_dir);
  if (database->data_dir == NULL) {
    log_error("metrics_db: strdup failed");
    free(database);
    return NULL;
  }

  database->retention_ms = retention_ms > 0
    ? retention_ms
    : METRICS_DB_DEFAULT_RETENTION_MS;
  database->pool = pool;

  graphql_layer_config_t* config = graphql_layer_config_default();
  if (config == NULL) {
    log_error("metrics_db: graphql_layer_config_default failed");
    free(database->data_dir);
    free(database);
    return NULL;
  }

  config->path = data_dir;
  config->enable_persist = 1;
  config->lru_memory_mb = 50;
  config->worker_threads = 1;

  int error_code = 0;
  database->layer = graphql_layer_create(data_dir, config, NULL, &error_code);
  graphql_layer_config_destroy(config);

  if (database->layer == NULL) {
    log_error("metrics_db: graphql_layer_create failed (error=%d, path=%s)",
              error_code, data_dir);
    free(database->data_dir);
    free(database);
    return NULL;
  }

  /* Launch the layer's worker pool */
  if (database->layer->pool) {
    work_pool_launch(database->layer->pool);
  }

  /* Register the SDL schema */
  char* schema_error = NULL;
  int schema_result = graphql_schema_parse(database->layer, METRICS_SDL,
                                            &schema_error);
  if (schema_result != 0) {
    log_error("metrics_db: schema parse failed: %s",
              schema_error ? schema_error : "unknown error");
    free(schema_error);
    graphql_layer_destroy(database->layer);
    free(database->data_dir);
    free(database);
    return NULL;
  }

  log_info("metrics_db: opened database at %s", data_dir);
  return database;
}

void metrics_db_destroy(metrics_db_t* database) {
  if (database == NULL) return;

  if (database->layer != NULL) {
    /* Flush all dirty bnodes to disk */
    database_t* db = database->layer->db;
    if (db != NULL) {
      database_flush_dirty_bnodes(db);
    }

    /* Shutdown the layer's worker pool */
    if (database->layer->pool) {
      work_pool_shutdown(database->layer->pool);
      work_pool_join_all(database->layer->pool);
    }

    graphql_layer_destroy(database->layer);
  }

  free(database->data_dir);
  free(database);
  log_info("metrics_db: closed database");
}

/* Build a GraphQL mutation string from a topology report.
 *
 * Returns a heap-allocated mutation string, or NULL on failure.
 * Caller must free(). */
static char* _build_node_report_mutation(const char* node_id_str,
                                          uint64_t timestamp_ms,
                                          size_t total_connections,
                                          float avg_hebbian_weight) {
  size_t buffer_capacity = 4096;
  char* buffer = malloc(buffer_capacity);
  if (buffer == NULL) return NULL;

  int length = snprintf(buffer, buffer_capacity,
    "mutation { createNodeReport("
    "nodeId: \"%s\", "
    "timestamp: \"%llu\", "
    "totalConnections: \"%zu\", "
    "avgHebbianWeight: \"%f\") { id } }",
    node_id_str,
    (unsigned long long)timestamp_ms,
    total_connections,
    (double)avg_hebbian_weight);

  if (length < 0 || (size_t)length >= buffer_capacity) {
    free(buffer);
    return NULL;
  }

  return buffer;
}

int metrics_db_store_report(metrics_db_t* database,
                             const char* node_id_str,
                             uint64_t timestamp_ms,
                             size_t total_connections,
                             float avg_hebbian_weight) {
  if (database == NULL || database->layer == NULL) {
    log_error("metrics_db_store_report: null database or layer");
    return -1;
  }

  char* mutation = _build_node_report_mutation(node_id_str, timestamp_ms,
                                                 total_connections,
                                                 avg_hebbian_weight);
  if (mutation == NULL) {
    log_error("metrics_db_store_report: failed to build mutation");
    return -1;
  }

  graphql_result_t* result = graphql_mutate_sync(database->layer, mutation);
  free(mutation);

  if (result == NULL) {
    log_error("metrics_db_store_report: mutation returned null");
    return -1;
  }

  int success = result->success;
  if (!success) {
    const char* json = graphql_result_to_json(result);
    log_error("metrics_db_store_report: mutation failed: %s",
              json ? json : "unknown error");
  }

  graphql_result_destroy(result);
  return success ? 0 : -1;
}

char* metrics_db_status_json(metrics_db_t* database) {
  if (database == NULL || database->layer == NULL) {
    log_error("metrics_db_status_json: null database or layer");
    return NULL;
  }

  const char* query = "{ NodeReport { nodeId timestamp totalConnections avgHebbianWeight } }";
  graphql_result_t* result = graphql_query_sync(database->layer, query);
  if (result == NULL) {
    log_error("metrics_db_status_json: query returned null");
    return NULL;
  }

  /* graphql_result_to_json returns const char*; make a mutable copy */
  const char* json_const = graphql_result_to_json(result);
  char* json = NULL;
  if (json_const != NULL) {
    json = strdup(json_const);
  }
  graphql_result_destroy(result);

  if (json == NULL) {
    log_error("metrics_db_status_json: result_to_json returned null");
  }

  return json;
}

void metrics_db_prune(metrics_db_t* database) {
  if (database == NULL || database->layer == NULL) return;

  uint64_t now_ms = (uint64_t)time(NULL) * 1000;
  uint64_t cutoff_ms = 0;
  if (now_ms > database->retention_ms) {
    cutoff_ms = now_ms - database->retention_ms;
  }

  if (cutoff_ms == 0) return;

  /* Query declared schema fields; WaveDB appends internal "id" to the JSON
   * output automatically, so we can still parse it from the result.  Using
   * only declared fields avoids GraphQL validation errors. */
  const char* query = "{ NodeReport { nodeId timestamp } }";
  graphql_result_t* result = graphql_query_sync(database->layer, query);
  if (result == NULL || !result->success) {
    log_error("metrics_db_prune: query failed");
    if (result) graphql_result_destroy(result);
    return;
  }

  const char* json_const = graphql_result_to_json(result);
  graphql_result_destroy(result);

  if (json_const == NULL) {
    log_error("metrics_db_prune: result_to_json returned null");
    return;
  }

  /* Make a mutable copy for scanning */
  char* json = strdup(json_const);
  if (json == NULL) {
    log_error("metrics_db_prune: strdup failed");
    return;
  }

  const char* cursor = json;
  while ((cursor = strstr(cursor, "\"id\":")) != NULL) {
    cursor += 5;

    const char* id_start = NULL;
    size_t id_len = 0;
    if (*cursor == '"') {
      cursor++;
      id_start = cursor;
      const char* id_end = strchr(cursor, '"');
      if (id_end == NULL) break;
      id_len = (size_t)(id_end - cursor);
      cursor = id_end + 1;
    }

    const char* ts_ptr = strstr(cursor, "\"timestamp\":");
    if (ts_ptr == NULL) break;
    ts_ptr += 12;

    uint64_t entity_timestamp = 0;
    if (*ts_ptr == '"') {
      ts_ptr++;
      entity_timestamp = strtoull(ts_ptr, NULL, 10);
    } else {
      entity_timestamp = strtoull(ts_ptr, NULL, 10);
    }

    if (entity_timestamp < cutoff_ms && id_start != NULL && id_len > 0) {
      char* id_str = strndup(id_start, id_len);
      if (id_str != NULL) {
        char delete_mutation[256];
        snprintf(delete_mutation, sizeof(delete_mutation),
                 "mutation { deleteNodeReport(id: \"%s\") { id } }", id_str);
        graphql_result_t* del_result = graphql_mutate_sync(database->layer, delete_mutation);
        if (del_result != NULL) {
          if (!del_result->success) {
            log_error("metrics_db_prune: delete failed for id=%s", id_str);
          }
          graphql_result_destroy(del_result);
        }
        free(id_str);
      }
    }

    const char* next_obj = strstr(cursor, "\"id\":");
    if (next_obj) cursor = next_obj;
    else break;
  }

  free(json);
  log_info("metrics_db_prune: completed pruning pass");
}