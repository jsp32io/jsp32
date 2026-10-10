#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "quickjs.h"
#include "js_request.h"

#define CONFIG_JS_NET_ENABLED 1
#define CONFIG_JS_NET_TLS_ENABLED 1
#define CONFIG_JS_NET_MAX_TLS_CONNECTIONS 2
#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 1
#define CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_MAX_CERTS 200
#define CONFIG_JS_NET_MAX_CONNECTIONS 16
#define CONFIG_JS_NET_MAX_PENDING 8
#define CONFIG_JS_NET_MAX_TRANSFER_SIZE 16384
#define CONFIG_JS_NET_BUFFER_LIMIT 32768
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_ERR_INVALID_ARG 4
#define likely(x) (x)
#define unlikely(x) (x)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
typedef int BaseType_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
typedef struct host_queue *QueueHandle_t;
typedef QueueHandle_t SemaphoreHandle_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) pthread_mutex_lock(lock)
#define portEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)
QueueHandle_t xQueueCreate(unsigned count, unsigned size);
int xQueueSend(QueueHandle_t queue, const void *data, TickType_t wait);
int xQueueReceive(QueueHandle_t queue, void *data, TickType_t wait);
unsigned uxQueueMessagesWaiting(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
int xTaskCreate(void (*entry)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelete(void *);
int64_t esp_timer_get_time(void);

typedef struct js_runtime { JSContext *ctx; } js_runtime_t;
typedef struct { uint16_t type; } js_event_t;
#define JS_EVENT_NET 0x800
typedef void (*js_event_handler_fn)(js_runtime_t *, const js_event_t *, void *);
typedef esp_err_t (*js_runtime_module_init_fn)(js_runtime_t *, JSContext *, void *);
typedef void (*js_runtime_module_deinit_fn)(js_runtime_t *, JSContext *, void *);
esp_err_t js_runtime_register_module(js_runtime_module_init_fn, js_runtime_module_deinit_fn, void *);
JSContext *js_runtime_context(js_runtime_t *runtime);
esp_err_t js_runtime_track_requests(js_runtime_t *runtime, js_request_table_t *table);
esp_err_t js_event_register_handler(js_runtime_t *, uint16_t, js_event_handler_fn, void *);
esp_err_t js_event_unregister_handler(js_runtime_t *, uint16_t);
esp_err_t js_event_post(js_runtime_t *, const js_event_t *, TickType_t);

int host_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
#define getaddrinfo host_getaddrinfo
ssize_t host_send(int, const void *, size_t, int);
#define send host_send
