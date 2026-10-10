#include "mqtt_internal.h"
#include "js_telemetry.h"
#include "stubs.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct app_registers {uint16_t d[2048];};
static uint64_t clock_ms=100;
static unsigned reads;
static app_register_event_fn subscriber;
static void *subscriber_opaque;
static void (*worker)(void *);
static void *worker_opaque;
static jmp_buf worker_exit;
static js_runtime_module_init_fn js_init;
static js_runtime_module_deinit_fn js_deinit;
static void *js_opaque;

SemaphoreHandle_t xSemaphoreCreateMutex(void){return malloc(1);}
int xSemaphoreTake(SemaphoreHandle_t lock,unsigned wait){(void)lock;(void)wait;return 1;}
int xSemaphoreGive(SemaphoreHandle_t lock){(void)lock;return 1;}
void vSemaphoreDelete(SemaphoreHandle_t lock){free(lock);}
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle)
{(void)name;(void)stack;(void)priority;(void)handle;worker=fn;worker_opaque=arg;return pdPASS;}
void vTaskDelay(unsigned ms){assert(ms==10);longjmp(worker_exit,1);}
int64_t esp_timer_get_time(void){return clock_ms*1000;}
esp_err_t esp_read_mac(uint8_t *mac,int type){assert(type==ESP_MAC_WIFI_STA);memcpy(mac,"\xaa\xbb\xcc\xdd\xee\xff",6);return ESP_OK;}
esp_err_t esp_crt_bundle_attach(void *ctx){(void)ctx;return ESP_OK;}
static void tick(uint64_t now){clock_ms=now;if(!setjmp(worker_exit))worker(worker_opaque);}

size_t app_registers_area_count(app_register_area_t area){return area==APP_REGISTER_AREA_D?2048:256;}
bool app_register_area_is_bit(app_register_area_t area){return area!=APP_REGISTER_AREA_D;}
esp_err_t app_register_address_parse(const char *s,app_register_address_t *a)
{
    if(!s || !s[0] || !s[1])return ESP_ERR_INVALID_ARG;
    const char *areas="XYMD",*p=strchr(areas,s[0]);char *end;
    long index=strtol(s+1,&end,10);
    if(!p || *end || index<0 || index>65535)return ESP_ERR_INVALID_ARG;
    a->area=p-areas;a->index=index;return ESP_OK;
}
esp_err_t app_registers_read_many(app_registers_t *r,const app_register_address_t *a,uint16_t *v,size_t count,uint64_t *revision)
{(void)revision;reads++;for(size_t i=0;i<count;i++)v[i]=r->d[a[i].index];return ESP_OK;}
esp_err_t app_registers_subscribe(app_registers_t *r,app_register_event_fn fn,void *opaque,uint32_t *id)
{(void)r;subscriber=fn;subscriber_opaque=opaque;*id=1;return ESP_OK;}
bool app_registers_unsubscribe(app_registers_t *r,uint32_t id){(void)r;(void)id;subscriber=NULL;return true;}
static void changed(uint16_t index)
{
    app_register_change_t change={.address={APP_REGISTER_AREA_D,index}};
    app_register_event_t event={.changes=&change,.change_count=1};
    subscriber(&event,subscriber_opaque);
}

typedef struct {char key[16],value[257];bool byte;} nvs_entry_t;
static nvs_entry_t entries[12];static size_t entry_count;
static nvs_entry_t *entry(const char *key,bool create)
{
    assert(strlen(key)<=15);
    for(size_t i=0;i<entry_count;i++)if(!strcmp(entries[i].key,key))return &entries[i];
    if(!create)return NULL;
    assert(entry_count<12);nvs_entry_t *e=&entries[entry_count++];strcpy(e->key,key);return e;
}
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *handle)
{assert(!strcmp(ns,"jsp32_config"));(void)mode;*handle=1;return ESP_OK;}
void nvs_close(nvs_handle_t handle){(void)handle;}
esp_err_t nvs_get_u8(nvs_handle_t handle,const char *key,uint8_t *value)
{(void)handle;nvs_entry_t *e=entry(key,false);if(!e)return ESP_ERR_NVS_NOT_FOUND;assert(e->byte);*value=e->value[0];return ESP_OK;}
esp_err_t nvs_set_u8(nvs_handle_t handle,const char *key,uint8_t value)
{(void)handle;nvs_entry_t *e=entry(key,true);e->byte=true;e->value[0]=value;return ESP_OK;}
esp_err_t nvs_get_str(nvs_handle_t handle,const char *key,char *value,size_t *size)
{
    (void)handle;nvs_entry_t *e=entry(key,false);if(!e)return ESP_ERR_NVS_NOT_FOUND;
    assert(!e->byte);if(*size<strlen(e->value)+1)return ESP_ERR_NVS_INVALID_LENGTH;
    *size=strlen(e->value)+1;strcpy(value,e->value);return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle,const char *key,const char *value)
{(void)handle;nvs_entry_t *e=entry(key,true);e->byte=false;assert(strlen(value)<sizeof(e->value));strcpy(e->value,value);return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t handle){(void)handle;return ESP_OK;}

