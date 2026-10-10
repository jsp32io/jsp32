#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stubs.h"
#include "js_fs.h"

struct app_filesystem { int mounted; };
struct app_filesystem_reader { size_t offset; };
struct app_filesystem_writer { uint8_t data[65536]; size_t size; };
static struct app_filesystem store = {1};
static uint8_t file_data[65536];
static size_t file_size;
static bool file_exists;
static bool directory_exists;
static js_runtime_module_init_fn module_init;
static void *module_opaque;

const char *esp_err_to_name(esp_err_t error) { (void)error; return "MOCK_ERROR"; }
esp_err_t js_runtime_register_module(js_runtime_module_init_fn init,
                                     js_runtime_module_deinit_fn deinit, void *opaque)
{ (void)deinit; module_init = init; module_opaque = opaque; return ESP_OK; }

static bool valid(const char *path)
{ return path && path[0] == '/' && !strstr(path, "..") && !strchr(path, '\\'); }

esp_err_t app_filesystem_list(app_filesystem_t *fs, const char *path,
    const char *after, app_filesystem_file_info_t *entries, size_t capacity,
    size_t *count, bool *more)
{
    assert(fs == &store && capacity >= 1);
    if (!valid(path) || (after && !valid(after))) return ESP_ERR_INVALID_ARG;
    if (strcmp(path, "/") && strcmp(path, "/dir") &&
        strcmp(path, "/plugins") && strcmp(path, "/stdlib")) {
        return ESP_ERR_NOT_FOUND;
    }
    *count = 0; *more = false;
    if (strcmp(path, "/") == 0 && directory_exists &&
        (!after || strcmp(after, "/dir") < 0)) {
        strcpy(entries[(*count)++].path, "/dir");
        entries[0].directory = true;
    }
    if (strcmp(path, "/") == 0 && file_exists &&
        (!after || strcmp(after, "/test.txt") < 0)) {
        if (*count == capacity) { *more = true; return ESP_OK; }
        strcpy(entries[*count].path, "/test.txt");
        entries[*count].size = file_size;
        ++*count;
    }
    return ESP_OK;
}

esp_err_t app_filesystem_stat(app_filesystem_t *fs, const char *path,
    app_filesystem_file_info_t *info, char *digest)
{
    assert(fs == &store);
    if (!valid(path)) return ESP_ERR_INVALID_ARG;
    if (strcmp(path, "/test.txt") || !file_exists) return ESP_ERR_NOT_FOUND;
    strcpy(info->path, path); info->size = file_size;
    memset(digest, 'a', 64); digest[64] = 0;
    return ESP_OK;
}

