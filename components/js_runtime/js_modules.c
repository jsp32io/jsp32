#include "js_runtime_internal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_filesystem.h"
#include "sdkconfig.h"

static bool ends_in_js(const char *path)
{
    size_t length = strlen(path);
    return length >= 3 && strcmp(path + length - 3, ".js") == 0;
}

static bool append_segment(char path[APP_FILESYSTEM_PATH_SIZE], size_t *length,
                           const char *segment, size_t segment_length)
{
    if (segment_length == 0) return false;
    if (segment_length == 1 && segment[0] == '.') return true;
    if (segment_length == 2 && segment[0] == '.' && segment[1] == '.') {
        if (*length == 0) return false;
        while (*length > 0 && path[*length - 1] != '/') --*length;
        if (*length > 0) --*length;
        path[*length] = '\0';
        return true;
    }
    for (size_t i = 0; i < segment_length; ++i) {
        unsigned char ch = (unsigned char)segment[i];
        if (ch < 0x20 || ch == 0x7f || ch == '\\' || ch == ':' ||
            ch == '?' || ch == '#') return false;
    }
    if (*length + 1 + segment_length >= APP_FILESYSTEM_PATH_SIZE) return false;
    path[(*length)++] = '/';
    memcpy(path + *length, segment, segment_length);
    *length += segment_length;
    path[*length] = '\0';
    return true;
}

static bool append_path(char path[APP_FILESYSTEM_PATH_SIZE], size_t *length,
                        const char *source)
{
    const char *segment = source;
    for (const char *cursor = source;; ++cursor) {
        if (*cursor != '/' && *cursor != '\0') continue;
        if (!append_segment(path, length, segment, (size_t)(cursor - segment))) {
            return false;
        }
        if (*cursor == '\0') return true;
        segment = cursor + 1;
    }
}

static char *module_normalize(JSContext *ctx, const char *base,
                              const char *name, void *opaque)
{
    (void)opaque;
    if (!name || !name[0]) {
        JS_ThrowReferenceError(ctx, "empty module specifier");
        return NULL;
    }
    char path[APP_FILESYSTEM_PATH_SIZE] = {0};
    size_t length = 0;
    bool absolute = name[0] == '/';
    bool relative = name[0] == '.' && name[1] == '/';
    relative = relative || (name[0] == '.' && name[1] == '.' && name[2] == '/');
    if (relative) {
        if (!base || base[0] != '/') {
            JS_ThrowReferenceError(ctx, "relative import has no file base: %s", name);
            return NULL;
        }
        const char *slash = strrchr(base, '/');
        length = (size_t)(slash - base);
        if (length >= sizeof(path)) {
            JS_ThrowReferenceError(ctx, "module path too long: %s", name);
            return NULL;
        }
        memcpy(path, base, length);
        path[length] = '\0';
    } else if (!absolute) {
        memcpy(path, "/stdlib", sizeof("/stdlib"));
        length = sizeof("/stdlib") - 1;
    }
    const char *source = absolute ? name + 1 : name;
    if (!append_path(path, &length, source) || length == 0) {
        JS_ThrowReferenceError(ctx, "invalid module path: %s", name);
        return NULL;
    }
    if (!absolute && !relative && !ends_in_js(path)) {
        const char *last = strrchr(path, '/');
        if (strchr(last + 1, '.') || length + sizeof(".js") > sizeof(path)) {
            JS_ThrowReferenceError(ctx, "invalid stdlib module name: %s", name);
            return NULL;
        }
        memcpy(path + length, ".js", sizeof(".js"));
    } else if (!ends_in_js(path)) {
        JS_ThrowReferenceError(ctx, "module path must end in .js: %s", name);
        return NULL;
    }
    char *normalized = js_malloc(ctx, strlen(path) + 1);
    if (!normalized) return NULL;
    strcpy(normalized, path);
    return normalized;
}

static JSModuleDef *module_load(JSContext *ctx, const char *path, void *opaque)
{
    js_runtime_t *runtime = opaque;
    if (runtime->file_module_count >= CONFIG_JS_RUNTIME_MAX_FILE_MODULES) {
        JS_ThrowRangeError(ctx, "too many JavaScript modules");
        return NULL;
    }
    app_filesystem_reader_t *reader = NULL;
    app_filesystem_file_info_t info;
    char digest[APP_FILESYSTEM_SHA256_HEX_SIZE];
    esp_err_t err = app_filesystem_read_open(runtime->filesystem, path,
                                            &reader, &info, digest);
    if (err != ESP_OK) {
        JS_ThrowReferenceError(ctx, "cannot open module %s: %s", path,
                               esp_err_to_name(err));
        return NULL;
    }
    const size_t single_limit = (size_t)CONFIG_JS_RUNTIME_MAX_MODULE_SIZE_KB * 1024;
    const size_t total_limit = (size_t)CONFIG_JS_RUNTIME_MAX_MODULE_SOURCE_KB * 1024;
    if (info.size > single_limit || info.size > total_limit -
        runtime->file_module_source_bytes) {
        app_filesystem_read_close(reader);
        JS_ThrowRangeError(ctx, "module source limit exceeded: %s", path);
        return NULL;
    }
    char *source = malloc(info.size + 1);
    if (!source) {
        app_filesystem_read_close(reader);
        JS_ThrowOutOfMemory(ctx);
        return NULL;
    }
    size_t received = 0;
    while (received < info.size) {
        size_t count = 0;
        err = app_filesystem_read(reader, source + received,
                                  info.size - received, &count);
        if (err != ESP_OK || count == 0) break;
        received += count;
    }
    app_filesystem_read_close(reader);
    if (err != ESP_OK || received != info.size) {
        free(source);
        JS_ThrowReferenceError(ctx, "cannot read module %s", path);
        return NULL;
    }
    source[received] = '\0';
    ++runtime->file_module_count;
    runtime->file_module_source_bytes += received;
    JSValue compiled = JS_Eval(ctx, source, received, path,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(source);
    if (JS_IsException(compiled)) {
        --runtime->file_module_count;
        runtime->file_module_source_bytes -= received;
        return NULL;
    }
    return JS_VALUE_GET_PTR(compiled);
}

void js_modules_init(js_runtime_t *runtime)
{
    JS_SetModuleLoaderFunc(runtime->rt, module_normalize, module_load, runtime);
}