struct fake_client {
    void (*callback)(void *,esp_event_base_t,int32_t,void *);void *opaque;
    char client_id[32],url[257],payload[8193],topic[257];
    int accepted,attempted;bool full;
};
static struct fake_client clients[1];static unsigned client_count;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *config)
{
    assert(client_count<1 && config->outbox.limit==32768 && config->broker.verification.crt_bundle_attach);
    struct fake_client *client=&clients[client_count++];
    strcpy(client->client_id,config->credentials.client_id);strcpy(client->url,config->broker.address.uri);return client;
}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c,int id,void (*fn)(void *,esp_event_base_t,int32_t,void *),void *opaque)
{assert(id==ESP_EVENT_ANY_ID);c->callback=fn;c->opaque=opaque;return ESP_OK;}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c){(void)c;return ESP_OK;}
esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c){(void)c;return ESP_OK;}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t c,const char *topic,const char *data,int len,int qos,int retain,bool store)
{
    assert(len==0 && qos==0 && !retain && store);c->attempted++;
    if(c->full)return -2;
    strcpy(c->topic,topic);strcpy(c->payload,data);return ++c->accepted;
}
static void event(int id){clients[0].callback(clients[0].opaque,"mqtt",id,NULL);}

static httpd_uri_t routes[3];
esp_err_t app_http_server_register(app_http_server_t *s,const char *owner,const httpd_uri_t *handlers,size_t count)
{(void)s;assert(!strcmp(owner,"mqtt") && count==3);memcpy(routes,handlers,sizeof(routes));return ESP_OK;}
esp_err_t app_http_server_unregister_owner(app_http_server_t *s,const char *owner){(void)s;(void)owner;return ESP_OK;}
esp_err_t httpd_resp_set_type(httpd_req_t *r,const char *type){(void)r;assert(!strcmp(type,"application/json"));return ESP_OK;}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *name,const char *value){(void)r;(void)name;(void)value;return ESP_OK;}
esp_err_t httpd_resp_sendstr(httpd_req_t *r,const char *s){r->response=strdup(s);return ESP_OK;}
esp_err_t httpd_resp_send_err(httpd_req_t *r,int code,const char *s){r->error=code;return httpd_resp_sendstr(r,s);}
int httpd_req_recv(httpd_req_t *r,char *out,size_t size)
{size_t n=size>7?7:size;memcpy(out,r->body+r->cursor,n);r->cursor+=n;return n;}
static httpd_req_t request(unsigned route,const char *body)
{
    httpd_req_t req={.user_ctx=routes[route].user_ctx,.body=body,.content_len=body?strlen(body):0};
    assert(routes[route].handler(&req)==ESP_OK);return req;
}
esp_err_t js_runtime_register_module(js_runtime_module_init_fn init,js_runtime_module_deinit_fn deinit,void *opaque)
{js_init=init;js_deinit=deinit;js_opaque=opaque;return ESP_OK;}

