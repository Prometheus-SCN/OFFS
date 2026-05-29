//
// Created by victor on 5/28/25.
//

#ifndef OFFS_SERVICE_H
#define OFFS_SERVICE_H

typedef enum {
  service_result_ok = 0,
  service_result_error = -1,
  service_result_timeout = -2,
  service_result_not_installed = -3
} service_result_e;

typedef struct {
  int (*stop)(void);
  int (*start)(void);
  int (*is_running)(void);
  int (*install)(const char* install_dir);
  int (*uninstall)(void);
  int (*wait_for_stop)(int timeout_ms);
  int (*wait_for_start)(int timeout_ms);
  const char* name;
} service_ops_t;

const service_ops_t* service_get_ops(void);

#endif // OFFS_SERVICE_H
