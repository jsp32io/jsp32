#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "stubs.h"
#include "js_net.h"
#include "js_abort.h"

#undef getaddrinfo
#undef send

struct host_queue {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned capacity, item_size, count, start;
    uint8_t *data;
};

QueueHandle_t xQueueCreate(unsigned count, unsigned size)
{
    QueueHandle_t queue = calloc(1, sizeof(*queue));
    assert(queue);
    queue->capacity = count;
    queue->item_size = size;
    queue->data = calloc(count, size);
    assert(queue->data);
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->changed, NULL);
    return queue;
}
int xQueueSend(QueueHandle_t queue, const void *data, TickType_t wait)
{
    pthread_mutex_lock(&queue->lock);
    while (queue->count == queue->capacity && wait == portMAX_DELAY)
        pthread_cond_wait(&queue->changed, &queue->lock);
    bool sent = queue->count < queue->capacity;
    if (sent) {
        unsigned index = (queue->start + queue->count++) % queue->capacity;
        memcpy(queue->data + index * queue->item_size, data, queue->item_size);
        pthread_cond_broadcast(&queue->changed);
    }
    pthread_mutex_unlock(&queue->lock);
    return sent;
}
int xQueueReceive(QueueHandle_t queue, void *data, TickType_t wait)
{
    pthread_mutex_lock(&queue->lock);
    while (!queue->count && wait == portMAX_DELAY)
        pthread_cond_wait(&queue->changed, &queue->lock);
    bool received = queue->count != 0;
    if (received) {
        memcpy(data, queue->data + queue->start * queue->item_size, queue->item_size);
        queue->start = (queue->start + 1) % queue->capacity;
        --queue->count;
        pthread_cond_broadcast(&queue->changed);
    }
    pthread_mutex_unlock(&queue->lock);
    return received;
}
unsigned uxQueueMessagesWaiting(QueueHandle_t queue)
{
    pthread_mutex_lock(&queue->lock);
    unsigned count = queue->count;
    pthread_mutex_unlock(&queue->lock);
    return count;
}
void vQueueDelete(QueueHandle_t queue)
{
    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->changed);
    free(queue->data);
    free(queue);
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return xQueueCreate(1, 1); }
int xSemaphoreGive(SemaphoreHandle_t semaphore) { char value = 1; return xQueueSend(semaphore, &value, 0); }
int xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait) { char value; return xQueueReceive(semaphore, &value, wait); }
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { vQueueDelete(semaphore); }
typedef struct { void (*entry)(void *); void *argument; } host_task_t;
static void *task_start(void *opaque)
{
    host_task_t task = *(host_task_t *)opaque;
    free(opaque);
    task.entry(task.argument);
    return NULL;
}
int xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                 void *argument, unsigned priority, void *out)
{
    host_task_t *task = malloc(sizeof(*task));
    *task = (host_task_t){entry, argument};
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, task_start, task));
    pthread_detach(thread);
    return pdPASS;
}
void vTaskDelete(void *task) { pthread_exit(NULL); }
int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static js_runtime_module_init_fn module_init;
static js_runtime_module_deinit_fn module_deinit;
static void *module_opaque;
static js_event_handler_fn event_handler;
static void *event_opaque;
static atomic_int dropped_wakes;
static atomic_int posted_wakes;
static atomic_bool dns_running;
static atomic_int send_mode;
static atomic_bool send_pause;
static QueueHandle_t events;

