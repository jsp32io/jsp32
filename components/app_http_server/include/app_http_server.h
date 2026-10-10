#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_http_server app_http_server_t;

/* Return true when the fallback sent a response. */
typedef bool (*app_http_fallback_fn)(httpd_req_t *request, void *context);

esp_err_t app_http_server_start(app_http_server_t **out_server);
esp_err_t app_http_server_register(app_http_server_t *server,
                                   const char *owner,
                                   const httpd_uri_t *handlers,
                                   size_t handler_count);
esp_err_t app_http_server_register_fallback(app_http_server_t *server,
                                            const char *owner,
                                            app_http_fallback_fn fallback,
                                            void *context);
esp_err_t app_http_server_unregister_owner(app_http_server_t *server,
                                           const char *owner);
/* Providers append module-owned fields to /system/info in the HTTP task.
   unregister_owner() also removes the provider before its context is freed. */
typedef void (*app_http_info_provider_fn)(cJSON *root, void *context);
esp_err_t app_http_server_register_info_provider(app_http_server_t *server,
                                                 const char *owner,
                                                 app_http_info_provider_fn provider,
                                                 void *context);
httpd_handle_t app_http_server_handle(app_http_server_t *server);

/* Shared by components that keep an HTTP socket in an async worker. */
bool app_http_server_worker_try_acquire(app_http_server_t *server);
void app_http_server_worker_release(app_http_server_t *server);

esp_err_t app_http_server_stop(app_http_server_t *server);

#ifdef __cplusplus
}
#endif
