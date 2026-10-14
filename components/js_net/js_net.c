#include "js_net.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "js_event.h"
#include "js_runtime.h"
#include "js_request.h"
#include "quickjs.h"
#include "socket_worker.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#if CONFIG_JS_NET_ENABLED

typedef struct net_request net_request_t;
struct net_request {
    socket_request_t native;
    uint32_t id;
    JSValue owner;
    net_request_t *next;
};

typedef struct {
    unsigned pending;
    bool reading;
    bool accepting;
    bool ending;
    uint32_t close_id;
    uint32_t close_generation;
} net_slot_t;

typedef struct {
    unsigned slot;
    uint32_t generation;
    bool listener;
    JSValue close_promise;
} net_handle_t;

static struct {
    js_runtime_t *runtime;
    socket_engine_t engine;
    net_slot_t slots[CONFIG_JS_NET_MAX_CONNECTIONS];
    net_request_t *requests;
    js_request_table_t promises;
    js_request_table_t closes;
    size_t buffered;
    unsigned native_pending;
    JSValue error_constructor;
    bool initialized;
} s_net;
static JSClassID connection_class;
static JSClassID listener_class;
static bool registered;

static const char *operation_name(socket_operation_t operation)
{
    static const char *const names[] = {"dialTCP", "listenTCP", "accept",
                                        "read", "write", "closeWrite", "dialTLS"};
    return names[operation];
}

static JSValue make_error(JSContext *ctx, const char *code,
                          socket_operation_t operation, int number, size_t written)
{
    JSValue args[] = {JS_NewString(ctx, code), JS_NewString(ctx, operation_name(operation)),
                      JS_NewInt32(ctx, number), JS_NewInt64(ctx, written)};
    JSValue error = JS_CallConstructor(ctx, s_net.error_constructor, 4, (JSValueConst *)args);
    for (unsigned i = 0; i < 4; ++i) JS_FreeValue(ctx, args[i]);
    return error;
}

static JSValue immediate_promise(JSContext *ctx, const char *error,
                                 socket_operation_t operation)
{
    JSValue value = error ? make_error(ctx, error, operation, 0, 0) : JS_UNDEFINED;
    if (JS_IsException(value)) return value;
    JSValue promise = js_request_promise(ctx, error != NULL, value);
    JS_FreeValue(ctx, value);
    return promise;
}

static bool integer(JSContext *ctx, JSValueConst value, const char *name,
                     int min, int max, int *out)
{
    if (!JS_IsNumber(value)) {
        JS_ThrowTypeError(ctx, "%s must be an integer", name);
        return false;
    }
    double number;
    if (JS_ToFloat64(ctx, &number, value) < 0) return false;
    if (!isfinite(number) || floor(number) != number || number < min || number > max) {
        JS_ThrowRangeError(ctx, "%s is out of range", name);
        return false;
    }
    *out = (int)number;
    return true;
}

static bool option_integer(JSContext *ctx, JSValueConst options, const char *name,
                            int min, int max, int *out)
{
    JSValue value = JS_GetPropertyStr(ctx, options, name);
    if (JS_IsException(value)) return false;
    bool ok = JS_IsUndefined(value) || integer(ctx, value, name, min, max, out);
    JS_FreeValue(ctx, value);
    return ok;
}

