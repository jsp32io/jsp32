#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='jsp32-modbus-tcp-') as directory:
    temp = Path(directory)
    (temp / 'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_INVALID_ARG 1\n#define ESP_ERR_NO_MEM 2\n#define ESP_ERR_NVS_NOT_FOUND 3\nconst char *esp_err_to_name(esp_err_t);\n')
    (temp / 'sdkconfig.h').write_text('#pragma once\n#define CONFIG_APP_REGISTERS_MAX_TRANSACTION 256\n#define CONFIG_MODBUS_TCP_MAX_CLIENTS 2\n#define CONFIG_MODBUS_TCP_TASK_STACK_SIZE 6144\n')
    (temp / 'esp_log.h').write_text('#pragma once\n#define ESP_LOGI(tag,...) ((void)(tag))\n#define ESP_LOGW(tag,...) ((void)(tag))\n#define ESP_LOGE(tag,...) ((void)(tag))\n')
    (temp / 'esp_timer.h').write_text('#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n')
    (temp / 'nvs.h').write_text('#pragma once\n#include <stdint.h>\n#include "esp_err.h"\ntypedef int nvs_handle_t;\n#define NVS_READONLY 0\nesp_err_t nvs_open(const char *,int,nvs_handle_t *);\nesp_err_t nvs_get_u8(nvs_handle_t,const char *,uint8_t *);\nesp_err_t nvs_get_u16(nvs_handle_t,const char *,uint16_t *);\nvoid nvs_close(nvs_handle_t);\n')
    (temp / 'freertos').mkdir()
    (temp / 'freertos/FreeRTOS.h').write_text('#pragma once\n#define pdPASS 1\n#define pdMS_TO_TICKS(ms) (ms)\n')
    (temp / 'freertos/task.h').write_text('#pragma once\nint xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);\nvoid vTaskDelay(unsigned);\n')
    (temp / 'lwip').mkdir()
    (temp / 'lwip/inet.h').write_text('#pragma once\n#include <arpa/inet.h>\n')
    (temp / 'lwip/sockets.h').write_text('#pragma once\n#include <sys/socket.h>\n#include <sys/select.h>\n#include <netinet/in.h>\n#include <unistd.h>\n')
    executable = temp / 'test'
    subprocess.run([
        os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-pthread',
        '-I', str(temp), '-I', str(root / 'components/modbus_tcp/include'),
        '-I', str(root / 'components/app_registers/include'),
        str(root / 'components/modbus_tcp/modbus_tcp_protocol.c'),
        str(root / 'components/modbus_tcp/modbus_tcp.c'),
        str(root / 'tests/modbus_tcp_host/test.c'), '-o', str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
