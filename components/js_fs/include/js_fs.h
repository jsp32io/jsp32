#pragma once

#include "app_filesystem.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the global fs object before /main.js is evaluated. */
esp_err_t js_fs_register(app_filesystem_t *filesystem);

#ifdef __cplusplus
}
#endif
