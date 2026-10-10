#include "js_fs.h"

#include <stdlib.h>
#include <string.h>

#include "js_runtime.h"
#include "quickjs.h"

#define JS_FS_PAGE_SIZE 32
#define JS_FS_MAX_FILE_SIZE (64 * 1024)

static bool s_registered;
static app_filesystem_t *s_filesystem;

static app_filesystem_t *filesystem(JSContext *ctx)
{
    (void)ctx;
    return s_filesystem;
}

static bool read_only_path(const char *path)
{
    return strcmp(path, "/main.js") == 0 ||
           strcmp(path, "/plugins") == 0 ||
           strncmp(path, "/plugins/", sizeof("/plugins/") - 1) == 0 ||
           strcmp(path, "/stdlib") == 0 ||
           strncmp(path, "/stdlib/", sizeof("/stdlib/") - 1) == 0;
}

static JSValue read_only_error(JSContext *ctx, const char *operation)
{
    return JS_ThrowInternalError(ctx, "fs.%s: path is read-only", operation);
}

static void clear_exception(JSContext *ctx)
{
    JSValue error = JS_GetException(ctx);
    JS_FreeValue(ctx, error);
}

static JSValue fs_error(JSContext *ctx, const char *operation, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        return JS_ThrowTypeError(ctx, "fs.%s: invalid path or argument", operation);
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return JS_ThrowReferenceError(ctx, "fs.%s: path not found", operation);
    }
    if (err == APP_FILESYSTEM_ERR_EXISTS) {
        return JS_ThrowInternalError(ctx, "fs.%s: path already exists", operation);
    }
    if (err == APP_FILESYSTEM_ERR_NOT_EMPTY) {
        return JS_ThrowInternalError(ctx, "fs.%s: directory is not empty", operation);
    }
    if (err == APP_FILESYSTEM_ERR_IS_DIRECTORY) {
        return JS_ThrowTypeError(ctx, "fs.%s: path is a directory", operation);
    }
    if (err == APP_FILESYSTEM_ERR_NOT_DIRECTORY) {
        return JS_ThrowTypeError(ctx, "fs.%s: path is not a directory", operation);
    }
    if (err == APP_FILESYSTEM_ERR_FULL) {
        return JS_ThrowInternalError(ctx, "fs.%s: filesystem is full", operation);
    }
    if (err == ESP_ERR_NO_MEM) return JS_ThrowOutOfMemory(ctx);
    return JS_ThrowInternalError(ctx, "fs.%s failed: %s", operation,
                                 esp_err_to_name(err));
}

static const char *path_arg(JSContext *ctx, int argc, JSValueConst *argv,
                            int index, bool optional)
{
    if (index >= argc || JS_IsUndefined(argv[index])) {
        if (optional) {
            JSValue root = JS_NewString(ctx, "/");
            if (JS_IsException(root)) return NULL;
            const char *path = JS_ToCString(ctx, root);
            JS_FreeValue(ctx, root);
            return path;
        }
        JS_ThrowTypeError(ctx, "path must be an absolute string");
        return NULL;
    }
    if (!JS_IsString(argv[index])) {
        JS_ThrowTypeError(ctx, "path must be an absolute string");
        return NULL;
    }
    size_t length;
    const char *path = JS_ToCStringLen(ctx, &length, argv[index]);
    if (!path) return NULL;
    if (strlen(path) != length || path[0] != '/') {
        JS_FreeCString(ctx, path);
        JS_ThrowTypeError(ctx, "path must be an absolute string without NUL");
        return NULL;
    }
    return path;
}

static JSValue file_info(JSContext *ctx, const app_filesystem_file_info_t *info,
                         const char *sha256)
{
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object)) return object;
    if (JS_SetPropertyStr(ctx, object, "path", JS_NewString(ctx, info->path)) < 0 ||
        JS_SetPropertyStr(ctx, object, "size", JS_NewFloat64(ctx, (double)info->size)) < 0 ||
        JS_SetPropertyStr(ctx, object, "directory", JS_NewBool(ctx, info->directory)) < 0 ||
        (sha256 && JS_SetPropertyStr(ctx, object, "sha256", JS_NewString(ctx, sha256)) < 0)) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

