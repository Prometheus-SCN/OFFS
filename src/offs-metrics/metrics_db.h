/*
 * metrics_db.h — WaveDB + GraphQL layer for metrics persistence
 *
 * Manages the lifecycle of a WaveDB database with a GraphQL schema layer
 * for storing and querying topology reports. Each incoming report is
 * persisted as a NodeReport entity via GraphQL mutations.
 */

#ifndef METRICS_DB_H
#define METRICS_DB_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations — avoids pulling in headers that conflict with WaveDB */
typedef struct graphql_layer_t graphql_layer_t;
typedef struct scheduler_pool_t scheduler_pool_t;

/* Retention window in milliseconds (default: 7 days) */
#define METRICS_DB_DEFAULT_RETENTION_MS (7ULL * 24 * 60 * 60 * 1000)
/* Pruning interval in milliseconds (default: 1 hour) */
#define METRICS_DB_PRUNE_INTERVAL_MS (60ULL * 60 * 1000)

typedef struct metrics_db_t metrics_db_t;

struct metrics_db_t {
  graphql_layer_t* layer;
  char* data_dir;
  uint64_t retention_ms;
  scheduler_pool_t* pool;
};

/* Create and open the metrics database.
 *
 * Creates or opens a persistent WaveDB at data_dir with a GraphQL
 * schema layer registered for NodeReport, PeerSnapshot, RingEntry,
 * and RateLimitStats types.
 *
 * Returns NULL on failure (logs error).
 */
metrics_db_t* metrics_db_create(const char* data_dir,
                                 uint64_t retention_ms,
                                 scheduler_pool_t* pool);

/* Destroy the metrics database.
 *
 * Flushes dirty bnodes, destroys the GraphQL layer, frees resources.
 */
void metrics_db_destroy(metrics_db_t* database);

/* Store a topology report in the database.
 *
 * Decoded report fields are persisted as a GraphQL NodeReport entity.
 * Returns 0 on success, -1 on failure (logs error).
 */
int metrics_db_store_report(metrics_db_t* database,
                             const char* node_id_str,
                             uint64_t timestamp_ms,
                             size_t total_connections,
                             float avg_hebbian_weight);

/* Run one pruning pass: delete NodeReport entities where timestamp
 * falls outside the retention window.
 *
 * Non-fatal: logs errors but does not fail the caller.
 */
void metrics_db_prune(metrics_db_t* database);

/* Query all NodeReport entities and return as JSON string.
 *
 * Caller must free the returned string with free().
 * Returns NULL on failure.
 */
char* metrics_db_status_json(metrics_db_t* database);

#endif