esp_err_t js_runtime_register_module(js_runtime_module_init_fn init,
                                      js_runtime_module_deinit_fn deinit, void *opaque)
{ module_init = init; module_deinit = deinit; module_opaque = opaque; return ESP_OK; }
JSContext *js_runtime_context(js_runtime_t *runtime) { return runtime->ctx; }
esp_err_t js_runtime_track_requests(js_runtime_t *runtime, js_request_table_t *table)
{ assert(table->ctx == runtime->ctx); return ESP_OK; }
esp_err_t js_event_register_handler(js_runtime_t *runtime, uint16_t type,
                                     js_event_handler_fn handler, void *opaque)
{ assert(type == JS_EVENT_NET); event_handler = handler; event_opaque = opaque; return ESP_OK; }
esp_err_t js_event_unregister_handler(js_runtime_t *runtime, uint16_t type)
{ event_handler = NULL; return ESP_OK; }
esp_err_t js_event_post(js_runtime_t *runtime, const js_event_t *event, TickType_t wait)
{
    assert(wait == 0);
    int remaining = atomic_load(&dropped_wakes);
    if (remaining > 0) { atomic_fetch_sub(&dropped_wakes, 1); return ESP_FAIL; }
    atomic_fetch_add(&posted_wakes, 1);
    return xQueueSend(events, event, 0) ? ESP_OK : ESP_FAIL;
}
int host_getaddrinfo(const char *host, const char *service, const struct addrinfo *hints,
                      struct addrinfo **result)
{
    if (!strcmp(host, "slow.test")) {
        atomic_store(&dns_running, true);
        usleep(300000);
        atomic_store(&dns_running, false);
        return getaddrinfo("127.0.0.1", service, hints, result);
    }
    if (!strcmp(host, "missing.test")) return EAI_NONAME;
    if (!strcmp(host, "local.test") || !strcmp(host, "service.test"))
        return getaddrinfo("127.0.0.1", service, hints, result);
    return getaddrinfo(host, service, hints, result);
}

ssize_t host_send(int fd, const void *bytes, size_t size, int flags)
{
    int mode = atomic_load(&send_mode);
    if (mode == 2) { errno = EAGAIN; return -1; }
    if (mode == 1 && atomic_exchange(&send_pause, false)) { errno = EAGAIN; return -1; }
    if (mode && size > 2) size = 2;
    ssize_t result = send(fd, bytes, size, flags);
    if (mode == 1 && result > 0) atomic_store(&send_pause, true);
    if (mode == 3 && result > 0) atomic_store(&send_mode, 2);
    return result;
}

static JSValue set_send_mode(JSContext *ctx, JSValueConst object, int argc, JSValueConst *argv)
{
    int32_t mode = 0;
    if (argc) JS_ToInt32(ctx, &mode, argv[0]);
    atomic_store(&send_mode, mode);
    atomic_store(&send_pause, false);
    return JS_UNDEFINED;
}

static JSValue drop_wakes(JSContext *ctx, JSValueConst object, int argc, JSValueConst *argv)
{ atomic_store(&dropped_wakes, 5); return JS_UNDEFINED; }
static JSValue check(JSContext *ctx, JSValueConst object, int argc, JSValueConst *argv)
{
    if (argc && JS_ToBool(ctx, argv[0])) return JS_UNDEFINED;
    return JS_ThrowInternalError(ctx, "assertion failed");
}

static void dump_error(JSContext *ctx, JSValueConst error)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue stage = JS_GetPropertyStr(ctx, global, "testStage");
    const char *stage_name = JS_ToCString(ctx, stage);
    fprintf(stderr, "stage: %s\n", stage_name ? stage_name : "unknown");
    JS_FreeCString(ctx, stage_name);
    JS_FreeValue(ctx, stage);
    JS_FreeValue(ctx, global);
    JSValue number = JS_GetPropertyStr(ctx, error, "errno");
    int32_t system_error = 0;
    JS_ToInt32(ctx, &system_error, number);
    JS_FreeValue(ctx, number);
    number = JS_GetPropertyStr(ctx, error, "tlsCode");
    int32_t tls_error = 0;
    JS_ToInt32(ctx, &tls_error, number);
    JS_FreeValue(ctx, number);
    JSValue stack = JS_GetPropertyStr(ctx, error, "stack");
    const char *message = JS_ToCString(ctx, error);
    const char *trace = JS_ToCString(ctx, stack);
    fprintf(stderr, "%s (errno=%d: %s, tlsCode=%d)\n%s\n", message ? message : "exception",
            system_error, strerror(system_error), tls_error, trace ? trace : "");
    JS_FreeCString(ctx, message);
    JS_FreeCString(ctx, trace);
    JS_FreeValue(ctx, stack);
}