static JSValue js_fs_list(JSContext *ctx, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
    (void)this_value;
    const char *path = path_arg(ctx, argc, argv, 0, true);
    if (!path) return JS_EXCEPTION;
    const char *after = NULL;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (!JS_IsString(argv[1])) {
            JS_FreeCString(ctx, path);
            return JS_ThrowTypeError(ctx, "after must be a path string");
        }
        size_t length;
        after = JS_ToCStringLen(ctx, &length, argv[1]);
        if (!after) { JS_FreeCString(ctx, path); return JS_EXCEPTION; }
        if (strlen(after) != length) {
            JS_FreeCString(ctx, after);
            JS_FreeCString(ctx, path);
            return JS_ThrowTypeError(ctx, "after must not contain NUL");
        }
    }
    app_filesystem_file_info_t *entries = calloc(JS_FS_PAGE_SIZE, sizeof(*entries));
    if (!entries) {
        JS_FreeCString(ctx, after);
        JS_FreeCString(ctx, path);
        return JS_ThrowOutOfMemory(ctx);
    }
    size_t count = 0;
    bool more = false;
    esp_err_t err = app_filesystem_list(filesystem(ctx), path, after, entries,
                                        JS_FS_PAGE_SIZE, &count, &more);
    JS_FreeCString(ctx, after);
    JS_FreeCString(ctx, path);
    if (err != ESP_OK) {
        free(entries);
        return fs_error(ctx, "list", err);
    }
    JSValue result = JS_NewObject(ctx);
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(result) || JS_IsException(array)) goto fail;
    for (uint32_t i = 0; i < count; ++i) {
        JSValue item = file_info(ctx, &entries[i], NULL);
        if (JS_IsException(item) || JS_SetPropertyUint32(ctx, array, i, item) < 0) goto fail;
    }
    if (JS_SetPropertyStr(ctx, result, "entries", array) < 0) goto fail_result;
    array = JS_UNDEFINED;
    JSValue next = more && count ? JS_NewString(ctx, entries[count - 1].path) : JS_NULL;
    if (JS_SetPropertyStr(ctx, result, "nextAfter", next) < 0) goto fail_result;
    free(entries);
    return result;
fail:
    JS_FreeValue(ctx, array);
fail_result:
    JS_FreeValue(ctx, result);
    free(entries);
    return JS_EXCEPTION;
}

static JSValue js_fs_stat(JSContext *ctx, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
    (void)this_value;
    const char *path = path_arg(ctx, argc, argv, 0, false);
    if (!path) return JS_EXCEPTION;
    app_filesystem_file_info_t info = {0};
    char sha256[APP_FILESYSTEM_SHA256_HEX_SIZE];
    esp_err_t err = app_filesystem_stat(filesystem(ctx), path, &info, sha256);
    JS_FreeCString(ctx, path);
    if (err != ESP_OK) return fs_error(ctx, "stat", err);
    info.directory = false;
    return file_info(ctx, &info, sha256);
}

static JSValue read_file(JSContext *ctx, int argc, JSValueConst *argv, bool bytes)
{
    const char *path = path_arg(ctx, argc, argv, 0, false);
    if (!path) return JS_EXCEPTION;
    app_filesystem_reader_t *reader = NULL;
    app_filesystem_file_info_t info;
    char sha256[APP_FILESYSTEM_SHA256_HEX_SIZE];
    esp_err_t err = app_filesystem_read_open(filesystem(ctx), path, &reader,
                                             &info, sha256);
    JS_FreeCString(ctx, path);
    if (err != ESP_OK) return fs_error(ctx, "read", err);
    if (info.size > JS_FS_MAX_FILE_SIZE) {
        app_filesystem_read_close(reader);
        return JS_ThrowRangeError(ctx, "fs.read: file exceeds %d bytes",
                                  JS_FS_MAX_FILE_SIZE);
    }
    uint8_t *data = malloc(info.size ? info.size : 1);
    if (!data) {
        app_filesystem_read_close(reader);
        return JS_ThrowOutOfMemory(ctx);
    }
    size_t received = 0;
    while (received < info.size) {
        size_t count = 0;
        err = app_filesystem_read(reader, data + received,
                                  info.size - received, &count);
        if (err != ESP_OK || count == 0) break;
        received += count;
    }
    app_filesystem_read_close(reader);
    if (err != ESP_OK || received != info.size) {
        free(data);
        return fs_error(ctx, "read", err == ESP_OK ? ESP_FAIL : err);
    }
    JSValue result = bytes ? JS_NewArrayBufferCopy(ctx, data, info.size)
                           : JS_NewStringLen(ctx, (const char *)data, info.size);
    free(data);
    return result;
}

static JSValue js_fs_read_text(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void)this_value;
    return read_file(ctx, argc, argv, false);
}

static JSValue js_fs_read_bytes(JSContext *ctx, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void)this_value;
    return read_file(ctx, argc, argv, true);
}

