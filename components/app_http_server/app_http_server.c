#include "app_http_server.h"

#include <stdlib.h>
#include <string.h>

#include "app_http_server_internal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#define OWNER_LENGTH 24
#define MAX_FALLBACKS 8

typedef struct {
    char owner[OWNER_LENGTH];
    char uri[CONFIG_HTTPD_MAX_URI_LEN + 1];
    httpd_method_t method;
    bool used;
} route_entry_t;

typedef struct {
    char owner[OWNER_LENGTH];
    app_http_fallback_fn function;
    void *context;
    bool used;
} fallback_entry_t;

typedef struct {
    char owner[OWNER_LENGTH];
    app_http_info_provider_fn function;
    void *context;
} info_entry_t;

struct app_http_server {
    httpd_handle_t handle;
    SemaphoreHandle_t operation_mutex;
    portMUX_TYPE worker_lock;
    uint32_t active_workers;
    uint32_t max_workers;
    route_entry_t routes[CONFIG_APP_HTTP_SERVER_MAX_HANDLERS];
    fallback_entry_t fallbacks[MAX_FALLBACKS];
    info_entry_t info_providers[MAX_FALLBACKS];
    bool running;
};

typedef enum {
    WORK_REGISTER_ROUTES,
    WORK_REGISTER_FALLBACK,
    WORK_REGISTER_INFO_PROVIDER,
    WORK_UNREGISTER_OWNER,
} work_type_t;

typedef struct {
    app_http_server_t *server;
    work_type_t type;
    const char *owner;
    const httpd_uri_t *handlers;
    size_t handler_count;
    app_http_fallback_fn fallback;
    app_http_info_provider_fn info_provider;
    void *fallback_context;
    SemaphoreHandle_t done;
    esp_err_t result;
} server_work_t;

static const char *TAG = "app_http";
static app_http_server_t *s_server;

static bool owner_is_valid(const char *owner)
{
    return owner && owner[0] && strlen(owner) < OWNER_LENGTH;
}

static route_entry_t *find_free_route(app_http_server_t *server)
{
    for (size_t i = 0; i < CONFIG_APP_HTTP_SERVER_MAX_HANDLERS; ++i) {
        if (!server->routes[i].used) {
            return &server->routes[i];
        }
    }
    return NULL;
}

static bool route_exists(app_http_server_t *server, const char *uri,
                         httpd_method_t method)
{
    for (size_t i = 0; i < CONFIG_APP_HTTP_SERVER_MAX_HANDLERS; ++i) {
        route_entry_t *entry = &server->routes[i];
        if (entry->used && entry->method == method &&
            strcmp(entry->uri, uri) == 0) {
            return true;
        }
    }
    return false;
}

