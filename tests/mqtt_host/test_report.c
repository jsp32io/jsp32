#include "mqtt_report.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "io_telemetry.h"

struct app_registers { uint16_t d[2048]; bool x; };
static unsigned reads;
size_t app_registers_area_count(app_register_area_t area) { return area == APP_REGISTER_AREA_D ? 2048 : 256; }
bool app_register_area_is_bit(app_register_area_t area) { return area != APP_REGISTER_AREA_D; }
esp_err_t app_register_address_parse(const char *s, app_register_address_t *a)
{
    if (!s || !s[0] || !s[1] || (s[1] == '0' && s[2])) return ESP_ERR_INVALID_ARG;
    const char *areas = "XYMD";const char *p = strchr(areas, s[0]);
    if (!p) return ESP_ERR_INVALID_ARG;
    char *end;long index = strtol(s + 1, &end, 10);
    if (*end || index < 0 || index > 65535) return ESP_ERR_INVALID_ARG;
    a->area = p - areas;a->index = index;return ESP_OK;
}
esp_err_t app_registers_read_many(app_registers_t *r,const app_register_address_t *a,
                                  uint16_t *values,size_t count,uint64_t *revision)
{
    (void)revision;reads++;
    for (size_t i=0;i<count;i++) values[i]=a[i].area==APP_REGISTER_AREA_D?r->d[a[i].index]:r->x;
    return ESP_OK;
}
static mqtt_report_t *parse(const char *json)
{
    mqtt_report_t *r=NULL;char error[160];
    esp_err_t err=mqtt_report_parse("io","AABBCCDDEEFF",json,&r,error,sizeof(error));
    if(err!=ESP_OK)fprintf(stderr,"%s\n",error);
    assert(err==ESP_OK && r);return r;
}
static void reject(const char *json)
{
    mqtt_report_t *r=NULL;char error[160];
    assert(mqtt_report_parse("io","AABBCCDDEEFF",json,&r,error,sizeof(error))!=ESP_OK && !r);
}
int main(void)
{
    struct app_registers store={.d={[0]=0xffff,[1]=0xff9c,[2]=0xff9c,[3]=0xffff,
        [4]=0x3fc0,[5]=0,[2047]=65535},.x=true};
    mqtt_report_t *r=parse("{\"fields\":{\"run\":{\"address\":\"X0\",\"type\":\"boolean\"},"
        "\"length\":{\"address\":\"D0\",\"type\":\"int32\",\"scale\":0.01},"
        "\"reverse\":{\"address\":\"D2\",\"type\":\"int32\",\"wordOrder\":\"lowFirst\"},"
        "\"float\":{\"address\":\"D4\",\"type\":\"float32\",\"offset\":2},"
        "\"last\":{\"address\":\"D2047\",\"type\":\"uint16\"},"
        "\"signed\":{\"address\":\"D1\",\"type\":\"int16\"},"
        "\"unsigned\":{\"address\":\"D0\",\"type\":\"uint32\"}}}");
    assert(!strcmp(r->topic,"/jsp32/AABBCCDDEEFF/io"));
    char *encoded=mqtt_report_encode(r,&store,1234);assert(encoded && reads==1);
    cJSON *root=cJSON_Parse(encoded),*data=cJSON_GetObjectItem(root,"data");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(data,"run")));
    assert(cJSON_GetObjectItem(data,"length")->valuedouble==-1);
    assert(cJSON_GetObjectItem(data,"reverse")->valuedouble==-100);
    assert(cJSON_GetObjectItem(data,"float")->valuedouble==3.5);
    assert(cJSON_GetObjectItem(data,"last")->valuedouble==65535);
    assert(cJSON_GetObjectItem(data,"signed")->valuedouble==-100);
    assert(cJSON_GetObjectItem(data,"unsigned")->valuedouble==4294967196.0);
    assert(cJSON_GetObjectItem(root,"uptimeMs")->valuedouble==1234);
    cJSON_Delete(root);cJSON_free(encoded);
    store.d[4]=0x7f80;
    encoded=mqtt_report_encode(r,&store,1);root=cJSON_Parse(encoded);
    assert(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(root,"data"),"float")));
    cJSON_Delete(root);cJSON_free(encoded);free(r);

    const char *field="\"fields\":{\"v\":{\"address\":\"D2046\",\"type\":\"uint32\"}}";
    char json[1024];snprintf(json,sizeof(json),"{\"topic\":\"/custom\",\"mode\":\"change\",\"minIntervalMs\":200,\"heartbeatMs\":1000,%s}",field);
    r=parse(json);assert(!strcmp(r->topic,"/custom"));
    assert(mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,2047}));
    assert(!mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,2045}));
    assert(mqtt_report_due(r,100,true));mqtt_report_sent(r,100);
    assert(!mqtt_report_due(r,150,true));
    r->pending=true;
    assert(!mqtt_report_due(r,250,true)); // merge changes inside minimum interval
    assert(mqtt_report_due(r,300,true));mqtt_report_sent(r,300);
    assert(!mqtt_report_due(r,1300,false)); // offline
    assert(mqtt_report_due(r,1300,true)); // heartbeat
    free(r);
    snprintf(json,sizeof(json),"{\"intervalMs\":500,%s}",field);r=parse(json);
    assert(mqtt_report_due(r,100,true));mqtt_report_sent(r,100);
    assert(!mqtt_report_due(r,200,true));
    assert(!mqtt_report_due(r,600,false));
    assert(mqtt_report_due(r,700,true)); // reconnect sends pending snapshot
    free(r);
    const char *prefixes[]={"\"brokers\":[1]","\"mode\":\"bad\"","\"qos\":3","\"retain\":1",
        "\"topic\":\"\"","\"topic\":\"/a/+\"","\"intervalMs\":0",
        "\"minIntervalMs\":100","\"extra\":true","\"qos\":0,\"qos\":1"};
    for(size_t i=0;i<sizeof(prefixes)/sizeof(prefixes[0]);i++) {
        snprintf(json,sizeof(json),"{%s,%s}",prefixes[i],field);reject(json);
    }
    reject("{\"fields\":{\"v\":{\"address\":\"D2047\",\"type\":\"uint32\"}}}");
    reject("{\"fields\":{\"v\":{\"address\":\"X0\",\"type\":\"uint16\"}}}");
    reject("{\"fields\":{\"v\":{\"address\":\"D0\",\"type\":\"boolean\"}}}");
    reject("{\"fields\":{\"v\":{\"address\":\"D0\",\"type\":\"int16\",\"wordOrder\":\"lowFirst\"}}}");
    reject("{\"fields\":{\"v\":{\"address\":\"X0\",\"type\":\"boolean\",\"scale\":1}}}");
    reject("[]");reject("{} trailing");
    reject("{\"fields\":{\"v\":{\"address\":\"D0\",\"type\":\"uint16\",\"disabled\":1}}}");
    reject("{\"fields\":{\"v\":{\"address\":\"D0\",\"type\":\"uint16\",\"disabled\":true}}}");
    reject("{\"fields\":{\"v\":{\"name\":1,\"address\":\"D0\",\"type\":\"uint16\"}}}");
    reject("{\"fields\":{\"v\":{\"name\":\"\",\"address\":\"D0\",\"type\":\"uint16\"}}}");
    reject("{\"fields\":{\"v\":{\"address\":\"D0\",\"type\":\"uint16\",\"trigger\":1}}}");
    r=parse(IO_TELEMETRY_JSON);assert(r->field_count==35);
    assert(r->change_mode && r->min_interval_ms==200 && r->heartbeat_ms==30000);
    assert(mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,108}));
    assert(!mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,140}));
    assert(!mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,141}));
    assert(!mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,148}));
    assert(!mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,156}));
    assert(!mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,159}));
    assert(mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,161}));
    assert(mqtt_report_due(r,100,true));mqtt_report_sent(r,100);
    assert(!mqtt_report_due(r,30099,true));assert(mqtt_report_due(r,30100,true));
    store.d[109]=15;store.d[132]=18;store.d[133]=54919; // 1,234,567 microHz
    store.d[156]=0xffff;store.d[157]=0xffc9; // -55 hundredths of a millimetre
    encoded=mqtt_report_encode(r,&store,1500);assert(encoded);root=cJSON_Parse(encoded);
    data=cJSON_GetObjectItem(root,"data");
    assert(cJSON_GetObjectItem(data,"x1RisingCount")->valuedouble==15);
    assert(!cJSON_HasObjectItem(data,"X1 电平"));
    assert(fabs(cJSON_GetObjectItem(data,"x1SpeedHz")->valuedouble-1.234567)<1e-9);
    assert(fabs(cJSON_GetObjectItem(data,"encoderPositionMm")->valuedouble+0.55)<1e-9);
    cJSON_Delete(root);cJSON_free(encoded);free(r);
    root=cJSON_Parse(IO_TELEMETRY_JSON);cJSON *io_fields=cJSON_GetObjectItem(root,"fields");
    cJSON_DeleteItemFromObjectCaseSensitive(io_fields,"x1RisingCount");
    char *modified=cJSON_PrintUnformatted(root);r=parse(modified);cJSON_free(modified);assert(r->field_count==34);
    assert(!mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,108}));
    encoded=mqtt_report_encode(r,&store,1500);data=cJSON_Parse(encoded);
    assert(!cJSON_HasObjectItem(cJSON_GetObjectItem(data,"data"),"x1RisingCount"));
    cJSON_Delete(data);cJSON_free(encoded);free(r);
    r=parse("{\"fields\":{}}");assert(r->field_count==0);
    assert(!mqtt_report_due(r,1000,true));free(r);cJSON_Delete(root);
    r=parse("{\"mode\":\"change\",\"heartbeatMs\":30000,\"fields\":{\"length\":{\"address\":\"D156\",\"type\":\"uint32\",\"trigger\":false}}}");
    assert(!mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,156}));
    assert(!mqtt_report_matches(r,(app_register_address_t){APP_REGISTER_AREA_D,157}));
    assert(mqtt_report_due(r,100,true));mqtt_report_sent(r,100);
    assert(!mqtt_report_due(r,30099,true));assert(mqtt_report_due(r,30100,true));
    encoded=mqtt_report_encode(r,&store,30100);assert(encoded && strstr(encoded,"length"));cJSON_free(encoded);free(r);
    cJSON *many=cJSON_CreateObject(),*fields=cJSON_AddObjectToObject(many,"fields");
    for(unsigned i=0;i<65;i++) {
        char name[16];snprintf(name,sizeof(name),"field%u",i);
        cJSON *field_item=cJSON_AddObjectToObject(fields,name);
        cJSON_AddStringToObject(field_item,"address","D0");
        cJSON_AddStringToObject(field_item,"type","uint16");
        if(i==63) {char *json64=cJSON_PrintUnformatted(many);r=parse(json64);free(r);cJSON_free(json64);}
    }
    char *json65=cJSON_PrintUnformatted(many);reject(json65);cJSON_free(json65);cJSON_Delete(many);
    puts("MQTT report tests passed: types, snapshots, bounds, modes, reconnect scheduling.");
}