static JSValue js_fs_write(JSContext *ctx, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void)this_value;
    const char *path = path_arg(ctx, argc, argv, 0, false);
    if (!path) return JS_EXCEPTION;
    if (read_only_path(path)) {
        JS_FreeCString(ctx, path);
        return read_only_error(ctx, "write");
    }
    if (argc < 2) {
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "data must be a string, Uint8Array, or ArrayBuffer");
    }
    const uint8_t *data = NULL;
    size_t size = 0;
    const char *string = NULL;
    JSValue buffer = JS_UNDEFINED;
    if (JS_IsString(argv[1])) {
        string = JS_ToCStringLen(ctx, &size, argv[1]);
        data = (const uint8_t *)string;
    } else {
        data = JS_GetArrayBuffer(ctx, &size, argv[1]);
        if (!data) {
            clear_exception(ctx);
            size_t offset = 0, element_size = 0;
            buffer = JS_GetTypedArrayBuffer(ctx, argv[1], &offset,
                                            &size, &element_size);
            if (!JS_IsException(buffer) && element_size == 1) {
                size_t buffer_size = 0;
                const uint8_t *base = JS_GetArrayBuffer(ctx, &buffer_size, buffer);
                if (base && offset <= buffer_size && size <= buffer_size - offset) {
                    data = base + offset;
                }
            }
            if (JS_IsException(buffer)) { clear_exception(ctx); buffer = JS_UNDEFINED; }
        }
    }
    if (!data) {
        JS_FreeValue(ctx, buffer);
        JS_FreeCString(ctx, string);
        JS_FreeCString(ctx, path);
        return JS_ThrowTypeError(ctx, "data must be a string, Uint8Array, or ArrayBuffer");
    }
    if (size > JS_FS_MAX_FILE_SIZE) {
        JS_FreeValue(ctx, buffer);
        JS_FreeCString(ctx, string);
        JS_FreeCString(ctx, path);
        return JS_ThrowRangeError(ctx, "fs.write: data exceeds %d bytes",
                                  JS_FS_MAX_FILE_SIZE);
    }
    app_filesystem_writer_t *writer = NULL;
    bool created = false;
    const app_filesystem_write_options_t options = {
        .condition = APP_FILESYSTEM_WRITE_UPSERT,
    };
    esp_err_t err = app_filesystem_write_begin(filesystem(ctx), path, &options,
                                                &writer, &created);
    if (err == ESP_OK) {
        err = app_filesystem_write(writer, data, size);
        if (err == ESP_OK) {
            char sha256[APP_FILESYSTEM_SHA256_HEX_SIZE];
            err = app_filesystem_write_commit(writer, NULL, sha256);
            writer = NULL;
        }
    }
    if (writer) app_filesystem_write_abort(writer);
    JS_FreeValue(ctx, buffer);
    JS_FreeCString(ctx, string);
    JS_FreeCString(ctx, path);
    if (err != ESP_OK) return fs_error(ctx, "write", err);
    return JS_UNDEFINED;
}

static JSValue js_fs_mkdir(JSContext *ctx, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void)this_value;
    const char *path = path_arg(ctx, argc, argv, 0, false);
    if (!path) return JS_EXCEPTION;
    if (read_only_path(path)) {
        JS_FreeCString(ctx, path);
        return read_only_error(ctx, "mkdir");
    }
    esp_err_t err = app_filesystem_mkdir(filesystem(ctx), path);
    JS_FreeCString(ctx, path);
    return err == ESP_OK ? JS_UNDEFINED : fs_error(ctx, "mkdir", err);
}

static JSValue js_fs_remove(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void)this_value;
    const char *path = path_arg(ctx, argc, argv, 0, false);
    if (!path) return JS_EXCEPTION;
    if (read_only_path(path)) {
        JS_FreeCString(ctx, path);
        return read_only_error(ctx, "remove");
    }
    esp_err_t err = app_filesystem_delete(filesystem(ctx), path, NULL);
    JS_FreeCString(ctx, path);
    return err == ESP_OK ? JS_UNDEFINED : fs_error(ctx, "remove", err);
}

static const JSCFunctionListEntry fs_methods[] = {
    JS_CFUNC_DEF("list", 1, js_fs_list),
    JS_CFUNC_DEF("stat", 1, js_fs_stat),
    JS_CFUNC_DEF("readText", 1, js_fs_read_text),
    JS_CFUNC_DEF("readBytes", 1, js_fs_read_bytes),
    JS_CFUNC_DEF("write", 2, js_fs_write),
    JS_CFUNC_DEF("mkdir", 1, js_fs_mkdir),
    JS_CFUNC_DEF("remove", 1, js_fs_remove),
};

static esp_err_t fs_module_init(js_runtime_t *runtime, JSContext *ctx, void *opaque)
{
    (void)runtime; (void)opaque;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(global) || JS_IsException(object)) {
        JS_FreeValue(ctx, global);
        JS_FreeValue(ctx, object);
        return ESP_ERR_NO_MEM;
    }
    if (JS_SetPropertyFunctionList(ctx, object, fs_methods,
            sizeof(fs_methods) / sizeof(fs_methods[0])) < 0) {
        JS_FreeValue(ctx, global);
        JS_FreeValue(ctx, object);
        return ESP_FAIL;
    }
    int result = JS_SetPropertyStr(ctx, global, "fs", object);
    JS_FreeValue(ctx, global);
    return result < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t js_fs_register(app_filesystem_t *fs)
{
    if (!fs) return ESP_ERR_INVALID_ARG;
    if (s_registered) return ESP_ERR_INVALID_STATE;
    esp_err_t err = js_runtime_register_module(fs_module_init, NULL, fs);
    if (err == ESP_OK) {
        s_filesystem = fs;
        s_registered = true;
    }
    return err;
}
