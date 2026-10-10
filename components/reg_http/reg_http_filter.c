#include "reg_http_filter.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static bool decode_value(char *value)
{
    char *write = value;
    for (char *read = value; *read; ++read) {
        char decoded = *read;
        if (*read == '%') {
            if (!read[1] || !read[2]) return false;
            int high = hex_digit(read[1]);
            int low = hex_digit(read[2]);
            if (high < 0 || low < 0 || (high == 0 && low == 0)) return false;
            decoded = (char)((high << 4) | low);
            read += 2;
        } else if (*read == '+') {
            decoded = ' ';
        }
        *write++ = decoded;
    }
    *write = '\0';
    return true;
}

static esp_err_t parse_addresses(char *value, reg_http_filter_t *filter)
{
    if (!*value) return ESP_ERR_INVALID_ARG;
    size_t count = 1;
    for (const char *p = value; *p; ++p) if (*p == ',') ++count;
    filter->addresses = calloc(count, sizeof(*filter->addresses));
    if (!filter->addresses) return ESP_ERR_NO_MEM;
    char *item = value;
    for (;;) {
        char *next = strchr(item, ',');
        if (next) *next = '\0';
        if (app_register_address_parse(item,
                &filter->addresses[filter->address_count]) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        ++filter->address_count;
        if (!next) break;
        item = next + 1;
    }
    return ESP_OK;
}

static bool parse_positive_count(const char *text, size_t *count)
{
    if (*text < '1' || *text > '9') return false;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || !end || *end || parsed == 0 || parsed > UINT16_MAX) return false;
    *count = (size_t)parsed;
    return true;
}

esp_err_t reg_http_filter_parse(char *query, reg_http_filter_t *filter)
{
    if (!filter) return ESP_ERR_INVALID_ARG;
    *filter = (reg_http_filter_t){0};
    if (!query || !*query) {
        filter->all = true;
        return ESP_OK;
    }
    bool has_addresses = false, has_start = false, has_count = false;
    for (char *part = query; part;) {
        char *next = strchr(part, '&');
        if (next) *next++ = '\0';
        char *equals = strchr(part, '=');
        if (!equals) return ESP_ERR_INVALID_ARG;
        *equals++ = '\0';
        if (!decode_value(equals)) return ESP_ERR_INVALID_ARG;
        if (strcmp(part, "addresses") == 0 && !has_addresses) {
            has_addresses = true;
            esp_err_t err = parse_addresses(equals, filter);
            if (err != ESP_OK) return err;
        } else if (strcmp(part, "start") == 0 && !has_start) {
            has_start = true;
            if (app_register_address_parse(equals, &filter->start) != ESP_OK)
                return ESP_ERR_INVALID_ARG;
        } else if (strcmp(part, "count") == 0 && !has_count) {
            has_count = true;
            if (!parse_positive_count(equals, &filter->count))
                return ESP_ERR_INVALID_ARG;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
        part = next;
    }
    if (has_start != has_count) return ESP_ERR_INVALID_ARG;
    if (has_start) {
        size_t capacity = app_registers_area_count(filter->start.area);
        if (filter->count > capacity - filter->start.index)
            return ESP_ERR_INVALID_ARG;
        filter->has_range = true;
    }
    filter->all = !has_addresses && !has_start;
    return ESP_OK;
}

bool reg_http_filter_matches(const reg_http_filter_t *filter,
                             app_register_address_t address)
{
    if (filter->all) return true;
    if (filter->has_range && address.area == filter->start.area &&
        address.index >= filter->start.index &&
        (size_t)(address.index - filter->start.index) < filter->count) {
        return true;
    }
    for (size_t i = 0; i < filter->address_count; ++i) {
        if (filter->addresses[i].area == address.area &&
            filter->addresses[i].index == address.index) return true;
    }
    return false;
}

void reg_http_filter_destroy(reg_http_filter_t *filter)
{
    if (!filter) return;
    free(filter->addresses);
    *filter = (reg_http_filter_t){0};
}
