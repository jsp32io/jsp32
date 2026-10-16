#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_TIMEOUT 5
#define ESP_ERR_NVS_NOT_FOUND 6
#define ESP_ERR_NVS_INVALID_LENGTH 7
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 8
#define CONFIG_NETWORK_AP_PREFIX "JSP32"
#define CONFIG_NETWORK_SNTP_SERVER "pool.ntp.org"
typedef struct { bool wait_for_sync; bool start; const char *server; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(host) { .wait_for_sync=true, .start=true, .server=host }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
esp_err_t esp_netif_sntp_start(void);
void esp_netif_sntp_deinit(void);
#define CONFIG_NETWORK_AP_MAX_CONNECTIONS 4
#define CONFIG_NETWORK_CONNECT_TIMEOUT_MS 30000
#define CONFIG_NETWORK_MAX_RETRIES 2
#define CONFIG_NETWORK_SCAN_MAX_RESULTS 20
#define CONFIG_NETWORK_HTTP_MAX_BODY 1024
#define CONFIG_NETWORK_TASK_STACK_SIZE 6144
#define CONFIG_NETWORK_MANAGER_MAX_TX_POWER_QDBM 52
#define CONFIG_APP_HTTP_SERVER_PORT 80
#define ESP_LOGW(tag,...) ((void)(tag))
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
#define ESP_EVENT_DECLARE_BASE(name) extern int name
#define ESP_EVENT_DEFINE_BASE(name) int name
#define WIFI_EVENT 1
#define IP_EVENT 2
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_STOP 2
#define WIFI_EVENT_STA_DISCONNECTED 3
#define WIFI_EVENT_SCAN_DONE 4
#define IP_EVENT_STA_GOT_IP 1
#define IP_EVENT_STA_LOST_IP 2
#define ESP_MAC_WIFI_SOFTAP 1
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define WIFI_STORAGE_RAM 0
#define WIFI_ALL_CHANNEL_SCAN 1
#define WIFI_CONNECT_AP_BY_SIGNAL 1
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_IF_AP 1
#define WIFI_IF_STA 0
#define WIFI_MODE_AP 1
#define WIFI_MODE_STA 2
#define WIFI_MODE_APSTA 3
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(n) (n)
#define HTTP_GET 1
#define HTTP_PUT 2
#define HTTP_POST 3
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_404_NOT_FOUND 404
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_SOCK_ERR_TIMEOUT -2
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) ((ip)->addr&255),(((ip)->addr>>8)&255),(((ip)->addr>>16)&255),(((ip)->addr>>24)&255)

typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef unsigned TickType_t;
typedef int esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef int esp_netif_t;
typedef int wifi_mode_t;
typedef int nvs_handle_t;
typedef void *httpd_handle_t;
typedef struct {uint32_t addr;} esp_ip4_addr_t;
typedef struct {esp_ip4_addr_t ip,netmask,gw;} esp_netif_ip_info_t;
typedef struct {esp_netif_ip_info_t ip_info;} ip_event_got_ip_t;
typedef struct {uint32_t reason;} wifi_event_sta_disconnected_t;
typedef struct {bool nvs_enable;} wifi_init_config_t;
typedef struct {uint8_t ssid[32];int8_t rssi;uint8_t primary;uint8_t authmode;} wifi_ap_record_t;
typedef struct {unsigned num;} wifi_sta_list_t;
typedef union {
 struct {uint8_t ssid[32],password[64];unsigned scan_method,sort_method;struct {int authmode;} threshold;struct {bool capable;} pmf_cfg;} sta;
 struct {uint8_t ssid[32],password[64];unsigned ssid_len,channel,authmode,max_connection;} ap;
} wifi_config_t;
typedef struct {void *user_ctx; const char *uri; int method; size_t content_len; const char *body; size_t received; char response[12000]; int status; char location[128];} httpd_req_t;
typedef struct {const char *uri;int method;esp_err_t (*handler)(httpd_req_t *);void *user_ctx;} httpd_uri_t;

size_t strlcpy(char *, const char *, size_t);
const char *esp_err_to_name(esp_err_t);
int64_t esp_timer_get_time(void);
int xSemaphoreTake(SemaphoreHandle_t,unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t);
int xQueueSend(QueueHandle_t,const void *,unsigned);
int xQueueReceive(QueueHandle_t,void *,unsigned);
QueueHandle_t xQueueCreate(unsigned,size_t);
void vQueueDelete(QueueHandle_t);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *);
void vTaskDelete(TaskHandle_t);
esp_err_t esp_event_post(int,int,const void *,size_t,unsigned);
esp_err_t esp_event_handler_instance_register(int,int,void (*)(void *,int,int,void *),void *,void **);
esp_err_t esp_event_handler_instance_unregister(int,int,void *);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
void esp_netif_destroy_default_wifi(esp_netif_t *);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
esp_err_t esp_netif_get_ip_info(esp_netif_t *,esp_netif_ip_info_t *);
esp_err_t esp_netif_set_ip_info(esp_netif_t *,const esp_netif_ip_info_t *);
esp_err_t esp_netif_dhcps_stop(esp_netif_t *);
esp_err_t esp_netif_dhcps_start(esp_netif_t *);
esp_err_t esp_netif_get_hostname(esp_netif_t *,const char **);
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(wifi_mode_t);
esp_err_t esp_wifi_set_config(int,const wifi_config_t *);
esp_err_t esp_wifi_get_config(int,wifi_config_t *);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_set_max_tx_power(int8_t);
esp_err_t esp_wifi_get_max_tx_power(int8_t *);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *);
esp_err_t esp_wifi_scan_stop(void);
esp_err_t esp_wifi_scan_start(const void *,bool);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *,wifi_ap_record_t *);
esp_err_t esp_wifi_clear_ap_list(void);
esp_err_t esp_read_mac(uint8_t *,int);
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_u8(nvs_handle_t,const char *,uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t,const char *,uint8_t);
esp_err_t nvs_set_str(nvs_handle_t,const char *,const char *);
esp_err_t nvs_erase_key(nvs_handle_t,const char *);
esp_err_t nvs_get_str(nvs_handle_t,const char *,char *,size_t *);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t httpd_resp_send_err(httpd_req_t *,int,const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *,const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *,const char *,const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *,const char *);
esp_err_t httpd_resp_sendstr(httpd_req_t *,const char *);
esp_err_t httpd_resp_send(httpd_req_t *,const char *,size_t);
int httpd_req_recv(httpd_req_t *,char *,size_t);
int httpd_req_to_sockfd(httpd_req_t *);
