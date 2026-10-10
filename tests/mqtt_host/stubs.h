#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "quickjs.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NVS_NOT_FOUND 4
#define ESP_ERR_NVS_INVALID_LENGTH 5
#define ESP_LOGW(tag,...) ((void)(tag))
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffu
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t,unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);
void vTaskDelay(unsigned);
int64_t esp_timer_get_time(void);
#define ESP_MAC_WIFI_STA 0
esp_err_t esp_read_mac(uint8_t *,int);
esp_err_t esp_crt_bundle_attach(void *);
typedef const char *esp_event_base_t;
#define ESP_EVENT_ANY_ID -1
enum {MQTT_EVENT_CONNECTED=1,MQTT_EVENT_DISCONNECTED,MQTT_EVENT_ERROR,MQTT_EVENT_BEFORE_CONNECT};
typedef struct fake_client *esp_mqtt_client_handle_t;
typedef struct {
    struct {struct {const char *uri;} address;struct {esp_err_t (*crt_bundle_attach)(void *);} verification;} broker;
    struct {const char *client_id,*username;struct {const char *password;} authentication;} credentials;
    struct {int reconnect_timeout_ms,timeout_ms;} network;
    struct {uint64_t limit;} outbox;
} esp_mqtt_client_config_t;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t,int,
    void (*)(void *,esp_event_base_t,int32_t,void *),void *);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t);
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t,const char *,const char *,int,int,int,bool);
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_u8(nvs_handle_t,const char *,uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t,const char *,uint8_t);
esp_err_t nvs_get_str(nvs_handle_t,const char *,char *,size_t *);
esp_err_t nvs_set_str(nvs_handle_t,const char *,const char *);
esp_err_t nvs_commit(nvs_handle_t);
typedef struct app_http_server app_http_server_t;
typedef struct {void *user_ctx;size_t content_len;const char *body;size_t cursor;char *response;int error;} httpd_req_t;
typedef struct {const char *uri;int method;esp_err_t (*handler)(httpd_req_t *);void *user_ctx;} httpd_uri_t;
#define HTTP_GET 0
#define HTTP_PUT 1
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
esp_err_t httpd_resp_send_err(httpd_req_t *,int,const char *);
esp_err_t httpd_resp_sendstr(httpd_req_t *,const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *,const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *,const char *,const char *);
int httpd_req_recv(httpd_req_t *,char *,size_t);
esp_err_t app_http_server_register(app_http_server_t *,const char *,const httpd_uri_t *,size_t);
esp_err_t app_http_server_unregister_owner(app_http_server_t *,const char *);
typedef struct js_runtime js_runtime_t;
typedef esp_err_t (*js_runtime_module_init_fn)(js_runtime_t *,JSContext *,void *);
typedef void (*js_runtime_module_deinit_fn)(js_runtime_t *,JSContext *,void *);
esp_err_t js_runtime_register_module(js_runtime_module_init_fn,js_runtime_module_deinit_fn,void *);