static bool options_valid(JSContext *ctx, JSValueConst options,
                           const char *const *fields, size_t field_count)
{
    if (!JS_IsObject(options) || JS_IsArray(ctx, options) || JS_IsFunction(ctx, options)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return false;
    }
    JSPropertyEnum *properties;
    uint32_t count;
    if (JS_GetOwnPropertyNames(ctx, &properties, &count, options,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) return false;
    bool valid = true;
    for (uint32_t i = 0; i < count && valid; ++i) {
        const char *name = JS_AtomToCString(ctx, properties[i].atom);
        if (!name) { valid = false; break; }
        bool found = false;
        for (size_t j = 0; j < field_count; ++j) found |= strcmp(name, fields[j]) == 0;
        if (!found) {
            JS_ThrowTypeError(ctx, "unknown net option: %s", name);
            valid = false;
        }
        JS_FreeCString(ctx, name);
    }
    JS_FreePropertyEnum(ctx, properties, count);
    return valid;
}

static bool parse_timeout(JSContext *ctx, JSValueConst options, int *timeout, bool reading)
{
    if (JS_IsUndefined(options)) return true;
    static const char *const fields[] = {"timeoutMs", "signal"};
    return options_valid(ctx, options, fields, reading ? 2 : 1) &&
           option_integer(ctx, options, "timeoutMs", 0, INT32_MAX, timeout);
}

static net_handle_t *get_handle(JSContext *ctx, JSValueConst object)
{
    net_handle_t *handle = JS_GetOpaque(object, connection_class);
    if (!handle) handle = JS_GetOpaque(object, listener_class);
    if (!handle) JS_ThrowTypeError(ctx, "invalid TCP handle");
    return handle;
}

static void handle_finalizer(JSRuntime *runtime, JSValue object)
{
    net_handle_t *handle = JS_GetOpaque(object, connection_class);
    if (!handle) handle = JS_GetOpaque(object, listener_class);
    if (!handle) return;
    if (s_net.initialized)
        socket_engine_close(&s_net.engine, handle->slot, handle->generation);
    JS_FreeValueRT(runtime, handle->close_promise);
    free(handle);
}

static void handle_mark(JSRuntime *runtime, JSValueConst object, JS_MarkFunc *mark)
{
    net_handle_t *handle = JS_GetOpaque(object, connection_class);
    if (!handle) handle = JS_GetOpaque(object, listener_class);
    if (handle) JS_MarkValue(runtime, handle->close_promise, mark);
}

static JSValue address_object(JSContext *ctx, const socket_address_t *address)
{
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object)) return object;
    if (JS_DefinePropertyValueStr(ctx, object, "address", JS_NewString(ctx, address->address),
                                  JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(ctx, object, "port", JS_NewInt32(ctx, address->port),
                                  JS_PROP_ENUMERABLE) < 0 ||
        JS_DefinePropertyValueStr(ctx, object, "family", JS_NewInt32(ctx, address->family),
                                  JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

static JSValue make_handle(JSContext *ctx, unsigned slot, uint32_t generation, bool listener)
{
    net_handle_t *handle = calloc(1, sizeof(*handle));
    if (!handle) return JS_ThrowOutOfMemory(ctx);
    *handle = (net_handle_t){.slot = slot, .generation = generation,
                             .listener = listener, .close_promise = JS_UNDEFINED};
    JSValue object = JS_NewObjectClass(ctx, listener ? listener_class : connection_class);
    if (JS_IsException(object)) { free(handle); return object; }
    JS_SetOpaque(object, handle);
    socket_slot_t *native = &s_net.engine.slots[slot];
    if (JS_DefinePropertyValueStr(ctx, object, "localAddress",
                                  address_object(ctx, &native->local), JS_PROP_ENUMERABLE) < 0 ||
        (!listener && JS_DefinePropertyValueStr(ctx, object, "remoteAddress",
                                  address_object(ctx, &native->remote), JS_PROP_ENUMERABLE) < 0)) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

static JSValue bytes_object(JSContext *ctx, const uint8_t *bytes, size_t size)
{
    JSValue buffer = JS_NewArrayBufferCopy(ctx, bytes, size);
    if (JS_IsException(buffer)) return buffer;
    JSValue array = JS_NewTypedArray(ctx, 1, (JSValueConst *)&buffer, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, buffer);
    return array;
}

static void free_request(JSContext *ctx, net_request_t *request)
{
    JS_FreeValue(ctx, request->owner);
    free(request->native.data);
    free(request->native.ca);
    js_cancel_release(request->native.cancel);
    free(request);
}

static void cancel_read(uint32_t id, void *opaque)
{
    net_request_t *request = opaque;
    js_cancel_abort(id, request->native.cancel);
}
static void dispose_read(void *opaque)
{
    net_request_t *request = opaque;
    js_cancel_release(request->native.cancel);
}
static JSValue submit(JSContext *ctx, socket_request_t *native, JSValueConst owner,
                       int timeout, JSValueConst signal_value)
{
    JSValue signal = JS_DupValue(ctx, signal_value);
    if (JS_IsException(signal) || js_abort_aborted(ctx, signal)) {
        free(native->data); free(native->ca);
        if (JS_IsException(signal)) return signal;
        JSValue reason = js_abort_reason(ctx, signal);
        JSValue promise = js_request_promise(ctx, true, reason);
        JS_FreeValue(ctx, reason); JS_FreeValue(ctx, signal);
        return promise;
    }
    if (s_net.native_pending >= CONFIG_JS_NET_MAX_PENDING ||
        native->size + native->ca_size > CONFIG_JS_NET_BUFFER_LIMIT - s_net.buffered) {
        free(native->data);
        free(native->ca);
        JS_FreeValue(ctx, signal);
        return immediate_promise(ctx, "QUEUE_FULL", native->operation);
    }
    net_request_t *request = calloc(1, sizeof(*request));
    if (!request) { free(native->data); free(native->ca); JS_FreeValue(ctx, signal); return JS_ThrowOutOfMemory(ctx); }
    request->native = *native;
    request->owner = JS_DupValue(ctx, owner);
    request->native.cancel = JS_IsUndefined(signal) ? NULL : js_cancel_new();
    if (!JS_IsUndefined(signal) && !request->native.cancel) {
        JS_FreeValue(ctx, signal); free_request(ctx, request); return JS_ThrowOutOfMemory(ctx);
    }
    js_cancel_retain(request->native.cancel);
    JSValue promise = js_request_create(&s_net.promises, native->generation,
                                        request, dispose_read, cancel_read, &request->id);
    if (JS_IsException(promise)) {
        js_cancel_release(request->native.cancel); JS_FreeValue(ctx, signal);
        free_request(ctx, request); return promise;
    }
    if (!js_abort_attach(&s_net.promises, request->id, native->generation, signal)) {
        JSValue error = JS_GetException(ctx);
        js_request_reject(&s_net.promises, request->id, native->generation, error);
        JS_FreeValue(ctx, error); JS_FreeValue(ctx, signal); free_request(ctx, request);
        return promise;
    }
    JS_FreeValue(ctx, signal);
    request->native.deadline_us = timeout ? esp_timer_get_time() + (int64_t)timeout * 1000 : 0;
    socket_request_t *command = &request->native;
    if (xQueueSend(s_net.engine.commands, &command, 0) != pdTRUE) {
        JSValue error = make_error(ctx, "QUEUE_FULL", native->operation, 0, 0);
        if (JS_IsException(error)) error = JS_GetException(ctx);
        js_request_reject(&s_net.promises, request->id, native->generation, error);
        JS_FreeValue(ctx, error);
        free_request(ctx, request);
        return promise;
    }
    request->next = s_net.requests;
    s_net.requests = request;
    s_net.buffered += native->size + native->ca_size;
    ++s_net.native_pending;
    net_slot_t *slot = &s_net.slots[native->slot];
    ++slot->pending;
    if (native->operation == SOCKET_READ) slot->reading = true;
    if (native->operation == SOCKET_ACCEPT) slot->accepting = true;
    if (native->operation == SOCKET_END) slot->ending = true;
    return promise;
}

static bool parse_tls(JSContext *ctx, JSValueConst options, socket_request_t *command)
{
    struct in_addr v4;
    struct in6_addr v6;
    bool ip = inet_pton(AF_INET, command->host, &v4) == 1 ||
              inet_pton(AF_INET6, command->host, &v6) == 1;
    if (strpbrk(command->host, "/[]@?#") || (!ip && strchr(command->host, ':'))) {
        JS_ThrowRangeError(ctx, "host must be a hostname or IP address");
        return false;
    }
    JSValue name = JS_GetPropertyStr(ctx, options, "serverName");
    if (JS_IsException(name)) return false;
    if (JS_IsUndefined(name)) {
        JS_FreeValue(ctx, name);
        if (ip) { JS_ThrowTypeError(ctx, "serverName is required for an IP host"); return false; }
        strcpy(command->server_name, command->host);
    } else {
        if (!JS_IsString(name)) { JS_FreeValue(ctx, name); JS_ThrowTypeError(ctx, "serverName must be a string"); return false; }
        size_t size;
        const char *text = JS_ToCStringLen(ctx, &size, name);
        JS_FreeValue(ctx, name);
        if (!text) return false;
        bool valid = size > 0 && size <= 253 && !memchr(text, 0, size);
        if (valid) memcpy(command->server_name, text, size + 1);
        JS_FreeCString(ctx, text);
        if (!valid) { JS_ThrowRangeError(ctx, "invalid serverName"); return false; }
    }
    size_t label = 0;
    const char *name_text = command->server_name;
    if (strlen(name_text) > 253) {
        JS_ThrowRangeError(ctx, "serverName is too long");
        return false;
    }
    for (size_t i = 0;; ++i) {
        unsigned char ch = name_text[i];
        if (!ch || ch == '.') {
            if (!label || label > 63 || name_text[i - label] == '-' || name_text[i - 1] == '-') {
                JS_ThrowRangeError(ctx, "serverName must be a DNS name");
                return false;
            }
            if (!ch) break;
            label = 0;
        } else {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '-')) {
                JS_ThrowRangeError(ctx, "serverName must be a DNS name");
                return false;
            }
            ++label;
        }
    }
    if (inet_pton(AF_INET, name_text, &v4) == 1) {
        JS_ThrowRangeError(ctx, "serverName must be a DNS name");
        return false;
    }
    JSValue ca = JS_GetPropertyStr(ctx, options, "ca");
    if (JS_IsException(ca)) return false;
    if (JS_IsUndefined(ca)) { JS_FreeValue(ctx, ca); return true; }
    if (!JS_IsArray(ctx, ca)) { JS_FreeValue(ctx, ca); JS_ThrowTypeError(ctx, "ca must be an array of PEM strings"); return false; }
    int count = 0;
    JSValue length = JS_GetPropertyStr(ctx, ca, "length");
    bool valid = !JS_IsException(length) && integer(ctx, length, "CA count", 0, 8, &count);
    JS_FreeValue(ctx, length);
    size_t contents = 0;
    for (int i = 0; valid && i < count; ++i) {
        JSAtom key = JS_NewAtomUInt32(ctx, i);
        int own = JS_GetOwnProperty(ctx, NULL, ca, key);
        JS_FreeAtom(ctx, key);
        if (own < 0) { valid = false; break; }
        JSValue value = own ? JS_GetPropertyUint32(ctx, ca, i) : JS_UNDEFINED;
        if (JS_IsException(value)) { valid = false; break; }
        if (!JS_IsString(value)) {
            JS_FreeValue(ctx, value);
            JS_ThrowTypeError(ctx, "each CA must be a PEM string");
            valid = false;
            break;
        }
        size_t size;
        const char *text = JS_ToCStringLen(ctx, &size, value);
        JS_FreeValue(ctx, value);
        if (!text) { valid = false; break; }
        if (!size || memchr(text, 0, size) || size > 16384 - contents) {
            JS_ThrowRangeError(ctx, "CA list must contain nonempty strings totaling at most 16 KiB");
            valid = false;
        } else {
            char *copy = realloc(command->ca, command->ca_size + size + 1);
            if (!copy) { JS_ThrowOutOfMemory(ctx); valid = false; }
            else {
                command->ca = copy;
                memcpy(copy + command->ca_size, text, size + 1);
                command->ca_size += size + 1;
                contents += size;
            }
        }
        JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, ca);
    if (!valid) { free(command->ca); command->ca = NULL; command->ca_size = 0; }
    return valid;
}

static JSValue js_open(JSContext *ctx, JSValueConst this_value, int argc,
                       JSValueConst *argv, int kind)
{
    bool listener = kind == SOCKET_LISTEN;
    bool secure = kind == SOCKET_DIAL_TLS;
    (void)this_value;
    if (!s_net.initialized) return JS_ThrowInternalError(ctx, "network runtime stopped");
    if (argc < 1) return JS_ThrowTypeError(ctx, "options are required");
    static const char *const dial_fields[] = {"host", "port", "timeoutMs", "noDelay"};
    static const char *const listen_fields[] = {"host", "port", "timeoutMs", "backlog"};
    static const char *const tls_fields[] = {"host", "port", "timeoutMs", "noDelay", "serverName", "ca"};
    if (!options_valid(ctx, argv[0], secure ? tls_fields : listener ? listen_fields : dial_fields,
                        secure ? 6 : 4)) return JS_EXCEPTION;
    socket_request_t command = {.operation = kind,
                                .no_delay = true, .backlog = 4};
    JSValue host = JS_GetPropertyStr(ctx, argv[0], "host");
    if (JS_IsException(host)) return host;
    if (listener && JS_IsUndefined(host)) strcpy(command.host, "0.0.0.0");
    else {
        if (!JS_IsString(host)) { JS_FreeValue(ctx, host); return JS_ThrowTypeError(ctx, "host must be a string"); }
        size_t size;
        const char *text = JS_ToCStringLen(ctx, &size, host);
        if (!text) { JS_FreeValue(ctx, host); return JS_EXCEPTION; }
        bool valid = size > 0 && size < sizeof(command.host) && !memchr(text, 0, size);
        if (valid) memcpy(command.host, text, size + 1);
        JS_FreeCString(ctx, text);
        if (!valid) { JS_FreeValue(ctx, host); return JS_ThrowRangeError(ctx, "invalid host"); }
    }
    JS_FreeValue(ctx, host);
    JSValue port_value = JS_GetPropertyStr(ctx, argv[0], "port");
    int port = 0, timeout = secure ? 10000 : 5000, backlog = 4;
    bool valid = !JS_IsException(port_value) &&
                 integer(ctx, port_value, "port", listener ? 0 : 1, 65535, &port);
    JS_FreeValue(ctx, port_value);
    if (!valid || !option_integer(ctx, argv[0], "timeoutMs", 0, INT32_MAX, &timeout)) return JS_EXCEPTION;
    command.port = port;
    if (listener) {
        if (!option_integer(ctx, argv[0], "backlog", 1, 128, &backlog)) return JS_EXCEPTION;
        command.backlog = backlog;
    } else {
        JSValue value = JS_GetPropertyStr(ctx, argv[0], "noDelay");
        if (JS_IsException(value)) return value;
        if (!JS_IsUndefined(value)) {
            if (!JS_IsBool(value)) { JS_FreeValue(ctx, value); return JS_ThrowTypeError(ctx, "noDelay must be boolean"); }
            command.no_delay = JS_ToBool(ctx, value);
        }
        JS_FreeValue(ctx, value);
    }
    if (secure && !parse_tls(ctx, argv[0], &command)) return JS_EXCEPTION;
#if !CONFIG_JS_NET_TLS_ENABLED
    if (secure) { free(command.ca); return immediate_promise(ctx, "UNSUPPORTED", kind); }
#endif
    if (s_net.promises.count >= CONFIG_JS_NET_MAX_PENDING ||
        command.ca_size > CONFIG_JS_NET_BUFFER_LIMIT - s_net.buffered) {
        free(command.ca);
        return immediate_promise(ctx, "QUEUE_FULL", command.operation);
    }
    int slot = socket_engine_reserve(&s_net.engine, &command.generation);
    if (slot < 0) { free(command.ca); return immediate_promise(ctx, "RESOURCE_LIMIT", command.operation); }
    command.slot = slot;
    s_net.slots[slot] = (net_slot_t){0};
    JSValue promise = submit(ctx, &command, JS_UNDEFINED, timeout, JS_UNDEFINED);
    if (s_net.slots[slot].pending == 0) socket_engine_close(&s_net.engine, slot, command.generation);
    return promise;
}

static bool input_bytes(JSContext *ctx, JSValueConst value, const uint8_t **bytes,
                         size_t *size, JSValue *buffer)
{
    *buffer = JS_UNDEFINED;
    *bytes = JS_GetArrayBuffer(ctx, size, value);
    if (*bytes) return true;
    JSValue error = JS_GetException(ctx);
    JS_FreeValue(ctx, error);
    size_t offset, element_size;
    *buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, size, &element_size);
    if (JS_IsException(*buffer)) {
        error = JS_GetException(ctx);
        JS_FreeValue(ctx, error);
        JS_ThrowTypeError(ctx, "data must be a byte typed array or ArrayBuffer");
        return false;
    }
    size_t capacity;
    uint8_t *base = JS_GetArrayBuffer(ctx, &capacity, *buffer);
    if (!base || element_size != 1 || offset > capacity || *size > capacity - offset) {
        JS_FreeValue(ctx, *buffer);
        *buffer = JS_UNDEFINED;
        JS_ThrowTypeError(ctx, "data must be a byte typed array or ArrayBuffer");
        return false;
    }
    *bytes = base + offset;
    return true;
}

static JSValue js_io(JSContext *ctx, JSValueConst this_value, int argc,
                     JSValueConst *argv, int operation)
{
    net_handle_t *handle = get_handle(ctx, this_value);
    if (!handle) return JS_EXCEPTION;
    if (!s_net.initialized) return JS_ThrowInternalError(ctx, "network runtime stopped");
    if (handle->listener != (operation == SOCKET_ACCEPT))
        return JS_ThrowTypeError(ctx, "operation is not supported by this TCP handle");
    int timeout = operation == SOCKET_WRITE || operation == SOCKET_END ? 5000 : 0;
    unsigned options_index = operation == SOCKET_READ || operation == SOCKET_WRITE ? 1 : 0;
    if (!parse_timeout(ctx, argc > (int)options_index ? argv[options_index] : JS_UNDEFINED,
                        &timeout, operation == SOCKET_READ)) return JS_EXCEPTION;
    socket_request_t command = {.operation = operation, .slot = handle->slot,
                                .generation = handle->generation};
    const uint8_t *bytes = NULL;
    JSValue buffer = JS_UNDEFINED;
    if (operation == SOCKET_READ) {
        int size;
        if (!integer(ctx, argc ? argv[0] : JS_UNDEFINED, "maxBytes", 1,
                      CONFIG_JS_NET_MAX_TRANSFER_SIZE, &size)) return JS_EXCEPTION;
        command.size = size;
    } else if (operation == SOCKET_WRITE) {
        if (!input_bytes(ctx, argc ? argv[0] : JS_UNDEFINED, &bytes, &command.size, &buffer))
            return JS_EXCEPTION;
        if (command.size > CONFIG_JS_NET_MAX_TRANSFER_SIZE) {
            JS_FreeValue(ctx, buffer);
            return JS_ThrowRangeError(ctx, "write exceeds maximum transfer size");
        }
    }
    JSValue signal = operation == SOCKET_READ
        ? js_abort_get_signal(ctx, argc > 1 ? argv[1] : JS_UNDEFINED) : JS_UNDEFINED;
    if (JS_IsException(signal)) { JS_FreeValue(ctx, buffer); return JS_EXCEPTION; }
    if (js_abort_aborted(ctx, signal)) {
        JS_FreeValue(ctx, buffer);
        JSValue promise = submit(ctx, &command, this_value, timeout, signal);
        JS_FreeValue(ctx, signal);
        return promise;
    }
    const char *error = NULL;
    net_slot_t *slot = &s_net.slots[handle->slot];
    if (socket_engine_closed(&s_net.engine, handle->slot, handle->generation)) error = "CLOSED";
    else if ((operation == SOCKET_WRITE || operation == SOCKET_END) && slot->ending) error = "CLOSED";
    else if ((operation == SOCKET_READ && slot->reading) ||
             (operation == SOCKET_ACCEPT && slot->accepting)) error = "BUSY";
    else if (s_net.native_pending >= CONFIG_JS_NET_MAX_PENDING ||
             command.size > CONFIG_JS_NET_BUFFER_LIMIT - s_net.buffered) error = "QUEUE_FULL";
    if (error) {
        JS_FreeValue(ctx, signal);
        JS_FreeValue(ctx, buffer);
        return immediate_promise(ctx, error, operation);
    }
    if (command.size) {
        command.data = malloc(command.size);
        if (!command.data) {
            JS_FreeValue(ctx, buffer); JS_FreeValue(ctx, signal);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (bytes) memcpy(command.data, bytes, command.size);
    }
    JS_FreeValue(ctx, buffer);
    JSValue promise = submit(ctx, &command, this_value, timeout, signal);
    JS_FreeValue(ctx, signal);
    return promise;
}

static JSValue js_closed(JSContext *ctx, JSValueConst object)
{
    net_handle_t *handle = get_handle(ctx, object);
    if (!handle) return JS_EXCEPTION;
    if (!s_net.initialized) return JS_TRUE;
    return JS_NewBool(ctx, socket_engine_closed(&s_net.engine, handle->slot, handle->generation));
}

static JSValue js_close(JSContext *ctx, JSValueConst object, int argc, JSValueConst *argv)
{
    (void)argc;
    (void)argv;
    net_handle_t *handle = get_handle(ctx, object);
    if (!handle) return JS_EXCEPTION;
    if (!JS_IsUndefined(handle->close_promise)) return JS_DupValue(ctx, handle->close_promise);
    if (!s_net.initialized) return immediate_promise(ctx, NULL, SOCKET_END);
    socket_slot_t *native = &s_net.engine.slots[handle->slot];
    portENTER_CRITICAL(&s_net.engine.lock);
    bool exists = native->used && native->generation == handle->generation;
    portEXIT_CRITICAL(&s_net.engine.lock);
    if (!exists) return immediate_promise(ctx, NULL, SOCKET_END);
    net_slot_t *slot = &s_net.slots[handle->slot];
    JSValue promise = js_request_create(&s_net.closes, handle->generation,
                                        NULL, NULL, NULL, &slot->close_id);
    if (JS_IsException(promise)) return promise;
    slot->close_generation = handle->generation;
    handle->close_promise = JS_DupValue(ctx, promise);
    socket_engine_close(&s_net.engine, handle->slot, handle->generation);
    return promise;
}

static void notify(void *opaque)
{
    js_event_t event = {.type = JS_EVENT_NET};
    /* Results stay in the completion queue; the worker retries a lost wake. */
    js_event_post(opaque, &event, 0);
}

static void event_handler(js_runtime_t *runtime, const js_event_t *event, void *opaque)
{
    (void)event;
    (void)opaque;
    JSContext *ctx = js_runtime_context(runtime);
    socket_request_t *native;
    /* A bounded batch lets timers and other modules continue to run. */
    for (unsigned count = 0; count < 8 &&
         xQueueReceive(s_net.engine.completions, &native, 0) == pdTRUE; ++count) {
        net_request_t *request = (net_request_t *)native;
        net_request_t **link = &s_net.requests;
        while (*link != request) link = &(*link)->next;
        *link = request->next;
        s_net.buffered -= native->size + native->ca_size;
        --s_net.native_pending;
        net_slot_t *slot = &s_net.slots[native->slot];
        --slot->pending;
        if (native->operation == SOCKET_READ) slot->reading = false;
        if (native->operation == SOCKET_ACCEPT) slot->accepting = false;
        if (!js_request_find(&s_net.promises, request->id, native->generation)) {
            free_request(ctx, request);
            continue;
        }
        JSValue value;
        if (native->error) value = make_error(ctx, native->error, native->operation,
                                             native->system_error, native->transferred);
        else if (native->operation == SOCKET_DIAL || native->operation == SOCKET_DIAL_TLS || native->operation == SOCKET_LISTEN)
            value = make_handle(ctx, native->slot, native->generation, native->operation == SOCKET_LISTEN);
        else if (native->operation == SOCKET_ACCEPT) {
            s_net.slots[native->accepted_slot] = (net_slot_t){0};
            value = make_handle(ctx, native->accepted_slot, native->accepted_generation, false);
        } else if (native->operation == SOCKET_READ)
            value = native->eof ? JS_NULL : bytes_object(ctx, native->data, native->transferred);
        else if (native->operation == SOCKET_WRITE) value = JS_NewInt64(ctx, native->transferred);
        else value = JS_UNDEFINED;
        if (native->error && !JS_IsException(value)) {
            if (native->tls_code) JS_SetPropertyStr(ctx, value, "tlsCode", JS_NewInt32(ctx, native->tls_code));
            if (native->verify_flags) JS_SetPropertyStr(ctx, value, "verifyFlags", JS_NewUint32(ctx, native->verify_flags));
        }
        bool rejected = native->error != NULL;
        if (JS_IsException(value)) {
            value = JS_GetException(ctx);
            rejected = true;
            if (native->operation == SOCKET_DIAL || native->operation == SOCKET_DIAL_TLS || native->operation == SOCKET_LISTEN)
                socket_engine_close(&s_net.engine, native->slot, native->generation);
            if (native->operation == SOCKET_ACCEPT)
                socket_engine_close(&s_net.engine, native->accepted_slot, native->accepted_generation);
        }
        if (rejected) js_request_reject(&s_net.promises, request->id, native->generation, value);
        else js_request_resolve(&s_net.promises, request->id, native->generation, value);
        JS_FreeValue(ctx, value);
        free_request(ctx, request);
    }
    for (unsigned i = 0; i < CONFIG_JS_NET_MAX_CONNECTIONS; ++i) {
        portENTER_CRITICAL(&s_net.engine.lock);
        bool closed = s_net.engine.slots[i].used && s_net.engine.slots[i].closed;
        portEXIT_CRITICAL(&s_net.engine.lock);
        net_slot_t *slot = &s_net.slots[i];
        if (!closed || slot->pending) continue;
        if (slot->close_id) {
            js_request_resolve(&s_net.closes, slot->close_id, slot->close_generation, JS_UNDEFINED);
            slot->close_id = 0;
        }
        socket_engine_release(&s_net.engine, i);
    }
}

static const JSCFunctionListEntry connection_methods[] = {
    JS_CFUNC_MAGIC_DEF("read", 1, js_io, SOCKET_READ),
    JS_CFUNC_MAGIC_DEF("write", 1, js_io, SOCKET_WRITE),
    JS_CFUNC_MAGIC_DEF("closeWrite", 0, js_io, SOCKET_END),
    JS_CFUNC_DEF("close", 0, js_close),
    JS_CGETSET_DEF("closed", js_closed, NULL),
};
static const JSCFunctionListEntry listener_methods[] = {
    JS_CFUNC_MAGIC_DEF("accept", 0, js_io, SOCKET_ACCEPT),
    JS_CFUNC_DEF("close", 0, js_close),
    JS_CGETSET_DEF("closed", js_closed, NULL),
};
static const JSCFunctionListEntry net_methods[] = {
    JS_CFUNC_MAGIC_DEF("dialTCP", 1, js_open, SOCKET_DIAL),
    JS_CFUNC_MAGIC_DEF("listenTCP", 1, js_open, SOCKET_LISTEN),
    JS_CFUNC_MAGIC_DEF("dialTLS", 1, js_open, SOCKET_DIAL_TLS),
};

static void module_deinit(js_runtime_t *runtime, JSContext *ctx, void *opaque)
{
    (void)opaque;
    s_net.initialized = false;
    socket_engine_stop(&s_net.engine);
    while (s_net.requests) {
        net_request_t *request = s_net.requests;
        s_net.requests = request->next;
        JSValue error = make_error(ctx, "CLOSED", request->native.operation, 0, 0);
        if (JS_IsException(error)) error = JS_GetException(ctx);
        js_request_reject(&s_net.promises, request->id, request->native.generation, error);
        JS_FreeValue(ctx, error);
        free_request(ctx, request);
    }
    for (unsigned i = 0; i < CONFIG_JS_NET_MAX_CONNECTIONS; ++i) {
        net_slot_t *slot = &s_net.slots[i];
        js_request_resolve(&s_net.closes, slot->close_id, slot->close_generation, JS_UNDEFINED);
        slot->close_id = 0;
    }
    js_request_table_close(&s_net.promises, JS_UNDEFINED);
    js_request_table_close(&s_net.closes, JS_UNDEFINED);
    JS_FreeValue(ctx, s_net.error_constructor);
    js_event_unregister_handler(runtime, JS_EVENT_NET);
}

static esp_err_t module_init(js_runtime_t *runtime, JSContext *ctx, void *opaque)
{
    (void)opaque;
    s_net.runtime = runtime;
    js_request_table_init(&s_net.promises, ctx);
    js_request_table_init(&s_net.closes, ctx);
    esp_err_t tracked = js_runtime_track_requests(runtime, &s_net.promises);
    if (tracked != ESP_OK) return tracked;
    tracked = js_runtime_track_requests(runtime, &s_net.closes);
    if (tracked != ESP_OK) return tracked;
    for (unsigned i = 0; i < CONFIG_JS_NET_MAX_CONNECTIONS; ++i)
        s_net.slots[i].close_id = 0;
    if (!connection_class) { JS_NewClassID(&connection_class); JS_NewClassID(&listener_class); }
    JSClassDef connection = {.class_name = "TCPConn", .finalizer = handle_finalizer, .gc_mark = handle_mark};
    JSClassDef listener = {.class_name = "TCPListener", .finalizer = handle_finalizer, .gc_mark = handle_mark};
    if (JS_NewClass(JS_GetRuntime(ctx), connection_class, &connection) < 0 ||
        JS_NewClass(JS_GetRuntime(ctx), listener_class, &listener) < 0) return ESP_ERR_NO_MEM;
    JSValue prototype = JS_NewObject(ctx);
    if (JS_IsException(prototype)) return ESP_ERR_NO_MEM;
    JS_SetPropertyFunctionList(ctx, prototype, connection_methods, sizeof(connection_methods) / sizeof(*connection_methods));
    JS_SetClassProto(ctx, connection_class, prototype);
    prototype = JS_NewObject(ctx);
    if (JS_IsException(prototype)) return ESP_ERR_NO_MEM;
    JS_SetPropertyFunctionList(ctx, prototype, listener_methods, sizeof(listener_methods) / sizeof(*listener_methods));
    JS_SetClassProto(ctx, listener_class, prototype);
    static const char source[] =
        "(class SocketError extends Error { constructor(code,operation,errno,bytesWritten){"
        "super(operation+': '+code);this.name='SocketError';this.code=code;"
        "this.operation=operation;if(errno)this.errno=errno;"
        "if(operation==='write')this.bytesWritten=bytesWritten;}})";
    s_net.error_constructor = JS_Eval(ctx, source, sizeof(source) - 1, "SocketError", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(s_net.error_constructor)) return ESP_FAIL;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue net = JS_NewObject(ctx);
    if (JS_IsException(net)) { JS_FreeValue(ctx, global); JS_FreeValue(ctx, s_net.error_constructor); return ESP_ERR_NO_MEM; }
    JS_SetPropertyFunctionList(ctx, net, net_methods, sizeof(net_methods) / sizeof(*net_methods));
    int result = JS_SetPropertyStr(ctx, global, "net", net);
    if (result >= 0) result = JS_SetPropertyStr(ctx, global, "SocketError", JS_DupValue(ctx, s_net.error_constructor));
    JS_FreeValue(ctx, global);
    if (result < 0) { JS_FreeValue(ctx, s_net.error_constructor); return ESP_FAIL; }
    esp_err_t error = js_event_register_handler(runtime, JS_EVENT_NET, event_handler, NULL);
    if (error != ESP_OK) { JS_FreeValue(ctx, s_net.error_constructor); return error; }
    if (!socket_engine_start(&s_net.engine, notify, runtime)) {
        js_event_unregister_handler(runtime, JS_EVENT_NET);
        JS_FreeValue(ctx, s_net.error_constructor);
        return ESP_ERR_NO_MEM;
    }
    s_net.initialized = true;
    return ESP_OK;
}

esp_err_t js_net_register(void)
{
    if (registered) return ESP_ERR_INVALID_STATE;
    esp_err_t error = js_runtime_register_module(module_init, module_deinit, NULL);
    if (error == ESP_OK) registered = true;
    return error;
}

#else
esp_err_t js_net_register(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif
