#include "js_telemetry.h"
#include "js_runtime.h"
#include "quickjs.h"
#include <string.h>

static app_mqtt_t *s_service;

static JSValue report_function(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv, int magic)
{
    (void)this_value;
    if (argc != 2 || !JS_IsString(argv[0]) || !JS_IsObject(argv[1]) || JS_IsArray(ctx, argv[1]))
        return JS_ThrowTypeError(ctx, "telemetry needs a plugin ID and declaration object");
    size_t id_length = 0, json_length = 0;
    const char *id = JS_ToCStringLen(ctx, &id_length, argv[0]);
    if (!id) return JS_EXCEPTION;
    if (id_length != strlen(id)) { JS_FreeCString(ctx, id); return JS_ThrowTypeError(ctx, "invalid plugin ID"); }
    JSValue serialized = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(serialized)) { JS_FreeCString(ctx, id); return JS_EXCEPTION; }
    const char *json = JS_ToCStringLen(ctx, &json_length, serialized);
    bool serialized_ok = json != NULL;
    char error[160] = "Cannot register telemetry";
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (json && json_length <= 16384)
        err = magic ? app_mqtt_validate_report(s_service, id, json, error, sizeof(error)) :
                      app_mqtt_register_report(s_service, id, json, error, sizeof(error));
    else if (json) strcpy(error, "telemetry declaration exceeds 16 KiB");
    JS_FreeCString(ctx, json);
    JS_FreeCString(ctx, id);
    JS_FreeValue(ctx, serialized);
    if (!serialized_ok) return JS_EXCEPTION;
    if (err != ESP_OK) return JS_ThrowTypeError(ctx, "%s", error);
    return JS_UNDEFINED;
}

static JSValue unregister_report(JSContext *ctx, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void)this_value;
    if (argc != 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "telemetry.unregister needs a plugin ID");
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;
    bool removed = app_mqtt_unregister_report(s_service, id);
    JS_FreeCString(ctx, id);
    return JS_NewBool(ctx, removed);
}

static esp_err_t module_init(js_runtime_t *runtime, JSContext *ctx, void *opaque)
{
    (void)runtime; (void)opaque;
    JSValue object = JS_NewObject(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    if (JS_IsException(object) || JS_IsException(global)) {
        JS_FreeValue(ctx, object); JS_FreeValue(ctx, global); return ESP_ERR_NO_MEM;
    }
    if (JS_SetPropertyStr(ctx, object, "register", JS_NewCFunctionMagic(ctx, report_function, "register", 2, JS_CFUNC_generic_magic, 0)) < 0 ||
        JS_SetPropertyStr(ctx, object, "validate", JS_NewCFunctionMagic(ctx, report_function, "validate", 2, JS_CFUNC_generic_magic, 1)) < 0 ||
        JS_SetPropertyStr(ctx, object, "unregister", JS_NewCFunction(ctx, unregister_report, "unregister", 1)) < 0) {
        JS_FreeValue(ctx, object); JS_FreeValue(ctx, global); return ESP_FAIL;
    }
    int result = JS_SetPropertyStr(ctx, global, "telemetry", object);
    JS_FreeValue(ctx, global);
    return result < 0 ? ESP_FAIL : ESP_OK;
}

static void module_deinit(js_runtime_t *runtime, JSContext *ctx, void *opaque)
{
    (void)runtime; (void)ctx; (void)opaque;
    app_mqtt_clear_reports(s_service);
}

esp_err_t js_telemetry_register(app_mqtt_t *service)
{
    if (!service || s_service) return ESP_ERR_INVALID_ARG;
    esp_err_t err = js_runtime_register_module(module_init, module_deinit, service);
    if (err == ESP_OK) s_service = service;
    return err;
}
