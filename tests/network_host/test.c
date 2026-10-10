#include "sdk.h"
#include <assert.h>
#include <stdlib.h>
#include "../../components/network_manager/network_manager.c"
#include "../../components/network_manager/network_store.c"
#include "../../components/network_manager/network_http.c"

static network_settings_t disk_record;
static bool disk_strings[4];
static unsigned string_writes, fail_string_write;
static bool disk_present, associated;
static esp_err_t commit_result;
static int commits, radio_mode, connects, scans, driver_starts, driver_stops, mode_changes;
static wifi_ap_record_t scan_records[3];
static uint16_t scan_count;
static esp_err_t scan_start_error, scan_read_error;
static int64_t now_us;
static wifi_config_t radio_ap, radio_sta;
static int netif_ap = 1, netif_sta = 2;
static int8_t radio_power = 52;
static uint32_t station_ip = 0x6401a8c0;
static network_command_t queued[32];
static unsigned queue_count;
static app_http_info_provider_fn provider;
static void *provider_context;
static httpd_uri_t routes[20];
static size_t route_count;

size_t strlcpy(char *out, const char *in, size_t size) { size_t length=strlen(in);if(size){size_t n=length<size-1?length:size-1;memcpy(out,in,n);out[n]=0;}return length; }
const char *esp_err_to_name(esp_err_t err) { return err ? "TEST_ERROR" : "ESP_OK"; }
int64_t esp_timer_get_time(void) { return now_us; }
int xSemaphoreTake(SemaphoreHandle_t handle,unsigned timeout) { return 1; }
int xSemaphoreGive(SemaphoreHandle_t handle) { return 1; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)1; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
void vSemaphoreDelete(SemaphoreHandle_t handle) {}
int xQueueSend(QueueHandle_t handle,const void *value,unsigned timeout) { assert(queue_count<32);queued[queue_count++]=*(const network_command_t *)value;return 1; }
int xQueueReceive(QueueHandle_t handle,void *value,unsigned timeout) { if(!queue_count)return 0;*(network_command_t *)value=queued[0];memmove(queued,queued+1,--queue_count*sizeof(*queued));return 1; }
QueueHandle_t xQueueCreate(unsigned count,size_t size) { return (void *)1; }
void vQueueDelete(QueueHandle_t handle) {}
int xTaskCreate(void (*function)(void *),const char *name,unsigned stack,void *context,unsigned priority,TaskHandle_t *out) {*out=(void *)1;return 1;}
void vTaskDelete(TaskHandle_t task) {}
esp_err_t esp_event_post(int base,int id,const void *data,size_t size,unsigned timeout) {return ESP_OK;}
esp_err_t esp_wifi_set_mode(wifi_mode_t mode) {radio_mode=mode;++mode_changes;return ESP_OK;}
esp_err_t esp_wifi_set_config(int type,const wifi_config_t *config) {if(type==WIFI_IF_AP)radio_ap=*config;else radio_sta=*config;return ESP_OK;}
esp_err_t esp_wifi_get_config(int type,wifi_config_t *out) {*out=type==WIFI_IF_AP?radio_ap:radio_sta;return ESP_OK;}
esp_err_t esp_wifi_start(void) {++driver_starts;return ESP_OK;}
esp_err_t esp_wifi_stop(void) {++driver_stops;associated=false;return ESP_OK;}
esp_err_t esp_wifi_connect(void) {++connects;return ESP_OK;}
esp_err_t esp_wifi_disconnect(void) {associated=false;return ESP_OK;}
esp_err_t esp_wifi_set_max_tx_power(int8_t power) {radio_power=power;return ESP_OK;}
esp_err_t esp_wifi_get_max_tx_power(int8_t *power) {*power=radio_power;return ESP_OK;}
esp_err_t wifi_tx_power_load(int8_t *power) {*power=52;return ESP_OK;}
esp_err_t wifi_tx_power_save(int8_t power) {return ESP_OK;}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out) {if(!associated)return ESP_FAIL;memset(out,0,sizeof(*out));memcpy(out->ssid,radio_sta.sta.ssid,32);out->rssi=-45;return ESP_OK;}
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *out) {out->num=1;return ESP_OK;}
esp_err_t esp_wifi_scan_stop(void) {return ESP_OK;}
esp_err_t esp_wifi_scan_start(const void *config,bool block) {assert(block);assert(radio_mode==WIFI_MODE_APSTA||radio_mode==WIFI_MODE_STA);++scans;return scan_start_error;}
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *count) {*count=scan_count;return scan_read_error;}
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *count,wifi_ap_record_t *records) {if(*count>scan_count)*count=scan_count;memcpy(records,scan_records,*count*sizeof(*records));return ESP_OK;}
esp_err_t esp_wifi_clear_ap_list(void) {return ESP_OK;}
esp_err_t esp_read_mac(uint8_t *mac,int type) {memset(mac,0,6);mac[4]=0xab;mac[5]=0xcd;return ESP_OK;}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {return strcmp(key,"WIFI_STA_DEF")==0?&netif_sta:&netif_ap;}
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif,esp_netif_ip_info_t *info) {memset(info,0,sizeof(*info));info->ip.addr=netif==&netif_sta?station_ip:0x0104a8c0;return ESP_OK;}
esp_err_t esp_netif_get_hostname(esp_netif_t *netif,const char **hostname) {*hostname="jsp32-test";return ESP_OK;}
esp_err_t network_dns_start(network_manager_t *manager) {return ESP_OK;}
void network_dns_stop(network_manager_t *manager) {}
esp_err_t nvs_open(const char *name,int access,nvs_handle_t *handle) {
 *handle=1;assert(strcmp(name,"jsp32_config")==0);
 bool present=disk_present;
 for(size_t i=0;i<4;++i)present|=disk_strings[i];
 return present||access==NVS_READWRITE?ESP_OK:ESP_ERR_NVS_NOT_FOUND;
}
void nvs_close(nvs_handle_t handle) {}
static int string_index(const char *key) {
 const char *keys[]={"ap_ssid","ap_pass","wifi_ssid","wifi_pass"};
 for(size_t i=0;i<4;++i)if(strcmp(key,keys[i])==0)return (int)i;
 assert(!"Unexpected NVS key");return -1;
}
static char *disk_string(int index) {
 char *values[]={disk_record.ap_ssid,disk_record.ap_password,disk_record.sta_ssid,disk_record.sta_password};return values[index];
}
esp_err_t nvs_get_u8(nvs_handle_t handle,const char *key,uint8_t *out) {
 assert(strcmp(key,"wifi_mode")==0);if(!disk_present)return ESP_ERR_NVS_NOT_FOUND;*out=(uint8_t)disk_record.mode;return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle,const char *key,uint8_t value) {
 assert(strcmp(key,"wifi_mode")==0);disk_record.mode=(network_mode_t)value;disk_present=true;return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle,const char *key,char *out,size_t *size) {
 int i=string_index(key);const char *value;
 if(disk_strings[i])value=disk_string(i);
 else return ESP_ERR_NVS_NOT_FOUND;
 size_t needed=strlen(value)+1;
 if(!out){*size=needed;return ESP_OK;}
 if(*size<needed){*size=needed;return ESP_ERR_NVS_INVALID_LENGTH;}
 memcpy(out,value,needed);*size=needed;return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle,const char *key,const char *value) {
 if(++string_writes==fail_string_write)return ESP_FAIL;
 int i=string_index(key);size_t capacities[]={33,64,33,65};assert(strlen(value)<capacities[i]);strlcpy(disk_string(i),value,capacities[i]);disk_strings[i]=true;return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle,const char *key) {
 if(strcmp(key,"wifi_mode")==0){if(!disk_present)return ESP_ERR_NVS_NOT_FOUND;disk_present=false;return ESP_OK;}
 int i=string_index(key);if(!disk_strings[i])return ESP_ERR_NVS_NOT_FOUND;disk_strings[i]=false;disk_string(i)[0]=0;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {if(commit_result)return commit_result;++commits;return ESP_OK;}
esp_err_t app_http_server_register(app_http_server_t *server,const char *owner,const httpd_uri_t *handlers,size_t count) {assert(count<=20);memcpy(routes,handlers,count*sizeof(*routes));route_count=count;return ESP_OK;}
esp_err_t app_http_server_register_info_provider(app_http_server_t *server,const char *owner,app_http_info_provider_fn function,void *context) {provider=function;provider_context=context;return ESP_OK;}
esp_err_t app_http_server_unregister_owner(app_http_server_t *server,const char *owner) {route_count=0;provider=NULL;provider_context=NULL;return ESP_OK;}
esp_err_t httpd_resp_set_type(httpd_req_t *request,const char *type) {return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t *request,const char *name,const char *value) {return ESP_OK;}
esp_err_t httpd_resp_set_status(httpd_req_t *request,const char *status) {request->status=atoi(status);return ESP_OK;}
esp_err_t httpd_resp_sendstr(httpd_req_t *request,const char *text) {strlcpy(request->response,text,sizeof(request->response));return ESP_OK;}
esp_err_t httpd_resp_send(httpd_req_t *request,const char *text,size_t length) {return ESP_OK;}
esp_err_t httpd_resp_send_err(httpd_req_t *request,int status,const char *text) {request->status=status;return httpd_resp_sendstr(request,text);}
int httpd_req_recv(httpd_req_t *request,char *out,size_t capacity) {size_t remaining=request->content_len-request->received;size_t size=remaining<capacity?remaining:capacity;memcpy(out,request->body+request->received,size);request->received+=size;return size;}
int httpd_req_to_sockfd(httpd_req_t *request) {return -1;}

static network_manager_t make_manager(void) {
 network_manager_t manager={0};manager.status_mutex=(void *)1;manager.queue=(void *)1;manager.task=(void *)1;manager.sta_netif=&netif_sta;manager.ap_netif=&netif_ap;
 bool saved=false;assert(network_store_load(&manager.saved,&saved)==ESP_OK);manager.status.saved_mode=manager.saved.mode;manager.status.mode_saved=saved;
 assert(activate(&manager,&manager.saved,manager.saved.mode)==ESP_OK);update_state(&manager,manager.saved.mode==NETWORK_MODE_AP?NETWORK_STATE_AP_READY:NETWORK_STATE_STA_CONNECTING,ESP_OK);return manager;
}
static void drain(network_manager_t *manager) {network_command_t command;while(xQueueReceive(manager->queue,&command,0)){handle_command(manager,&command);network_secure_clear(&command,sizeof(command));}}
static void start_sta(network_manager_t *manager) {network_command_t command={.type=NETWORK_COMMAND_STA_START,.session=manager->session};handle_command(manager,&command);}
static void configure(network_manager_t *manager,const network_settings_t *settings) {uint32_t id;assert(network_manager_apply(manager,settings,&id)==ESP_OK);assert(manager->status.pending);drain(manager);now_us+=1000001;handle_timers(manager);}

int main(void) {
 network_settings_t settings;bool saved;
 assert(network_store_load(&settings,&saved)==ESP_OK);assert(!saved&&settings.mode==NETWORK_MODE_AP&&!settings.ap_password[0]);assert(strcmp(settings.ap_ssid,"JSP32-ABCD")==0);
 // Credentials without a mode do not override initial anonymous defaults.
 strcpy(disk_record.sta_ssid,"router");strcpy(disk_record.sta_password,"router-password");disk_strings[2]=disk_strings[3]=true;
 assert(network_store_load(&settings,&saved)==ESP_OK);assert(!saved&&settings.mode==NETWORK_MODE_AP&&!settings.sta_ssid[0]&&!settings.ap_password[0]);memset(disk_strings,0,sizeof(disk_strings));memset(&disk_record,0,sizeof(disk_record));
 network_manager_t manager=make_manager();assert(radio_mode==WIFI_MODE_APSTA);assert(manager.status.active_mode==NETWORK_MODE_AP);
 // AP policy runs APSTA without connecting; scans never restart or change the driver.
 start_sta(&manager);assert(connects==0&&!manager.sta_started);
 int initial_starts=driver_starts,initial_stops=driver_stops,initial_changes=mode_changes;
 scan_count=2;memcpy(scan_records[0].ssid,"weak",4);scan_records[0].rssi=-80;
 memset(scan_records[1].ssid,'s',32);scan_records[1].rssi=-25;scan_records[1].authmode=WIFI_AUTH_WPA2_PSK;
 assert(network_manager_scan(&manager)==ESP_OK);assert(manager.status.scanning);uint32_t blocked_id;assert(network_manager_apply(&manager,&manager.saved,&blocked_id)==ESP_ERR_INVALID_STATE);drain(&manager);
 assert(!manager.status.scanning&&scans==1&&manager.scan_result_count==2);assert(strlen(manager.scan_results[0].ssid)==32&&manager.scan_results[0].rssi==-25);
 assert(radio_mode==WIFI_MODE_APSTA&&manager.status.active_mode==NETWORK_MODE_AP&&!manager.status.mode_saved&&commits==0&&connects==0);
 assert(driver_starts==initial_starts&&driver_stops==initial_stops&&mode_changes==initial_changes);
 scan_start_error=ESP_ERR_TIMEOUT;assert(network_manager_scan(&manager)==ESP_OK);drain(&manager);assert(!manager.status.scanning&&manager.status.last_error==ESP_ERR_TIMEOUT&&radio_mode==WIFI_MODE_APSTA&&connects==0);scan_start_error=ESP_OK;
 scan_read_error=ESP_FAIL;assert(network_manager_scan(&manager)==ESP_OK);drain(&manager);assert(!manager.status.scanning&&manager.status.last_error==ESP_FAIL&&manager.scan_result_count==0&&radio_mode==WIFI_MODE_APSTA&&connects==0);scan_read_error=ESP_OK;scan_count=0;
 start_sta(&manager);assert(connects==0);
 settings=manager.saved;strcpy(settings.ap_ssid,"Private AP");strcpy(settings.ap_password,"ap-password");uint32_t deferred_id;
 assert(network_manager_apply_deferred(&manager,&settings,&deferred_id)==ESP_OK);drain(&manager);now_us+=2000000;handle_timers(&manager);assert(commits==0&&strcmp(manager.saved.ap_ssid,"JSP32-ABCD")==0);
 network_manager_release_apply(&manager);handle_timers(&manager);assert(commits==1&&!manager.status.pending);assert(radio_ap.ap.authmode==WIFI_AUTH_WPA2_PSK);assert(network_store_load(&settings,&saved)==ESP_OK&&saved&&settings.mode==NETWORK_MODE_AP);
 // Separate keys restore prior values if a write fails partway through.
 network_settings_t previous=manager.saved;settings=previous;strcpy(settings.ap_ssid,"new name");strcpy(settings.ap_password,"new-password");
 fail_string_write=string_writes+2;assert(network_store_save(&settings)==ESP_FAIL);fail_string_write=0;
 assert(strcmp(disk_record.ap_ssid,previous.ap_ssid)==0&&strcmp(disk_record.ap_password,previous.ap_password)==0&&disk_record.mode==previous.mode);
 // Strict STA connects after AP has stopped, and does not save untested credentials.
 network_settings_t candidate=manager.saved;candidate.mode=NETWORK_MODE_STA;memset(candidate.sta_ssid,'s',32);candidate.sta_ssid[32]=0;memset(candidate.sta_password,'a',64);candidate.sta_password[64]=0;
 configure(&manager,&candidate);assert(radio_mode==WIFI_MODE_STA&&commits==2&&manager.status.pending);assert(memcmp(radio_sta.sta.ssid,candidate.sta_ssid,32)==0);assert(memcmp(radio_sta.sta.password,candidate.sta_password,64)==0);
 uint32_t id;assert(network_manager_apply(&manager,&candidate,&id)==ESP_ERR_INVALID_STATE);start_sta(&manager);assert(connects==1);
 // Wrong credentials: restore protected AP and retain the prior saved record.
 network_command_t disconnected={.type=NETWORK_COMMAND_DISCONNECTED,.session=manager.session,.data.reason=201};handle_command(&manager,&disconnected);assert(manager.status.fallback&&!manager.status.pending&&radio_mode==WIFI_MODE_APSTA);assert(commits==2&&disk_record.mode==NETWORK_MODE_AP);assert(radio_ap.ap.authmode==WIFI_AUTH_WPA2_PSK);
 // A candidate timeout also recovers AP without committing credentials.
 configure(&manager,&candidate);start_sta(&manager);now_us+=CONFIG_NETWORK_CONNECT_TIMEOUT_MS*1000LL+1;handle_timers(&manager);
 assert(manager.status.fallback&&!manager.status.pending&&manager.status.last_error==ESP_ERR_TIMEOUT&&commits==2&&radio_mode==WIFI_MODE_APSTA);
 // Successful APSTA persists the selected mode and never auto-closes AP.
 candidate=manager.saved;candidate.mode=NETWORK_MODE_APSTA;strcpy(candidate.sta_ssid,"router");strcpy(candidate.sta_password,"router-password");configure(&manager,&candidate);start_sta(&manager);associated=true;handle_got_ip(&manager,station_ip);assert(commits==3&&disk_record.mode==NETWORK_MODE_APSTA&&!manager.status.pending);assert(manager.status.has_ipv4&&radio_mode==WIFI_MODE_APSTA);
 network_command_t lost_ip={.type=NETWORK_COMMAND_LOST_IP,.session=manager.session};handle_command(&manager,&lost_ip);assert(!manager.status.has_ipv4&&manager.status.state==NETWORK_STATE_STA_CONNECTING);handle_got_ip(&manager,station_ip);assert(manager.status.has_ipv4&&commits==3);
 now_us+=120000000;handle_timers(&manager);assert(radio_mode==WIFI_MODE_APSTA);assert(network_manager_scan(&manager)==ESP_OK);assert(network_manager_apply(&manager,&candidate,&id)==ESP_ERR_INVALID_STATE);drain(&manager);assert(!manager.status.scanning&&radio_mode==WIFI_MODE_APSTA&&manager.status.has_ipv4&&commits==3);
 // Storage failure must not report success or overwrite a saved mode.
 commit_result=ESP_FAIL;candidate.mode=NETWORK_MODE_AP;configure(&manager,&candidate);assert(manager.status.fallback&&manager.status.last_error==ESP_FAIL);assert(commits==3&&disk_record.mode==NETWORK_MODE_APSTA);commit_result=ESP_OK;
 // Strict STA resumes after a reboot; fallback does not change that preference.
 candidate=manager.saved;candidate.mode=NETWORK_MODE_STA;configure(&manager,&candidate);start_sta(&manager);associated=true;handle_got_ip(&manager,station_ip);assert(commits==4&&disk_record.mode==NETWORK_MODE_STA);
 // STA can scan without enabling AP or losing its connection/persisted mode.
 int before_scan_connects=connects;assert(network_manager_scan(&manager)==ESP_OK);drain(&manager);assert(!manager.status.scanning&&radio_mode==WIFI_MODE_STA&&manager.status.has_ipv4&&commits==4&&connects==before_scan_connects);

 network_manager_t reboot=make_manager();assert(reboot.status.mode_saved&&reboot.saved.mode==NETWORK_MODE_STA&&radio_mode==WIFI_MODE_STA);start_sta(&reboot);
 for(int i=0;i<=CONFIG_NETWORK_MAX_RETRIES;++i)retry_or_fallback(&reboot,ESP_ERR_TIMEOUT);
 assert(reboot.status.fallback&&reboot.status.active_mode==NETWORK_MODE_AP&&reboot.status.saved_mode==NETWORK_MODE_STA&&disk_record.mode==NETWORK_MODE_STA);
 // AP fallback also keeps a scanner but never starts a STA connection.
 before_scan_connects=connects;start_sta(&reboot);assert(network_manager_scan(&reboot)==ESP_OK);drain(&reboot);assert(radio_mode==WIFI_MODE_APSTA&&reboot.status.active_mode==NETWORK_MODE_AP&&connects==before_scan_connects&&disk_record.mode==NETWORK_MODE_STA);
 // Stale IP events cannot flip a fallback AP back online.
 handle_got_ip(&reboot,station_ip);assert(reboot.status.state==NETWORK_STATE_FALLBACK&&!reboot.status.has_ipv4);
 // Switching back to AP keeps credentials but never connects, even after scans/start events.
 settings=reboot.saved;settings.mode=NETWORK_MODE_AP;configure(&reboot,&settings);before_scan_connects=connects;start_sta(&reboot);
 assert(reboot.status.active_mode==NETWORK_MODE_AP&&radio_mode==WIFI_MODE_APSTA&&!reboot.status.has_ipv4&&disk_record.mode==NETWORK_MODE_AP);
 assert(strcmp(reboot.saved.sta_ssid,"router")==0&&strcmp(reboot.saved.sta_password,"router-password")==0);
 assert(network_manager_scan(&reboot)==ESP_OK);drain(&reboot);now_us+=120000000;handle_timers(&reboot);assert(connects==before_scan_connects&&!reboot.sta_started);
 // Parser retains omitted secrets; invalid changes are rejected.
 settings=reboot.saved;cJSON *json=cJSON_Parse("{\"mode\":\"apsta\",\"ap\":{\"open\":true},\"station\":{\"ssid\":\"router\"}}");assert(parse_settings(json,&settings));assert(!settings.ap_password[0]&&strcmp(settings.sta_password,"router-password")==0);cJSON_Delete(json);
 json=cJSON_Parse("{\"mode\":\"sta\",\"station\":{\"ssid\":\"new-router\"}}");assert(!parse_settings(json,&settings));cJSON_Delete(json);
 json=cJSON_Parse("{\"mode\":\"ap\",\"ap\":{\"open\":false}}");assert(!parse_settings(json,&settings));cJSON_Delete(json);
 // Configuration/status/system info never contain either saved password.
 httpd_req_t request={.user_ctx=&reboot};config_handler(&request);assert(!strstr(request.response,"router-password")&&!strstr(request.response,"ap-password"));assert(strstr(request.response,"passwordSet"));status_handler(&request);assert(!strstr(request.response,"router-password")&&!strstr(request.response,"ap-password"));
 assert(network_http_start(&reboot)==ESP_OK);assert(route_count==10);for(size_t i=0;i<route_count;++i)assert(!strstr(routes[i].uri,"provision"));json=cJSON_CreateObject();cJSON_AddObjectToObject(json,"network");provider(json,provider_context);char *body=cJSON_PrintUnformatted(json);assert(strstr(body,"wifiMaxTxPowerQdbm")&&!strstr(body,"savedMode"));assert(!strstr(body,"router-password")&&!strstr(body,"ap-password"));free(body);cJSON_Delete(json);
 // system/info preserves the original network schema in every connection policy.
 network_status_t previous_status=reboot.status;
 const char *network_keys[]={"station","softAp","wifiMaxTxPowerQdbm","wifiMaxTxPowerDbm"};
 const char *station_keys[]={"ip","netmask","gateway","hostname","ssid","rssi"};
 const char *ap_keys[]={"ip","netmask","gateway","hostname"};
 for(int policy=NETWORK_MODE_AP;policy<=NETWORK_MODE_APSTA;++policy){
  reboot.status.active_mode=(network_mode_t)policy;reboot.status.pending=true;reboot.status.scanning=true;reboot.status.fallback=true;reboot.status.last_error=ESP_FAIL;
  json=cJSON_CreateObject();cJSON *network=cJSON_AddObjectToObject(json,"network");provider(json,provider_context);
  assert(cJSON_GetArraySize(network)==4);for(size_t i=0;i<4;++i)assert(cJSON_GetObjectItemCaseSensitive(network,network_keys[i]));
  cJSON *station=cJSON_GetObjectItemCaseSensitive(network,"station"),*ap=cJSON_GetObjectItemCaseSensitive(network,"softAp");
  assert(cJSON_GetArraySize(station)==6&&cJSON_GetArraySize(ap)==4);
  for(size_t i=0;i<6;++i)assert(cJSON_GetObjectItemCaseSensitive(station,station_keys[i]));
  for(size_t i=0;i<4;++i)assert(cJSON_GetObjectItemCaseSensitive(ap,ap_keys[i]));
  assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(station,"ssid"))&&cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(station,"rssi")));
  cJSON_Delete(json);
 }
 reboot.status=previous_status;
 status_handler(&request);json=cJSON_Parse(request.response);
 assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json,"mode"))&&cJSON_IsString(cJSON_GetObjectItemCaseSensitive(json,"savedMode")));
 assert(cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json,"station"),"connectionEnabled")));cJSON_Delete(json);
 // Invalid HTTP payloads are rejected before a mode change is queued.
 request=(httpd_req_t){.user_ctx=&reboot,.body="{\"mode\":\"invalid\"}"};request.content_len=strlen(request.body);apply_handler(&request);assert(request.status==400&&!reboot.status.pending);
 // Invalid persisted settings return a usable anonymous AP default.
 disk_record.mode=(network_mode_t)99;assert(network_store_load(&settings,&saved)==ESP_ERR_INVALID_STATE);assert(!saved&&settings.mode==NETWORK_MODE_AP&&!settings.ap_password[0]);
 // Missing mode always uses anonymous AP, even with independently configured AP keys.
 disk_present=false;assert(network_store_load(&settings,&saved)==ESP_OK);assert(!saved&&settings.mode==NETWORK_MODE_AP&&!settings.ap_password[0]);
 assert(network_store_save(&settings)==ESP_OK);assert(disk_present);
 // Direct key changes are reflected on the next load.
 assert(nvs_set_str(1,"ap_ssid","direct-name")==ESP_OK);assert(nvs_set_str(1,"ap_pass","direct-pass")==ESP_OK);assert(network_store_load(&settings,&saved)==ESP_OK);assert(saved&&strcmp(settings.ap_ssid,"direct-name")==0&&strcmp(settings.ap_password,"direct-pass")==0);
 // Initial-save failures erase newly-created keys and preserve anonymous defaults.
 disk_present=false;memset(disk_strings,0,sizeof(disk_strings));memset(&disk_record,0,sizeof(disk_record));network_store_defaults(&settings);
 fail_string_write=string_writes+3;assert(network_store_save(&settings)==ESP_FAIL);fail_string_write=0;assert(!disk_present);for(size_t i=0;i<4;++i)assert(!disk_strings[i]);assert(network_store_load(&settings,&saved)==ESP_OK&&!saved&&!settings.ap_password[0]);
 puts("network host tests passed: separate NVS keys, defaults, persistence, three policies, all-mode scanning, AP without STA connection, rollback, HTTP and secret redaction");return 0;
}