int main(void)
{
    mqtt_broker_config_t config;assert(mqtt_config_load(&config)==ESP_OK);
    assert(!config.enabled && !config.url[0]);
    config.enabled=true;strcpy(config.url,"mqtt://broker:1883");strcpy(config.password,"hidden");
    assert(mqtt_config_save(&config)==ESP_OK && entry_count==4);
    assert(entry("mqtt_enabled",false) && entry("mqtt_url",false) &&
           entry("mqtt_username",false) && entry("mqtt_password",false));
    struct app_registers registers={.d={[0]=42}};app_mqtt_t *service=NULL;
    assert(app_mqtt_start(&registers,(app_http_server_t *)1,&service)==ESP_OK && service && client_count==1);
    assert(!strcmp(clients[0].client_id,"jsp32-AABBCCDDEEFF"));
    char error[160];const char *definition="{\"mode\":\"change\",\"fields\":{\"v\":{\"address\":\"D0\",\"type\":\"uint16\"},\"runtime\":{\"address\":\"D1\",\"type\":\"uint16\",\"trigger\":false}}}";
    assert(app_mqtt_register_report(service,"io",definition,error,sizeof(error))==ESP_OK);
    assert(app_mqtt_register_report(service,"io",definition,error,sizeof(error))==ESP_ERR_INVALID_STATE);
    event(MQTT_EVENT_CONNECTED);
    clients[0].full=true;tick(100);
    assert(clients[0].accepted==0 && reads==1);
    clients[0].full=false;registers.d[0]=43;changed(0);tick(110);
    assert(clients[0].accepted==0 && reads==1); // retry backoff
    tick(1100);assert(clients[0].accepted==1 && reads==2);
    assert(!strcmp(clients[0].topic,"/jsp32/AABBCCDDEEFF/io"));
    assert(strstr(clients[0].payload,"43")); // latest snapshot, not the failed payload
    changed(100);tick(1400);assert(clients[0].accepted==1);
    registers.d[1]=7;changed(1);tick(1450);assert(clients[0].accepted==1);
    event(MQTT_EVENT_DISCONNECTED);tick(1500);
    event(MQTT_EVENT_CONNECTED);tick(1510);assert(clients[0].accepted==2);
    assert(strstr(clients[0].payload,"\"runtime\":7"));
    registers.d[0]=44;changed(0);tick(1550);assert(clients[0].accepted==2);
    tick(1710);assert(clients[0].accepted==3);
    httpd_req_t req=request(0,NULL);assert(!req.error && !strstr(req.response,"hidden") && !strstr(req.response,"brokers"));free(req.response);
    req=request(1,"{\"enabled\":true,\"url\":\"mqtt://new\",\"username\":\"user\"}");
    assert(!req.error);free(req.response);assert(mqtt_config_load(&config)==ESP_OK);
    assert(!strcmp(config.password,"hidden"));
    assert(!strcmp(service->broker.config.url,"mqtt://broker:1883")); // reboot required
    const char *invalid_config[]={
        "{\"enabled\":true,\"url\":\"http://wrong\",\"username\":\"\"}",
        "{\"enabled\":true,\"url\":\"mqtt://user:secret@broker\",\"username\":\"\"}",
        "{\"enabled\":1,\"url\":\"mqtt://broker\",\"username\":\"\"}",
        "{\"brokers\":[]}","{}", "[]",
    };
    for(size_t i=0;i<sizeof(invalid_config)/sizeof(invalid_config[0]);i++) {
        req=request(1,invalid_config[i]);assert(req.error==400);free(req.response);
    }
    assert(mqtt_config_load(&config)==ESP_OK && !strcmp(config.url,"mqtt://new"));
    req=request(1,"{\"enabled\":true,\"url\":\"mqtts://secure\",\"username\":\"\",\"password\":\"\"}");
    assert(!req.error);free(req.response);assert(mqtt_config_load(&config)==ESP_OK && !config.password[0]);
    req=request(2,NULL);assert(strstr(req.response,"io") && !strstr(req.response,"hidden") && !strstr(req.response,"brokers"));free(req.response);

    assert(js_telemetry_register(service)==ESP_OK);
    JSRuntime *rt=JS_NewRuntime();JSContext *ctx=JS_NewContext(rt);assert(js_init(NULL,ctx,js_opaque)==ESP_OK);
    const char *script="telemetry.validate('io',{fields:{last:{address:'D2047',type:'uint16'}}});"
        "telemetry.register('script',{fields:{last:{address:'D2047',type:'uint16'}}});"
        "if(!telemetry.unregister('script'))throw Error('unregister failed');"
        "telemetry.register('script',{fields:{last:{address:'D2047',type:'uint16'}}});";
    JSValue result=JS_Eval(ctx,script,strlen(script),"test",JS_EVAL_TYPE_GLOBAL);
    assert(!JS_IsException(result));JS_FreeValue(ctx,result);
    unsigned report_count=0;for(mqtt_report_t *r=service->reports;r;r=r->next)report_count++;
    assert(report_count==2); // validation never replaces or registers a report
    const char *invalid="telemetry.register('bad',{fields:{last:{address:'D2047',type:'uint32'}}})";
    result=JS_Eval(ctx,invalid,strlen(invalid),"test",JS_EVAL_TYPE_GLOBAL);
    assert(JS_IsException(result));JS_FreeValue(ctx,JS_GetException(ctx));
    js_deinit(NULL,ctx,js_opaque);assert(!service->reports);
    JS_FreeContext(ctx);JS_FreeRuntime(rt);
    unsigned before=clients[0].accepted;tick(10000);assert(clients[0].accepted==before);
    assert(!app_mqtt_unregister_report(service,"missing"));
    for(unsigned i=0;i<APP_MQTT_MAX_REPORTS;i++) {
        char id[33];snprintf(id,sizeof(id),"report%u",i);
        assert(app_mqtt_register_report(service,id,definition,error,sizeof(error))==ESP_OK);
    }
    assert(app_mqtt_register_report(service,"overflow",definition,error,sizeof(error))==ESP_ERR_NO_MEM);
    app_mqtt_clear_reports(service);
    puts("MQTT service tests passed: single broker, queue backoff, reconnect, NVS/HTTP secrets, JS teardown.");
}
