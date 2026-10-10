#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stubs.h"
#include "app_ota.h"

#define BLOCK 65536U
static const esp_partition_t running = {.label="ota_0", .size=4*1024*1024};
static const esp_partition_t next = {.label="ota_1", .size=4*1024*1024};
static app_ota_t *ota;
static size_t committed, total, hashed;
static int writes, fail_write, fail_end;
static bool bad_hash, boot_selected, saw_verify;
static const char *id = "0123456789abcdef0123456789abcdef";

size_t strlcpy(char *dst, const char *src, size_t size)
{ size_t len=strlen(src); if(size){size_t n=len<size-1?len:size-1;memcpy(dst,src,n);dst[n]=0;}return len; }
const char *esp_err_to_name(esp_err_t err) { (void)err;return "MOCK_ERROR"; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) {return 1;}
void vSemaphoreDelete(SemaphoreHandle_t m) {(void)m;}
void xSemaphoreTake(SemaphoreHandle_t m,int wait) {(void)m;(void)wait;}
void xSemaphoreGive(SemaphoreHandle_t m) {(void)m;}
void *heap_caps_malloc(size_t size,int caps) {(void)caps;return malloc(size);}
const esp_partition_t *esp_ota_get_running_partition(void) {return &running;}
const esp_partition_t *esp_ota_get_boot_partition(void) {return &running;}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) {(void)p;return &next;}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,esp_ota_img_states_t *s) {(void)p;*s=2;return ESP_OK;}
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p,esp_app_desc_t *d) {(void)p;memset(d,0,sizeof(*d));strcpy(d->project_name,"jsp32");return ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) {(void)p;boot_selected=true;return ESP_OK;}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) {return ESP_OK;}
esp_err_t esp_ota_mark_app_invalid_rollback(void) {return ESP_OK;}
bool esp_ota_check_rollback_is_possible(void) {return false;}
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t size,esp_ota_handle_t *h) {(void)p;assert(size==OTA_WITH_SEQUENTIAL_WRITES);*h=1;return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void *data,size_t size)
{
    (void)h;assert(size==BLOCK);
    app_ota_status_t status;assert(app_ota_get_status(ota,&status)==ESP_OK);
    // The in-flight erase/write must not have advanced the public counter.
    assert(status.written==committed && status.phase==APP_OTA_PHASE_RECEIVING);
    assert(strcmp(status.upload_id,id)==0);
    ++writes;
    if(writes==fail_write) return ESP_FAIL;
    size_t count=total-committed; if(count>BLOCK) count=BLOCK;
    const unsigned char *bytes=data;
    for(size_t i=0;i<count;++i) assert(bytes[i]==0x42);
    for(size_t i=count;i<size;++i) assert(bytes[i]==0xff);
    committed+=count;
    return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t h) {(void)h;return fail_end?ESP_FAIL:ESP_OK;}
esp_err_t esp_ota_abort(esp_ota_handle_t h) {(void)h;return ESP_OK;}
int psa_crypto_init(void) {return PSA_SUCCESS;}
psa_hash_operation_t psa_hash_operation_init(void) {return 0;}
int psa_hash_setup(psa_hash_operation_t *h,int alg) {(void)h;(void)alg;return PSA_SUCCESS;}
int psa_hash_update(psa_hash_operation_t *h,const void *data,size_t size) {(void)h;(void)data;hashed+=size;return PSA_SUCCESS;}
int psa_hash_finish(psa_hash_operation_t *h,unsigned char *digest,size_t size,size_t *out_size)
{
    (void)h;assert(size==32 && hashed==total);
    app_ota_status_t status;assert(app_ota_get_status(ota,&status)==ESP_OK);
    assert(status.written==total && status.total==total && status.phase==APP_OTA_PHASE_VERIFYING);
    saw_verify=true;memset(digest,bad_hash?1:0,32);*out_size=32;return PSA_SUCCESS;
}
int psa_hash_abort(psa_hash_operation_t *h) {(void)h;return PSA_SUCCESS;}

static void run(size_t image_size,int failing_write,bool invalid_hash,bool invalid_image)
{
    total=image_size;committed=hashed=0;writes=0;fail_write=failing_write;
    bad_hash=invalid_hash;fail_end=invalid_image;boot_selected=saw_verify=false;
    unsigned char *bytes=malloc(total);assert(bytes);memset(bytes,0x42,total);
    app_ota_upload_t *upload=NULL;
    assert(app_ota_upload_begin(ota,total,"0000000000000000000000000000000000000000000000000000000000000000",id,&upload)==ESP_OK);
    size_t initial=total<4096?total:4096;
    assert(app_ota_upload_write(upload,bytes,initial)==ESP_OK);
    app_ota_status_t status;app_ota_get_status(ota,&status);
    assert(status.received==initial && status.written==0); // buffered only
    esp_err_t err=app_ota_upload_write(upload,bytes+initial,total-initial);
    if(err!=ESP_OK) {
        app_ota_get_status(ota,&status);assert(status.written==committed);
        app_ota_upload_abort(upload,err);
    } else {
        app_ota_get_status(ota,&status);
        assert(status.received==total && status.written==(total/BLOCK)*BLOCK);
        char digest[65];app_ota_partition_info_t partition;
        err=app_ota_upload_finish(upload,digest,&partition);
    }
    assert((err==ESP_OK)==(!failing_write&&!invalid_hash&&!invalid_image));
    assert(boot_selected==(err==ESP_OK));
    if(!failing_write) assert(saw_verify);
    app_ota_get_status(ota,&status);
    assert(status.phase==APP_OTA_PHASE_IDLE && status.written==0 && status.upload_id[0]==0);
    free(bytes);
}

int main(void)
{
    assert(app_ota_init(&ota)==ESP_OK);
    run(10,0,false,false);run(BLOCK,0,false,false);run(2*BLOCK+7,0,false,false);
    run(2*BLOCK+7,1,false,false);run(2*BLOCK+7,2,false,false);run(2*BLOCK+7,3,false,false);
    run(BLOCK+7,0,true,false);run(BLOCK+7,0,false,true);
    puts("OTA host tests passed: buffered bytes, block completion, padding, write failures and validation failures.");
}
