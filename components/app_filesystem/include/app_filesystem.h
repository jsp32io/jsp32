#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_FILESYSTEM_SHA256_HEX_SIZE 65
#define APP_FILESYSTEM_PATH_SIZE 256
#define APP_FILESYSTEM_ERR_PRECONDITION 0x10001
#define APP_FILESYSTEM_ERR_CHECKSUM     0x10002
#define APP_FILESYSTEM_ERR_FULL         0x10003
#define APP_FILESYSTEM_ERR_EXISTS       0x10004
#define APP_FILESYSTEM_ERR_NOT_EMPTY    0x10005
#define APP_FILESYSTEM_ERR_IS_DIRECTORY 0x10006
#define APP_FILESYSTEM_ERR_NOT_DIRECTORY 0x10007

typedef struct app_filesystem app_filesystem_t;
typedef struct app_filesystem_reader app_filesystem_reader_t;
typedef struct app_filesystem_writer app_filesystem_writer_t;

typedef struct {
    size_t total_bytes;
    size_t used_bytes;
    size_t max_name_bytes;
    bool directories;
} app_filesystem_info_t;

typedef struct {
    char path[APP_FILESYSTEM_PATH_SIZE];
    size_t size;
    bool directory;
} app_filesystem_file_info_t;

typedef enum {
    APP_FILESYSTEM_WRITE_UPSERT,
    APP_FILESYSTEM_WRITE_CREATE_ONLY,
    APP_FILESYSTEM_WRITE_MATCH,
} app_filesystem_write_condition_t;

typedef struct {
    app_filesystem_write_condition_t condition;
    const char *expected_sha256;
} app_filesystem_write_options_t;

esp_err_t app_filesystem_mount(app_filesystem_t **out_filesystem);
esp_err_t app_filesystem_info(app_filesystem_t *filesystem,
                              app_filesystem_info_t *out_info);
esp_err_t app_filesystem_list(app_filesystem_t *filesystem,
                              const char *directory, const char *after,
                              app_filesystem_file_info_t *entries,
                              size_t capacity, size_t *out_count,
                              bool *out_has_more);
esp_err_t app_filesystem_stat(app_filesystem_t *filesystem, const char *path,
                              app_filesystem_file_info_t *out_info,
                              char out_sha256[APP_FILESYSTEM_SHA256_HEX_SIZE]);

esp_err_t app_filesystem_read_open(app_filesystem_t *filesystem,
                                   const char *path,
                                   app_filesystem_reader_t **out_reader,
                                   app_filesystem_file_info_t *out_info,
                                   char out_sha256[APP_FILESYSTEM_SHA256_HEX_SIZE]);
esp_err_t app_filesystem_read(app_filesystem_reader_t *reader, void *buffer,
                              size_t buffer_size, size_t *out_read);
void app_filesystem_read_close(app_filesystem_reader_t *reader);

esp_err_t app_filesystem_write_begin(
    app_filesystem_t *filesystem, const char *path,
    const app_filesystem_write_options_t *options,
    app_filesystem_writer_t **out_writer, bool *out_created);
esp_err_t app_filesystem_write(app_filesystem_writer_t *writer,
                               const void *data, size_t size);
esp_err_t app_filesystem_write_commit(
    app_filesystem_writer_t *writer, const char *expected_content_sha256,
    char out_sha256[APP_FILESYSTEM_SHA256_HEX_SIZE]);
void app_filesystem_write_abort(app_filesystem_writer_t *writer);

esp_err_t app_filesystem_delete(app_filesystem_t *filesystem,
                                const char *path,
                                const char *expected_sha256);
esp_err_t app_filesystem_mkdir(app_filesystem_t *filesystem, const char *path);

esp_err_t app_filesystem_read_file(app_filesystem_t *filesystem,
                                   const char *path, char **out_data,
                                   size_t *out_size);
esp_err_t app_filesystem_unmount(app_filesystem_t *filesystem);

#ifdef __cplusplus
}
#endif
