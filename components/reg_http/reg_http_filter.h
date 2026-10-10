#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_registers.h"

typedef struct {
    bool all;
    bool has_range;
    app_register_address_t start;
    size_t count;
    app_register_address_t *addresses;
    size_t address_count;
} reg_http_filter_t;

// Parses a mutable, URL-encoded query. An empty query selects all addresses.
// The caller must release the filter with reg_http_filter_destroy(), including
// after a parse error.
esp_err_t reg_http_filter_parse(char *query, reg_http_filter_t *filter);
bool reg_http_filter_matches(const reg_http_filter_t *filter,
                             app_register_address_t address);
void reg_http_filter_destroy(reg_http_filter_t *filter);
