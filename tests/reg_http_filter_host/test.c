#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reg_http_filter.h"

size_t app_registers_area_count(app_register_area_t area)
{
    static const size_t capacities[] = {256, 256, 1024, 1024};
    return area <= APP_REGISTER_AREA_D ? capacities[area] : 0;
}

esp_err_t app_register_address_parse(const char *text, app_register_address_t *out)
{
    static const char areas[] = "XYMD";
    if (!text || !out || !text[1] || (text[1] == '0' && text[2]))
        return ESP_ERR_INVALID_ARG;
    const char *area = strchr(areas, text[0]);
    if (!area || text[1] < '0' || text[1] > '9') return ESP_ERR_INVALID_ARG;
    errno = 0;
    char *end = NULL;
    unsigned long index = strtoul(text + 1, &end, 10);
    app_register_area_t code = (app_register_area_t)(area - areas);
    if (errno || *end || index >= app_registers_area_count(code))
        return ESP_ERR_INVALID_ARG;
    *out = (app_register_address_t){code, (uint16_t)index};
    return ESP_OK;
}

static app_register_address_t address(const char *text)
{
    app_register_address_t result;
    assert(app_register_address_parse(text, &result) == ESP_OK);
    return result;
}

static reg_http_filter_t parse(const char *query)
{
    char copy[600];
    assert(strlen(query) < sizeof(copy));
    strcpy(copy, query);
    reg_http_filter_t filter;
    assert(reg_http_filter_parse(copy, &filter) == ESP_OK);
    return filter;
}

static void reject(const char *query)
{
    char copy[600];
    assert(strlen(query) < sizeof(copy));
    strcpy(copy, query);
    reg_http_filter_t filter;
    assert(reg_http_filter_parse(copy, &filter) == ESP_ERR_INVALID_ARG);
    reg_http_filter_destroy(&filter);
}

int main(void)
{
    reg_http_filter_t filter = parse("");
    assert(reg_http_filter_matches(&filter, address("X0")));
    assert(reg_http_filter_matches(&filter, address("D1023")));
    reg_http_filter_destroy(&filter);

    filter = parse("addresses=M10,D100%2CD101");
    assert(reg_http_filter_matches(&filter, address("M10")));
    assert(reg_http_filter_matches(&filter, address("D101")));
    assert(!reg_http_filter_matches(&filter, address("D102")));
    reg_http_filter_destroy(&filter);

    filter = parse("count=1024&start=D0"); // Larger than the transaction limit.
    assert(reg_http_filter_matches(&filter, address("D0")));
    assert(reg_http_filter_matches(&filter, address("D1023")));
    assert(!reg_http_filter_matches(&filter, address("M0")));
    reg_http_filter_destroy(&filter);

    filter = parse("addresses=X0&start=D100&count=2");
    assert(reg_http_filter_matches(&filter, address("X0")));
    assert(reg_http_filter_matches(&filter, address("D100")));
    assert(reg_http_filter_matches(&filter, address("D101")));
    assert(!reg_http_filter_matches(&filter, address("D102")));
    reg_http_filter_destroy(&filter);

    const char *invalid[] = {
        "addresses=", "addresses=D100,", "addresses=D0100", "addresses=A0",
        "addresses=D1024", "addresses=D100&addresses=D101", "addresses=D%GG",
        "start=D100", "count=2", "start=D100&count=0", "start=D100&count=925",
        "start=D0&count=010", "start=D0&count=1&count=2", "unknown=D100",
        "addresses=D100&", "addresses=D100%00D101",
        "format=compact", "format=full",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        reject(invalid[i]);
    puts("Register SSE filter tests passed");
}