static void unregister_owner_now(app_http_server_t *server, const char *owner)
{
    for (size_t i = 0; i < CONFIG_APP_HTTP_SERVER_MAX_HANDLERS; ++i) {
        route_entry_t *entry = &server->routes[i];
        if (entry->used && strcmp(entry->owner, owner) == 0) {
            httpd_unregister_uri_handler(server->handle, entry->uri,
                                         entry->method);
            memset(entry, 0, sizeof(*entry));
        }
    }
    for (size_t i = 0; i < MAX_FALLBACKS; ++i) {
        info_entry_t *provider = &server->info_providers[i];
        if (provider->function && strcmp(provider->owner, owner) == 0) {
            memset(provider, 0, sizeof(*provider));
        }
        fallback_entry_t *entry = &server->fallbacks[i];
        if (entry->used && strcmp(entry->owner, owner) == 0) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static void execute_work(void *argument)
{
    server_work_t *work = argument;
    app_http_server_t *server = work->server;
    work->result = ESP_OK;

    if (work->type == WORK_REGISTER_ROUTES) {
        size_t registered = 0;
        route_entry_t *registered_entries[CONFIG_APP_HTTP_SERVER_MAX_HANDLERS];
        for (size_t i = 0; i < work->handler_count; ++i) {
            const httpd_uri_t *handler = &work->handlers[i];
            if (!handler->uri || !handler->handler ||
                route_exists(server, handler->uri, handler->method)) {
                work->result = ESP_ERR_INVALID_STATE;
                break;
            }
            route_entry_t *entry = find_free_route(server);
            if (!entry) {
                work->result = ESP_ERR_NO_MEM;
                break;
            }
            work->result = httpd_register_uri_handler(server->handle, handler);
            if (work->result != ESP_OK) {
                break;
            }
            strlcpy(entry->owner, work->owner, sizeof(entry->owner));
            strlcpy(entry->uri, handler->uri, sizeof(entry->uri));
            entry->method = handler->method;
            entry->used = true;
            registered_entries[registered++] = entry;
        }
        if (work->result != ESP_OK && registered > 0) {
            for (size_t i = 0; i < registered; ++i) {
                route_entry_t *entry = registered_entries[i];
                httpd_unregister_uri_handler(server->handle, entry->uri,
                                             entry->method);
                memset(entry, 0, sizeof(*entry));
            }
        }
    } else if (work->type == WORK_REGISTER_FALLBACK) {
        work->result = ESP_ERR_NO_MEM;
        fallback_entry_t *free_entry = NULL;
        for (size_t i = 0; i < MAX_FALLBACKS; ++i) {
            fallback_entry_t *entry = &server->fallbacks[i];
            if (entry->used && strcmp(entry->owner, work->owner) == 0) {
                work->result = ESP_ERR_INVALID_STATE;
                free_entry = NULL;
                break;
            }
            if (!entry->used && !free_entry) {
                free_entry = entry;
            }
        }
        if (free_entry) {
            strlcpy(free_entry->owner, work->owner, sizeof(free_entry->owner));
            free_entry->function = work->fallback;
            free_entry->context = work->fallback_context;
            free_entry->used = true;
            work->result = ESP_OK;
        }
    } else if (work->type == WORK_REGISTER_INFO_PROVIDER) {
        work->result = ESP_ERR_NO_MEM;
        info_entry_t *available = NULL;
        for (size_t i = 0; i < MAX_FALLBACKS; ++i) {
            info_entry_t *entry = &server->info_providers[i];
            if (entry->function && strcmp(entry->owner, work->owner) == 0) {
                work->result = ESP_ERR_INVALID_STATE;
                available = NULL;
                break;
            }
            if (!entry->function && !available) available = entry;
        }
        if (available) {
            strlcpy(available->owner, work->owner, sizeof(available->owner));
            available->function = work->info_provider;
            available->context = work->fallback_context;
            work->result = ESP_OK;
        }
    } else {
        unregister_owner_now(server, work->owner);
    }
    xSemaphoreGive(work->done);
}

static esp_err_t submit_work(server_work_t *work)
{
    app_http_server_t *server = work->server;
    if (!server || !server->running) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(server->operation_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    work->done = xSemaphoreCreateBinary();
    if (!work->done) {
        xSemaphoreGive(server->operation_mutex);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = httpd_queue_work(server->handle, execute_work, work);
    if (err == ESP_OK) {
        xSemaphoreTake(work->done, portMAX_DELAY);
        err = work->result;
    }
    vSemaphoreDelete(work->done);
    xSemaphoreGive(server->operation_mutex);
    return err;
}

static esp_err_t not_found_handler(httpd_req_t *request,
                                   httpd_err_code_t error)
{
    (void)error;
    app_http_server_t *server = httpd_get_global_user_ctx(request->handle);
    if (server) {
        for (size_t i = 0; i < MAX_FALLBACKS; ++i) {
            fallback_entry_t *entry = &server->fallbacks[i];
            if (entry->used && entry->function(request, entry->context)) {
                return ESP_OK;
            }
        }
    }
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
}

esp_err_t app_http_server_start(app_http_server_t **out_server)
{
    if (!out_server) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server) {
        *out_server = s_server;
        return ESP_OK;
    }

    app_http_server_t *server = calloc(1, sizeof(*server));
    if (!server) {
        return ESP_ERR_NO_MEM;
    }
    server->operation_mutex = xSemaphoreCreateMutex();
    if (!server->operation_mutex) {
        free(server);
        return ESP_ERR_NO_MEM;
    }
    server->worker_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    server->max_workers = CONFIG_APP_HTTP_SERVER_MAX_OPEN_SOCKETS - 1;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_APP_HTTP_SERVER_PORT;
    config.max_uri_handlers = CONFIG_APP_HTTP_SERVER_MAX_HANDLERS;
    config.max_open_sockets = CONFIG_APP_HTTP_SERVER_MAX_OPEN_SOCKETS;
    config.stack_size = CONFIG_APP_HTTP_SERVER_TASK_STACK_SIZE;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.global_user_ctx = server;

    esp_err_t err = httpd_start(&server->handle, &config);
    if (err != ESP_OK) {
        vSemaphoreDelete(server->operation_mutex);
        free(server);
        return err;
    }
    server->running = true;
    s_server = server;
    httpd_register_err_handler(server->handle, HTTPD_404_NOT_FOUND,
                               not_found_handler);

    err = system_info_register(server);
    if (err == ESP_OK) err = man_register(server);
    if (err != ESP_OK) {
        app_http_server_stop(server);
        return err;
    }
    ESP_LOGI(TAG, "HTTP server started on port %d",
             CONFIG_APP_HTTP_SERVER_PORT);
    *out_server = server;
    return ESP_OK;
}

esp_err_t app_http_server_register(app_http_server_t *server,
                                   const char *owner,
                                   const httpd_uri_t *handlers,
                                   size_t handler_count)
{
    if (!owner_is_valid(owner) || !handlers || handler_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    server_work_t work = {
        .server = server,
        .type = WORK_REGISTER_ROUTES,
        .owner = owner,
        .handlers = handlers,
        .handler_count = handler_count,
    };
    return submit_work(&work);
}

esp_err_t app_http_server_register_fallback(app_http_server_t *server,
                                            const char *owner,
                                            app_http_fallback_fn fallback,
                                            void *context)
{
    if (!owner_is_valid(owner) || !fallback) {
        return ESP_ERR_INVALID_ARG;
    }
    server_work_t work = {
        .server = server,
        .type = WORK_REGISTER_FALLBACK,
        .owner = owner,
        .fallback = fallback,
        .fallback_context = context,
    };
    return submit_work(&work);
}

esp_err_t app_http_server_unregister_owner(app_http_server_t *server,
                                           const char *owner)
{
    if (!owner_is_valid(owner)) {
        return ESP_ERR_INVALID_ARG;
    }
    server_work_t work = {
        .server = server,
        .type = WORK_UNREGISTER_OWNER,
        .owner = owner,
    };
    return submit_work(&work);
}

httpd_handle_t app_http_server_handle(app_http_server_t *server)
{
    return server ? server->handle : NULL;
}

bool app_http_server_worker_try_acquire(app_http_server_t *server)
{
    if (!server) {
        return false;
    }
    bool acquired = false;
    portENTER_CRITICAL(&server->worker_lock);
    if (server->running && server->active_workers < server->max_workers) {
        ++server->active_workers;
        acquired = true;
    }
    portEXIT_CRITICAL(&server->worker_lock);
    return acquired;
}

void app_http_server_worker_release(app_http_server_t *server)
{
    if (!server) {
        return;
    }
    portENTER_CRITICAL(&server->worker_lock);
    if (server->active_workers > 0) {
        --server->active_workers;
    }
    portEXIT_CRITICAL(&server->worker_lock);
}

esp_err_t app_http_server_stop(app_http_server_t *server)
{
    if (!server || server != s_server) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&server->worker_lock);
    if (server->active_workers > 0) {
        portEXIT_CRITICAL(&server->worker_lock);
        return ESP_ERR_INVALID_STATE;
    }
    server->running = false;
    portEXIT_CRITICAL(&server->worker_lock);
    esp_err_t err = httpd_stop(server->handle);
    vSemaphoreDelete(server->operation_mutex);
    s_server = NULL;
    free(server);
    return err;
}

esp_err_t app_http_server_register_info_provider(app_http_server_t *server,
                                                 const char *owner,
                                                 app_http_info_provider_fn provider,
                                                 void *context)
{
    if (!owner_is_valid(owner) || !provider) return ESP_ERR_INVALID_ARG;
    server_work_t work = {
        .server = server,
        .type = WORK_REGISTER_INFO_PROVIDER,
        .owner = owner,
        .info_provider = provider,
        .fallback_context = context,
    };
    return submit_work(&work);
}

void app_http_server_append_info(app_http_server_t *server, cJSON *root)
{
    if (!server) return;
    for (size_t i = 0; i < MAX_FALLBACKS; ++i) {
        info_entry_t *entry = &server->info_providers[i];
        if (entry->function) entry->function(root, entry->context);
    }
}
