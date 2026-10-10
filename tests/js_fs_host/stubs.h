#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_INVALID_STATE 5
#define APP_FILESYSTEM_ERR_EXISTS 20
#define APP_FILESYSTEM_ERR_NOT_EMPTY 21
#define APP_FILESYSTEM_ERR_IS_DIRECTORY 22
#define APP_FILESYSTEM_ERR_NOT_DIRECTORY 23
#define APP_FILESYSTEM_ERR_FULL 24
#define APP_FILESYSTEM_SHA256_HEX_SIZE 65
#define APP_FILESYSTEM_PATH_SIZE 256

typedef struct app_filesystem app_filesystem_t;
typedef struct app_filesystem_reader app_filesystem_reader_t;
typedef struct app_filesystem_writer app_filesystem_writer_t;
typedef struct {
    char path[APP_FILESYSTEM_PATH_SIZE];
    size_t size;
    bool directory;
} app_filesystem_file_info_t;
typedef enum { APP_FILESYSTEM_WRITE_UPSERT } app_filesystem_write_condition_t;
typedef struct {
    app_filesystem_write_condition_t condition;
    const char *expected_sha256;
} app_filesystem_write_options_t;
typedef struct js_runtime js_runtime_t;
typedef esp_err_t (*js_runtime_module_init_fn)(js_runtime_t *, JSContext *, void *);
typedef void (*js_runtime_module_deinit_fn)(js_runtime_t *, JSContext *, void *);

const char *esp_err_to_name(esp_err_t error);
esp_err_t js_runtime_register_module(js_runtime_module_init_fn, js_runtime_module_deinit_fn, void *);
esp_err_t app_filesystem_list(app_filesystem_t *, const char *, const char *, app_filesystem_file_info_t *, size_t, size_t *, bool *);
esp_err_t app_filesystem_stat(app_filesystem_t *, const char *, app_filesystem_file_info_t *, char *);
esp_err_t app_filesystem_read_open(app_filesystem_t *, const char *, app_filesystem_reader_t **, app_filesystem_file_info_t *, char *);
esp_err_t app_filesystem_read(app_filesystem_reader_t *, void *, size_t, size_t *);
void app_filesystem_read_close(app_filesystem_reader_t *);
esp_err_t app_filesystem_write_begin(app_filesystem_t *, const char *, const app_filesystem_write_options_t *, app_filesystem_writer_t **, bool *);
esp_err_t app_filesystem_write(app_filesystem_writer_t *, const void *, size_t);
esp_err_t app_filesystem_write_commit(app_filesystem_writer_t *, const char *, char *);
void app_filesystem_write_abort(app_filesystem_writer_t *);
esp_err_t app_filesystem_mkdir(app_filesystem_t *, const char *);
esp_err_t app_filesystem_delete(app_filesystem_t *, const char *, const char *);
