#include "app_filesystem.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#define FILESYSTEM_BASE_PATH "/storage"
#define FILESYSTEM_PARTITION_LABEL "storage"
#define TRANSACTION_NEW_PATH "/.fs-new"
#define TRANSACTION_OLD_PATH "/.fs-old"
#define TRANSACTION_LOG_PATH "/.fs-txn"

struct app_filesystem {
    SemaphoreHandle_t mutex;
    bool mounted;
};

struct app_filesystem_reader {
    app_filesystem_t *filesystem;
    FILE *file;
};

struct app_filesystem_writer {
    app_filesystem_t *filesystem;
    FILE *file;
    char target[APP_FILESYSTEM_PATH_SIZE];
    psa_hash_operation_t sha256;
    bool sha256_initialized;
    bool created;
};

static const char *TAG = "app_filesystem";
static app_filesystem_t s_filesystem;

static bool is_internal_path(const char *path)
{
    return strncmp(path, "/.fs-", 5) == 0;
}

static bool path_is_valid(const char *path)
{
    if (!path || path[0] != '/' || path[1] == '\0' ||
        strlen(path) >= CONFIG_LITTLEFS_OBJ_NAME_LEN ||
        is_internal_path(path)) {
        return false;
    }
    const char *segment = path + 1;
    for (const unsigned char *p = (const unsigned char *)segment;; ++p) {
        if (*p != '/' && *p != '\0') {
            if (*p == '\\' || *p < 0x20 || *p == 0x7f) {
                return false;
            }
            continue;
        }
        size_t length = (const char *)p - segment;
        if (length == 0 || (length == 1 && segment[0] == '.') ||
            (length == 2 && segment[0] == '.' && segment[1] == '.')) {
            return false;
        }
        if (*p == '\0') {
            return true;
        }
        segment = (const char *)p + 1;
    }
}

static esp_err_t full_path(const char *path, char *buffer, size_t size)
{
    int length = snprintf(buffer, size, "%s%s", FILESYSTEM_BASE_PATH, path);
    return length < 0 || (size_t)length >= size ? ESP_ERR_INVALID_SIZE
                                                : ESP_OK;
}