static void drain_jobs(JSRuntime *runtime)
{
    JSContext *ctx;
    int count = 0, result;
    while ((result = JS_ExecutePendingJob(runtime, &ctx)) != 0 && ++count < 256) {
        if (result < 0) {
            JSValue error = JS_GetException(ctx);
            dump_error(ctx, error);
            abort();
        }
    }
}

static void run(js_runtime_t *runtime, const char *source)
{
    JSContext *ctx = runtime->ctx;
    JSValue promise = JS_Eval(ctx, source, strlen(source), "net-test", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(promise)) {
        JSValue error = JS_GetException(ctx);
        dump_error(ctx, error);
        abort();
    }
    int64_t deadline = esp_timer_get_time() + 10000000;
    while (JS_PromiseState(ctx, promise) == JS_PROMISE_PENDING) {
        js_event_t event;
        if (xQueueReceive(events, &event, 0)) event_handler(runtime, &event, event_opaque);
        drain_jobs(JS_GetRuntime(ctx));
        assert(esp_timer_get_time() < deadline);
        usleep(1000);
    }
    if (JS_PromiseState(ctx, promise) != JS_PROMISE_FULFILLED) {
        JSValue error = JS_PromiseResult(ctx, promise);
        dump_error(ctx, error);
        abort();
    }
    JS_FreeValue(ctx, promise);
    puts("scenario passed");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    events = xQueueCreate(1, sizeof(js_event_t));
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    assert(js_abort_init(ctx) == 0);
    js_runtime_t runtime = {.ctx = ctx};
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "check", JS_NewCFunction(ctx, check, "check", 1));
    JS_SetPropertyStr(ctx, global, "dropWakes", JS_NewCFunction(ctx, drop_wakes, "dropWakes", 0));
    JS_SetPropertyStr(ctx, global, "sendMode", JS_NewCFunction(ctx, set_send_mode, "sendMode", 1));
    JS_FreeValue(ctx, global);
    assert(js_net_register() == ESP_OK);
    assert(module_init(&runtime, ctx, module_opaque) == ESP_OK);

    run(&runtime,
        "(async()=>{"
        "const server=await net.listenTCP({host:'127.0.0.1',port:0});"
        "const accepting=server.accept();"
        "const client=await net.dialTCP({host:'127.0.0.1',port:server.localAddress.port});"
        "const peer=await accepting;const controller=new AbortController();"
        "const pending=client.read(16,{signal:controller.signal}).then(()=>check(false),"
        "e=>check(e===controller.signal.reason&&e.name==='AbortError'&&e.code==='ABORTED'));"
        "controller.abort();await pending;check(!client.closed);"
        "await peer.write(new Uint8Array([42]));check((await client.read(16))[0]===42);"
        "const prior=new AbortController();const reason=Error('custom cancellation');prior.abort(reason);"
        "await client.read(16,{signal:prior.signal}).then(()=>check(false),e=>check(e===reason));"
        "let invalid=false;try{client.read(16,{signal:{aborted:false}})}catch(e){invalid=e instanceof TypeError}"
        "check(invalid);await peer.write(new Uint8Array([7]));check((await client.read(16))[0]===7);"
        "controller.abort('ignored');await client.close();await peer.close();await server.close();})()");

    run(&runtime,
        "(async()=>{"
        "const server=await net.listenTCP({host:'127.0.0.1',port:0});"
        "const clients=[],peers=[];"
        "for(let i=0;i<7;i++){const accepting=server.accept();"
        "clients.push(await net.dialTCP({host:'127.0.0.1',port:server.localAddress.port}));"
        "peers.push(await accepting);}"
        "const cancelled=[];for(const client of clients){const c=new AbortController();"
        "cancelled.push(client.read(4096,{signal:c.signal}).catch(e=>check(e.code==='ABORTED')));c.abort();}"
        "const filling=clients[0].write(new Uint8Array(0));"
        "await clients[0].write(new Uint8Array(0)).then(()=>check(false),e=>check(e.code==='QUEUE_FULL'));"
        "await Promise.all(cancelled);await filling;"
        "for(const client of clients)await client.close();for(const peer of peers)await peer.close();"
        "await server.close();})()");

    run(&runtime,
        "(async()=>{"
        "const server=await net.listenTCP({host:'127.0.0.1',port:0});"
        "const accepting=server.accept();"
        "const client=await net.dialTCP({host:'127.0.0.1',port:server.localAddress.port});"
        "const peer=await accepting;"
        "await server.close();"
        "const waiting=client.read(64);"
        "await client.read(1).then(()=>check(false),e=>check(e instanceof SocketError&&e.code==='BUSY'));"
        "const data=new Uint8Array([99,1,2,3,99]);"
        "const written=client.write(data.subarray(1,4));data.fill(0);"
        "check(await written===3);"
        "const received=await peer.read(64);check(received.join(',')==='1,2,3');"
        "dropWakes();await peer.write(new Uint8Array([4,5]));"
        "check((await waiting).join(',')==='4,5');"
        "await client.closeWrite();check(await peer.read(64)===null);"
        "check(await peer.read(64)===null);"
        "await client.write(new Uint8Array([1])).then(()=>check(false),e=>check(e.code==='CLOSED'));"
        "await peer.write(new Uint8Array([6]));check((await client.read(64))[0]===6);"
        "await peer.closeWrite();check(await client.read(64)===null);"
        "const closing=client.close();check(client.closed&&closing===client.close());await closing;"
        "await peer.close();await server.close();check(server.closed);"
        "await client.close();})()");

    run(&runtime,
        "(async()=>{"
        "const server=await net.listenTCP({host:'127.0.0.1',port:0});"
        "await server.accept({timeoutMs:40}).then(()=>check(false),e=>check(e.code==='TIMEOUT'));"
        "const accepting=server.accept();"
        "const client=await net.dialTCP({host:'127.0.0.1',port:server.localAddress.port});"
        "const peer=await accepting;"
        "await client.read(16,{timeoutMs:40}).then(()=>check(false),e=>check(e.code==='TIMEOUT'));"
        "check(!client.closed);"
        "const waiting=client.read(16).catch(e=>check(e.code==='CLOSED'));"
        "await client.close();await waiting;await peer.close();"
        "const old=server;await server.close();"
        "const replacement=await net.listenTCP({host:'127.0.0.1',port:0});"
        "check(old.closed&&!replacement.closed);await old.close();check(!replacement.closed);"
        "await replacement.close();})()");

    run(&runtime,
        "(async()=>{"
        "const server=await net.listenTCP({host:'127.0.0.1',port:0});"
        "const slow=net.dialTCP({host:'slow.test',port:server.localAddress.port,timeoutMs:60})"
        ".then(()=>check(false),e=>check(e.code==='TIMEOUT'));"
        "const accepting=server.accept();"
        "const client=await net.dialTCP({host:'127.0.0.1',port:server.localAddress.port});"
        "const peer=await accepting;await client.write(new Uint8Array([7]));"
        "check((await peer.read(8))[0]===7);await slow;"
        "await client.close();await peer.close();await server.close();"
        "await net.dialTCP({host:'missing.test',port:1}).then(()=>check(false),e=>check(e.code==='DNS'));"
        "const next=await net.listenTCP({host:'127.0.0.1',port:0});await next.close();})()");

    run(&runtime,
        "(async()=>{"
        "const server=await net.listenTCP({host:'127.0.0.1',port:0});"
        "const accepting=server.accept();"
        "const client=await net.dialTCP({host:'local.test',port:server.localAddress.port});"
        "const peer=await accepting;check(client.remoteAddress.family===4);"
        "const pending=[client.read(1).catch(e=>check(e.code==='CLOSED'))];"
        "for(let i=0;i<7;i++)pending.push(client.write(new Uint8Array(0)).catch(e=>check(e.code==='CLOSED')));"
        "await client.write(new Uint8Array(0)).then(()=>check(false),e=>check(e.code==='QUEUE_FULL'));"
        "await client.close();await Promise.all(pending);await peer.close();await server.close();"
        "})()");

    run(&runtime,
        "(async()=>{"
        "const server=await net.listenTCP({host:'127.0.0.1',port:0});"
        "const accepting=server.accept();"
        "const client=await net.dialTCP({host:'127.0.0.1',port:server.localAddress.port});"
        "const peer=await accepting;"
        "const a=client.read(16384).catch(e=>check(e.code==='CLOSED'));"
        "const b=peer.read(16384).catch(e=>check(e.code==='CLOSED'));"
        "await client.write(new Uint8Array([1])).then(()=>check(false),e=>check(e.code==='QUEUE_FULL'));"
        "const c=client.close(),d=peer.close();await Promise.all([a,b,c,d]);await server.close();"
        "let threw=false;try{net.dialTCP({host:'127.0.0.1',port:1,unknown:1})}catch(e){threw=e instanceof TypeError}check(threw);"
        "})()");

    run(&runtime,
        "(async()=>{"
        "const listeners=[];for(let i=0;i<16;i++)listeners.push(await net.listenTCP({port:0}));"
        "await net.listenTCP({port:0}).then(()=>check(false),e=>check(e.code==='RESOURCE_LIMIT'));"
        "await Promise.all(listeners.map(x=>x.close()));"
        "const server=await net.listenTCP({host:'127.0.0.1',port:0});"
        "const port=server.localAddress.port;await server.close();"
        "await net.dialTCP({host:'127.0.0.1',port}).then(()=>check(false),e=>check(e.code==='CONNECTION_REFUSED'));"
        "})()");

    run(&runtime,
        "(async()=>{"
        "const server=await net.listenTCP({host:'127.0.0.1',port:0});"
        "const accepting=server.accept();"
        "const client=await net.dialTCP({host:'127.0.0.1',port:server.localAddress.port});"
        "const peer=await accepting;sendMode(1);"
        "const a=client.write(new Uint8Array([1,2,3,4,5]));"
        "const b=client.write(new Uint8Array([6,7,8]));const ended=client.closeWrite();"
        "const bytes=[];let part;while((part=await peer.read(64))!==null)bytes.push(...part);"
        "check(bytes.join(',')==='1,2,3,4,5,6,7,8');check(await a===5&&await b===3);await ended;"
        "sendMode(0);await Promise.all([client.close(),peer.close(),server.close()]);"
        "})()");

    run(&runtime,
        "(async()=>{"
        "const server=await net.listenTCP({host:'127.0.0.1',port:0});"
        "const accepting=server.accept();"
        "const client=await net.dialTCP({host:'127.0.0.1',port:server.localAddress.port});"
        "const peer=await accepting;sendMode(3);"
        "const reading=client.read(64).catch(e=>check(e.code==='CLOSED'));"
        "const a=client.write(new Uint8Array([1,2,3,4]),{timeoutMs:80})"
        ".then(()=>check(false),e=>check(e.code==='TIMEOUT'&&e.bytesWritten===2));"
        "const b=client.write(new Uint8Array([5])).then(()=>check(false),e=>check(e.code==='CLOSED'));"
        "await Promise.all([a,b,reading]);check(client.closed);sendMode(0);"
        "await Promise.all([client.close(),peer.close(),server.close()]);"
        "})()");

    if (argc > 1) {
        FILE *file = fopen(argv[1], "rb");
        assert(file);
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        rewind(file);
        char *source = calloc(1, size + 1);
        assert(source && fread(source, 1, size, file) == (size_t)size);
        fclose(file);
        run(&runtime, source);
        free(source);
    }

    /* Deinit must not need a completion event, even with pending I/O. */
    run(&runtime,
        "(async()=>{globalThis.shutdownServer=await net.listenTCP({host:'127.0.0.1',port:0});"
        "globalThis.shutdownWait=shutdownServer.accept().catch(e=>check(e.code==='CLOSED'));})()");
    atomic_store(&dropped_wakes, 100);
    module_deinit(&runtime, ctx, module_opaque);
    drain_jobs(rt);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    vQueueDelete(events);
    puts("net host tests passed");
    return 0;
}