esp_err_t app_filesystem_read_open(app_filesystem_t *fs, const char *path,
    app_filesystem_reader_t **out, app_filesystem_file_info_t *info, char *digest)
{
    esp_err_t err = app_filesystem_stat(fs, path, info, digest);
    if (err != ESP_OK) return err;
    *out = calloc(1, sizeof(**out));
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t app_filesystem_read(app_filesystem_reader_t *reader, void *buffer,
    size_t size, size_t *count)
{
    *count = file_size - reader->offset;
    if (*count > size) *count = size;
    memcpy(buffer, file_data + reader->offset, *count);
    reader->offset += *count;
    return ESP_OK;
}
void app_filesystem_read_close(app_filesystem_reader_t *reader) { free(reader); }

esp_err_t app_filesystem_write_begin(app_filesystem_t *fs, const char *path,
    const app_filesystem_write_options_t *options, app_filesystem_writer_t **out,
    bool *created)
{
    assert(fs == &store && options->condition == APP_FILESYSTEM_WRITE_UPSERT);
    if (!valid(path)) return ESP_ERR_INVALID_ARG;
    if (strcmp(path, "/test.txt")) return ESP_ERR_NOT_FOUND;
    *created = !file_exists;
    *out = calloc(1, sizeof(**out));
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t app_filesystem_write(app_filesystem_writer_t *writer, const void *data,
    size_t size)
{
    if (size > sizeof(writer->data) - writer->size) return APP_FILESYSTEM_ERR_FULL;
    memcpy(writer->data + writer->size, data, size);
    writer->size += size;
    return ESP_OK;
}
esp_err_t app_filesystem_write_commit(app_filesystem_writer_t *writer,
    const char *expected, char *digest)
{
    assert(!expected);
    memcpy(file_data, writer->data, writer->size);
    file_size = writer->size; file_exists = true;
    memset(digest, 'a', 64); digest[64] = 0;
    free(writer);
    return ESP_OK;
}
void app_filesystem_write_abort(app_filesystem_writer_t *writer) { free(writer); }
esp_err_t app_filesystem_mkdir(app_filesystem_t *fs, const char *path)
{
    assert(fs == &store);
    if (!valid(path)) return ESP_ERR_INVALID_ARG;
    if (strcmp(path, "/dir")) return ESP_ERR_NOT_FOUND;
    if (directory_exists) return APP_FILESYSTEM_ERR_EXISTS;
    directory_exists = true; return ESP_OK;
}
esp_err_t app_filesystem_delete(app_filesystem_t *fs, const char *path,
    const char *digest)
{
    assert(fs == &store && !digest);
    if (!valid(path)) return ESP_ERR_INVALID_ARG;
    if (!strcmp(path, "/test.txt") && file_exists) {
        file_exists = false; return ESP_OK;
    }
    if (!strcmp(path, "/dir") && directory_exists) {
        directory_exists = false; return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static void evaluate(JSContext *ctx, const char *source)
{
    JSValue result = JS_Eval(ctx, source, strlen(source), "fs-test", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        fprintf(stderr, "FAIL: %s\n%s\n", source, message);
        JS_FreeCString(ctx, message); JS_FreeValue(ctx, error);
        abort();
    }
    JS_FreeValue(ctx, result);
}

int main(void)
{
    assert(js_fs_register(NULL) == ESP_ERR_INVALID_ARG);
    assert(js_fs_register(&store) == ESP_OK);
    assert(js_fs_register(&store) == ESP_ERR_INVALID_STATE);
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    assert(module_init(NULL, ctx, module_opaque) == ESP_OK);
    evaluate(ctx,
        "function assert(v) { if (!v) throw Error('assertion'); }"
        "function throws(f, type) { let caught = false; try { f(); } catch(e) { caught = true; assert(e instanceof type); } assert(caught); }"
        "assert(fs.list().entries.length === 0);"
        "fs.mkdir('/dir');"
        "fs.write('/test.txt', 'hello');"
        "assert(fs.readText('/test.txt') === 'hello');"
        "assert(new Uint8Array(fs.readBytes('/test.txt'))[0] === 104);"
        "assert(fs.stat('/test.txt').size === 5);"
        "assert(fs.list().entries.length === 2);"
        "assert(fs.list('/', '/dir').entries[0].path === '/test.txt');"
        "assert(fs.list('/plugins').entries.length === 0);"
        "assert(fs.list('/stdlib').entries.length === 0);"
        "throws(() => fs.write('/plugins/new.js', 'x'), InternalError);"
        "throws(() => fs.write('/stdlib/time.js', 'x'), InternalError);"
        "throws(() => fs.write('/main.js', 'x'), InternalError);"
        "throws(() => fs.mkdir('/plugins'), InternalError);"
        "throws(() => fs.mkdir('/plugins/new'), InternalError);"
        "throws(() => fs.mkdir('/stdlib/new'), InternalError);"
        "throws(() => fs.remove('/plugins'), InternalError);"
        "throws(() => fs.remove('/plugins/new.js'), InternalError);"
        "throws(() => fs.remove('/stdlib/time.js'), InternalError);"
        "assert(fs.readText('/test.txt') === 'hello');"
        "throws(() => fs.write('/plugins2/new.js', 'x'), ReferenceError);"
        "fs.write('/test.txt', new Uint8Array([9, 8, 7]).subarray(1));"
        "assert(new Uint8Array(fs.readBytes('/test.txt')).join(',') === '8,7');"
        "fs.write('/test.txt', new ArrayBuffer(0));"
        "assert(fs.stat('/test.txt').size === 0);"
        "throws(() => fs.write('/test.txt', new Uint8Array(65537)), RangeError);"
        "throws(() => fs.readText('test.txt'), TypeError);"
        "throws(() => fs.list('/bad'), ReferenceError);"
        "throws(() => fs.remove('/main.js'), InternalError);"
        "fs.remove('/test.txt'); fs.remove('/dir');"
        "assert(fs.list().entries.length === 0);");
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    return 0;
}