static bool path_exists_locked(const char *path)
{
    char absolute[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    struct stat info;
    return full_path(path, absolute, sizeof(absolute)) == ESP_OK &&
           stat(absolute, &info) == 0;
}

static esp_err_t remove_locked(const char *path)
{
    char absolute[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    if (full_path(path, absolute, sizeof(absolute)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (unlink(absolute) == 0 || errno == ENOENT) {
        return ESP_OK;
    }
    return errno == ENOSPC ? APP_FILESYSTEM_ERR_FULL : ESP_FAIL;
}

static esp_err_t rename_locked(const char *source, const char *target)
{
    char source_path[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    char target_path[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    if (full_path(source, source_path, sizeof(source_path)) != ESP_OK ||
        full_path(target, target_path, sizeof(target_path)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    return rename(source_path, target_path) == 0 ? ESP_OK : ESP_FAIL;
}

static void digest_to_hex(const unsigned char digest[32],
                          char output[APP_FILESYSTEM_SHA256_HEX_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        output[i * 2] = digits[digest[i] >> 4];
        output[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    output[64] = '\0';
}

static esp_err_t hash_file(FILE *file,
                           char output[APP_FILESYSTEM_SHA256_HEX_SIZE])
{
    psa_hash_operation_t context = PSA_HASH_OPERATION_INIT;
    unsigned char digest[32];
    unsigned char buffer[1024];
    if (psa_hash_setup(&context, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    rewind(file);
    while (!feof(file)) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count > 0 &&
            psa_hash_update(&context, buffer, count) != PSA_SUCCESS) {
            psa_hash_abort(&context);
            return ESP_FAIL;
        }
        if (ferror(file)) {
            psa_hash_abort(&context);
            return ESP_FAIL;
        }
    }
    size_t digest_size = 0;
    if (psa_hash_finish(&context, digest, sizeof(digest),
                        &digest_size) != PSA_SUCCESS || digest_size != 32) {
        psa_hash_abort(&context);
        return ESP_FAIL;
    }
    digest_to_hex(digest, output);
    rewind(file);
    return ESP_OK;
}

static esp_err_t hash_path_locked(
    const char *path, char output[APP_FILESYSTEM_SHA256_HEX_SIZE],
    size_t *out_size)
{
    char absolute[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    if (full_path(path, absolute, sizeof(absolute)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *file = fopen(absolute, "rb");
    if (!file) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return ESP_FAIL;
    }
    esp_err_t err = hash_file(file, output);
    fclose(file);
    if (err == ESP_OK && out_size) {
        *out_size = (size_t)size;
    }
    return err;
}

static esp_err_t write_transaction_log_locked(const char *target)
{
    char absolute[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    full_path(TRANSACTION_LOG_PATH, absolute, sizeof(absolute));
    FILE *file = fopen(absolute, "wb");
    if (!file) {
        return errno == ENOSPC ? APP_FILESYSTEM_ERR_FULL : ESP_FAIL;
    }
    size_t length = strlen(target) + 1;
    bool ok = fwrite(target, 1, length, file) == length &&
              fflush(file) == 0 && fsync(fileno(file)) == 0;
    fclose(file);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t recover_transaction_locked(void)
{
    if (!path_exists_locked(TRANSACTION_LOG_PATH)) {
        remove_locked(TRANSACTION_NEW_PATH);
        remove_locked(TRANSACTION_OLD_PATH);
        return ESP_OK;
    }
    char absolute[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    full_path(TRANSACTION_LOG_PATH, absolute, sizeof(absolute));
    FILE *log = fopen(absolute, "rb");
    char target[APP_FILESYSTEM_PATH_SIZE] = {0};
    if (!log || fread(target, 1, sizeof(target), log) == 0 ||
        !memchr(target, '\0', sizeof(target)) || !path_is_valid(target)) {
        if (log) {
            fclose(log);
        }
        ESP_LOGE(TAG, "Filesystem transaction log is invalid; preserving recovery files");
        return ESP_ERR_INVALID_CRC;
    }
    fclose(log);
    bool target_exists = path_exists_locked(target);
    bool new_exists = path_exists_locked(TRANSACTION_NEW_PATH);
    bool old_exists = path_exists_locked(TRANSACTION_OLD_PATH);
    if (new_exists && !target_exists) {
        esp_err_t err = rename_locked(TRANSACTION_NEW_PATH, target);
        if (err != ESP_OK) {
            return err;
        }
        target_exists = true;
        new_exists = false;
    } else if (new_exists && target_exists && !old_exists) {
        remove_locked(TRANSACTION_NEW_PATH);
        new_exists = false;
    }
    if (!target_exists && old_exists) {
        esp_err_t err = rename_locked(TRANSACTION_OLD_PATH, target);
        if (err != ESP_OK) {
            return err;
        }
        target_exists = true;
    } else if (target_exists && old_exists) {
        remove_locked(TRANSACTION_OLD_PATH);
    }
    if (!new_exists) {
        remove_locked(TRANSACTION_LOG_PATH);
    }
    return ESP_OK;
}

esp_err_t app_filesystem_mount(app_filesystem_t **out_filesystem)
{
    if (!out_filesystem) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_filesystem.mounted) {
        *out_filesystem = &s_filesystem;
        return ESP_OK;
    }
    if (!s_filesystem.mutex) {
        s_filesystem.mutex = xSemaphoreCreateMutex();
        if (!s_filesystem.mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    const esp_vfs_littlefs_conf_t config = {
        .base_path = FILESYSTEM_BASE_PATH,
        .partition_label = FILESYSTEM_PARTITION_LABEL,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot mount LittleFS partition: %s",
                 esp_err_to_name(err));
        return err;
    }
    s_filesystem.mounted = true;
    xSemaphoreTake(s_filesystem.mutex, portMAX_DELAY);
    err = recover_transaction_locked();
    xSemaphoreGive(s_filesystem.mutex);
    if (err != ESP_OK) {
        app_filesystem_unmount(&s_filesystem);
        return err;
    }

    app_filesystem_info_t info;
    err = app_filesystem_info(&s_filesystem, &info);
    if (err != ESP_OK) {
        app_filesystem_unmount(&s_filesystem);
        return err;
    }
    *out_filesystem = &s_filesystem;
    ESP_LOGI(TAG, "LittleFS mounted at %s (%u/%u bytes used)",
             FILESYSTEM_BASE_PATH, (unsigned)info.used_bytes,
             (unsigned)info.total_bytes);
    return ESP_OK;
}

esp_err_t app_filesystem_info(app_filesystem_t *filesystem,
                              app_filesystem_info_t *out_info)
{
    if (filesystem != &s_filesystem || !filesystem->mounted || !out_info) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t total = 0;
    size_t used = 0;
    xSemaphoreTake(filesystem->mutex, portMAX_DELAY);
    esp_err_t err = esp_littlefs_info(FILESYSTEM_PARTITION_LABEL, &total, &used);
    xSemaphoreGive(filesystem->mutex);
    if (err == ESP_OK) {
        *out_info = (app_filesystem_info_t) {
            .total_bytes = total,
            .used_bytes = used,
            .max_name_bytes = CONFIG_LITTLEFS_OBJ_NAME_LEN - 2,
            .directories = true,
        };
    }
    return err;
}

static void insert_entry(app_filesystem_file_info_t *entries, size_t count,
                         app_filesystem_file_info_t entry)
{
    size_t position = count;
    while (position > 0 &&
           strcmp(entries[position - 1].path, entry.path) > 0) {
        entries[position] = entries[position - 1];
        --position;
    }
    entries[position] = entry;
}

esp_err_t app_filesystem_list(app_filesystem_t *filesystem,
                              const char *directory, const char *after,
                              app_filesystem_file_info_t *entries,
                              size_t capacity, size_t *out_count,
                              bool *out_has_more)
{
    if (filesystem != &s_filesystem || !filesystem->mounted || !entries ||
        capacity == 0 || !out_count || !out_has_more ||
        !directory || (strcmp(directory, "/") != 0 &&
                       !path_is_valid(directory)) ||
        (after && after[0] && !path_is_valid(after))) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(filesystem->mutex, portMAX_DELAY);
    char directory_path[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    if (full_path(directory, directory_path, sizeof(directory_path)) != ESP_OK) {
        xSemaphoreGive(filesystem->mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    DIR *handle = opendir(directory_path);
    if (!handle) {
        int reason = errno;
        xSemaphoreGive(filesystem->mutex);
        return reason == ENOENT ? ESP_ERR_NOT_FOUND
               : reason == ENOTDIR ? APP_FILESYSTEM_ERR_NOT_DIRECTORY
                                  : ESP_FAIL;
    }
    size_t count = 0;
    size_t eligible = 0;
    struct dirent *item;
    while ((item = readdir(handle))) {
        app_filesystem_file_info_t entry = {0};
        int length = snprintf(entry.path, sizeof(entry.path), "%s%s%s",
                              strcmp(directory, "/") == 0 ? "" : directory,
                              item->d_name[0] == '/' ? "" : "/",
                              item->d_name);
        if (length < 0 || (size_t)length >= sizeof(entry.path) ||
            !path_is_valid(entry.path) ||
            (after && after[0] && strcmp(entry.path, after) <= 0)) {
            continue;
        }
        char absolute[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
        struct stat stat_info;
        if (full_path(entry.path, absolute, sizeof(absolute)) != ESP_OK ||
            stat(absolute, &stat_info) != 0) {
            continue;
        }
        entry.size = (size_t)stat_info.st_size;
        entry.directory = S_ISDIR(stat_info.st_mode);
        ++eligible;
        if (count < capacity) {
            insert_entry(entries, count++, entry);
        } else if (strcmp(entry.path, entries[count - 1].path) < 0) {
            entries[count - 1] = entry;
            size_t position = count - 1;
            while (position > 0 &&
                   strcmp(entries[position - 1].path,
                          entries[position].path) > 0) {
                app_filesystem_file_info_t swap = entries[position - 1];
                entries[position - 1] = entries[position];
                entries[position] = swap;
                --position;
            }
        }
    }
    closedir(handle);
    xSemaphoreGive(filesystem->mutex);
    *out_count = count;
    *out_has_more = eligible > capacity;
    return ESP_OK;
}

esp_err_t app_filesystem_stat(app_filesystem_t *filesystem, const char *path,
                              app_filesystem_file_info_t *out_info,
                              char out_sha256[APP_FILESYSTEM_SHA256_HEX_SIZE])
{
    if (filesystem != &s_filesystem || !filesystem->mounted ||
        !path_is_valid(path) || !out_info || !out_sha256) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(filesystem->mutex, portMAX_DELAY);
    size_t size = 0;
    esp_err_t err = hash_path_locked(path, out_sha256, &size);
    xSemaphoreGive(filesystem->mutex);
    if (err == ESP_OK) {
        strlcpy(out_info->path, path, sizeof(out_info->path));
        out_info->size = size;
    }
    return err;
}

esp_err_t app_filesystem_read_open(app_filesystem_t *filesystem,
                                   const char *path,
                                   app_filesystem_reader_t **out_reader,
                                   app_filesystem_file_info_t *out_info,
                                   char out_sha256[APP_FILESYSTEM_SHA256_HEX_SIZE])
{
    if (filesystem != &s_filesystem || !filesystem->mounted ||
        !path_is_valid(path) || !out_reader || !out_info || !out_sha256) {
        return ESP_ERR_INVALID_ARG;
    }
    app_filesystem_reader_t *reader = calloc(1, sizeof(*reader));
    if (!reader) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(filesystem->mutex, portMAX_DELAY);
    reader->filesystem = filesystem;
    char absolute[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    full_path(path, absolute, sizeof(absolute));
    struct stat stat_info;
    if (stat(absolute, &stat_info) == 0 && S_ISDIR(stat_info.st_mode)) {
        app_filesystem_read_close(reader);
        return APP_FILESYSTEM_ERR_IS_DIRECTORY;
    }
    reader->file = fopen(absolute, "rb");
    if (!reader->file) {
        esp_err_t err = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        app_filesystem_read_close(reader);
        return err;
    }
    if (fseek(reader->file, 0, SEEK_END) != 0) {
        app_filesystem_read_close(reader);
        return ESP_FAIL;
    }
    long size = ftell(reader->file);
    esp_err_t err = size < 0 ? ESP_FAIL : hash_file(reader->file, out_sha256);
    if (err != ESP_OK) {
        app_filesystem_read_close(reader);
        return err;
    }
    strlcpy(out_info->path, path, sizeof(out_info->path));
    out_info->size = (size_t)size;
    *out_reader = reader;
    return ESP_OK;
}

esp_err_t app_filesystem_read(app_filesystem_reader_t *reader, void *buffer,
                              size_t buffer_size, size_t *out_read)
{
    if (!reader || !reader->file || !buffer || buffer_size == 0 || !out_read) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_read = fread(buffer, 1, buffer_size, reader->file);
    return ferror(reader->file) ? ESP_FAIL : ESP_OK;
}

void app_filesystem_read_close(app_filesystem_reader_t *reader)
{
    if (!reader) {
        return;
    }
    if (reader->file) {
        fclose(reader->file);
    }
    if (reader->filesystem) {
        xSemaphoreGive(reader->filesystem->mutex);
    }
    free(reader);
}

esp_err_t app_filesystem_write_begin(
    app_filesystem_t *filesystem, const char *path,
    const app_filesystem_write_options_t *options,
    app_filesystem_writer_t **out_writer, bool *out_created)
{
    if (filesystem != &s_filesystem || !filesystem->mounted ||
        !path_is_valid(path) || !options || !out_writer || !out_created ||
        options->condition < APP_FILESYSTEM_WRITE_UPSERT ||
        options->condition > APP_FILESYSTEM_WRITE_MATCH ||
        (options->condition == APP_FILESYSTEM_WRITE_MATCH &&
         (!options->expected_sha256 ||
          strlen(options->expected_sha256) != 64))) {
        return ESP_ERR_INVALID_ARG;
    }
    app_filesystem_writer_t *writer = calloc(1, sizeof(*writer));
    if (!writer) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(filesystem->mutex, portMAX_DELAY);
    esp_err_t recovery = recover_transaction_locked();
    if (recovery != ESP_OK) {
        xSemaphoreGive(filesystem->mutex);
        free(writer);
        return recovery;
    }
    char parent_path[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    full_path(path, parent_path, sizeof(parent_path));
    *strrchr(parent_path, '/') = '\0';
    struct stat parent_info;
    int parent_status = stat(parent_path, &parent_info);
    if (parent_status != 0 || !S_ISDIR(parent_info.st_mode)) {
        esp_err_t err = parent_status != 0 && errno == ENOENT
                            ? ESP_ERR_NOT_FOUND
                            : APP_FILESYSTEM_ERR_NOT_DIRECTORY;
        xSemaphoreGive(filesystem->mutex);
        free(writer);
        return err;
    }
    bool exists = path_exists_locked(path);
    if (exists) {
        char target_path[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
        struct stat target_info;
        full_path(path, target_path, sizeof(target_path));
        if (stat(target_path, &target_info) == 0 &&
            S_ISDIR(target_info.st_mode)) {
            xSemaphoreGive(filesystem->mutex);
            free(writer);
            return APP_FILESYSTEM_ERR_IS_DIRECTORY;
        }
    }
    if ((options->condition == APP_FILESYSTEM_WRITE_CREATE_ONLY && exists) ||
        (options->condition == APP_FILESYSTEM_WRITE_MATCH && !exists)) {
        xSemaphoreGive(filesystem->mutex);
        free(writer);
        return APP_FILESYSTEM_ERR_PRECONDITION;
    }
    if (options->condition == APP_FILESYSTEM_WRITE_MATCH) {
        char actual[APP_FILESYSTEM_SHA256_HEX_SIZE];
        if (hash_path_locked(path, actual, NULL) != ESP_OK ||
            strcasecmp(actual, options->expected_sha256) != 0) {
            xSemaphoreGive(filesystem->mutex);
            free(writer);
            return APP_FILESYSTEM_ERR_PRECONDITION;
        }
    }
    remove_locked(TRANSACTION_NEW_PATH);
    char absolute[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    full_path(TRANSACTION_NEW_PATH, absolute, sizeof(absolute));
    writer->file = fopen(absolute, "wb");
    if (!writer->file) {
        xSemaphoreGive(filesystem->mutex);
        free(writer);
        return errno == ENOSPC ? APP_FILESYSTEM_ERR_FULL : ESP_FAIL;
    }
    writer->filesystem = filesystem;
    writer->created = !exists;
    strlcpy(writer->target, path, sizeof(writer->target));
    writer->sha256 = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&writer->sha256, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        app_filesystem_write_abort(writer);
        return ESP_FAIL;
    }
    writer->sha256_initialized = true;
    *out_created = writer->created;
    *out_writer = writer;
    return ESP_OK;
}

esp_err_t app_filesystem_write(app_filesystem_writer_t *writer,
                               const void *data, size_t size)
{
    if (!writer || !writer->file || (!data && size > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size == 0) {
        return ESP_OK;
    }
    if (fwrite(data, 1, size, writer->file) != size) {
        return errno == ENOSPC ? APP_FILESYSTEM_ERR_FULL : ESP_FAIL;
    }
    return psa_hash_update(&writer->sha256, data, size) == PSA_SUCCESS
               ? ESP_OK : ESP_FAIL;
}

esp_err_t app_filesystem_write_commit(
    app_filesystem_writer_t *writer, const char *expected_content_sha256,
    char out_sha256[APP_FILESYSTEM_SHA256_HEX_SIZE])
{
    if (!writer || !writer->file || !out_sha256 ||
        (expected_content_sha256 && strlen(expected_content_sha256) != 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    unsigned char digest[32];
    bool flushed = fflush(writer->file) == 0 &&
                   fsync(fileno(writer->file)) == 0;
    fclose(writer->file);
    writer->file = NULL;
    size_t digest_size = 0;
    if (!flushed ||
        psa_hash_finish(&writer->sha256, digest, sizeof(digest),
                        &digest_size) != PSA_SUCCESS || digest_size != 32) {
        app_filesystem_write_abort(writer);
        return ESP_FAIL;
    }
    writer->sha256_initialized = false;
    digest_to_hex(digest, out_sha256);
    if (expected_content_sha256 &&
        strcasecmp(expected_content_sha256, out_sha256) != 0) {
        app_filesystem_write_abort(writer);
        return APP_FILESYSTEM_ERR_CHECKSUM;
    }

    esp_err_t err = write_transaction_log_locked(writer->target);
    bool moved_old = false;
    if (err == ESP_OK && !writer->created) {
        remove_locked(TRANSACTION_OLD_PATH);
        err = rename_locked(writer->target, TRANSACTION_OLD_PATH);
        moved_old = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = rename_locked(TRANSACTION_NEW_PATH, writer->target);
    }
    bool keep_transaction = false;
    if (err != ESP_OK && moved_old && !path_exists_locked(writer->target) &&
        rename_locked(TRANSACTION_OLD_PATH, writer->target) != ESP_OK) {
        keep_transaction = true;
    }
    if (err == ESP_OK) {
        remove_locked(TRANSACTION_OLD_PATH);
    } else if (!keep_transaction) {
        remove_locked(TRANSACTION_NEW_PATH);
    }
    if (!keep_transaction) {
        remove_locked(TRANSACTION_LOG_PATH);
    }
    xSemaphoreGive(writer->filesystem->mutex);
    free(writer);
    return err;
}

void app_filesystem_write_abort(app_filesystem_writer_t *writer)
{
    if (!writer) {
        return;
    }
    if (writer->file) {
        fclose(writer->file);
    }
    if (writer->sha256_initialized) {
        psa_hash_abort(&writer->sha256);
    }
    remove_locked(TRANSACTION_NEW_PATH);
    if (writer->filesystem) {
        xSemaphoreGive(writer->filesystem->mutex);
    }
    free(writer);
}

esp_err_t app_filesystem_delete(app_filesystem_t *filesystem,
                                const char *path,
                                const char *expected_sha256)
{
    if (filesystem != &s_filesystem || !filesystem->mounted ||
        !path_is_valid(path) ||
        (expected_sha256 && strlen(expected_sha256) != 64)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(filesystem->mutex, portMAX_DELAY);
    esp_err_t recovery = recover_transaction_locked();
    if (recovery != ESP_OK) {
        xSemaphoreGive(filesystem->mutex);
        return recovery;
    }
    char absolute[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    struct stat info;
    full_path(path, absolute, sizeof(absolute));
    if (stat(absolute, &info) != 0) {
        xSemaphoreGive(filesystem->mutex);
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (S_ISDIR(info.st_mode)) {
        if (expected_sha256) {
            xSemaphoreGive(filesystem->mutex);
            return APP_FILESYSTEM_ERR_PRECONDITION;
        }
        int result = rmdir(absolute);
        int reason = errno;
        xSemaphoreGive(filesystem->mutex);
        if (result == 0) return ESP_OK;
        return reason == ENOTEMPTY || reason == EEXIST
                   ? APP_FILESYSTEM_ERR_NOT_EMPTY : ESP_FAIL;
    }
    if (expected_sha256) {
        char actual[APP_FILESYSTEM_SHA256_HEX_SIZE];
        if (hash_path_locked(path, actual, NULL) != ESP_OK ||
            strcasecmp(actual, expected_sha256) != 0) {
            xSemaphoreGive(filesystem->mutex);
            return APP_FILESYSTEM_ERR_PRECONDITION;
        }
    }
    remove_locked(TRANSACTION_OLD_PATH);
    esp_err_t err = rename_locked(path, TRANSACTION_OLD_PATH);
    if (err == ESP_OK) {
        /* The file is already atomically absent; stale internal cleanup can
         * be retried by the next operation or mount recovery. */
        remove_locked(TRANSACTION_OLD_PATH);
    }
    xSemaphoreGive(filesystem->mutex);
    return err;
}

esp_err_t app_filesystem_mkdir(app_filesystem_t *filesystem, const char *path)
{
    if (filesystem != &s_filesystem || !filesystem->mounted ||
        !path_is_valid(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(filesystem->mutex, portMAX_DELAY);
    char absolute[sizeof(FILESYSTEM_BASE_PATH) + APP_FILESYSTEM_PATH_SIZE];
    full_path(path, absolute, sizeof(absolute));
    int result = mkdir(absolute, 0777);
    int reason = errno;
    xSemaphoreGive(filesystem->mutex);
    if (result == 0) return ESP_OK;
    if (reason == EEXIST) return APP_FILESYSTEM_ERR_EXISTS;
    if (reason == ENOENT) return ESP_ERR_NOT_FOUND;
    if (reason == ENOSPC) return APP_FILESYSTEM_ERR_FULL;
    return ESP_FAIL;
}

esp_err_t app_filesystem_read_file(app_filesystem_t *filesystem,
                                   const char *path, char **out_data,
                                   size_t *out_size)
{
    if (!out_data || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }
    app_filesystem_reader_t *reader = NULL;
    app_filesystem_file_info_t info;
    char sha256[APP_FILESYSTEM_SHA256_HEX_SIZE];
    esp_err_t err = app_filesystem_read_open(filesystem, path, &reader,
                                             &info, sha256);
    if (err != ESP_OK) {
        return err;
    }
    char *data = malloc(info.size + 1);
    if (!data) {
        app_filesystem_read_close(reader);
        return ESP_ERR_NO_MEM;
    }
    size_t total = 0;
    while (total < info.size) {
        size_t count = 0;
        err = app_filesystem_read(reader, data + total, info.size - total,
                                  &count);
        if (err != ESP_OK || count == 0) {
            break;
        }
        total += count;
    }
    app_filesystem_read_close(reader);
    if (err != ESP_OK || total != info.size) {
        free(data);
        return ESP_FAIL;
    }
    data[total] = '\0';
    *out_data = data;
    *out_size = total;
    return ESP_OK;
}

esp_err_t app_filesystem_unmount(app_filesystem_t *filesystem)
{
    if (filesystem != &s_filesystem || !filesystem->mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(filesystem->mutex, portMAX_DELAY);
    esp_err_t err = esp_vfs_littlefs_unregister(FILESYSTEM_PARTITION_LABEL);
    if (err == ESP_OK) {
        filesystem->mounted = false;
    }
    xSemaphoreGive(filesystem->mutex);
    return err;
}
