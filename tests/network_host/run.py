#!/usr/bin/env python3
"""Exercise the actual network manager/store/HTTP handlers with SDK and NVS fakes."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='jsp32-network-test-') as directory:
    temp = Path(directory)
    headers = ['esp_err.h', 'esp_event.h', 'esp_wifi.h', 'esp_wifi_default.h', 'esp_netif.h', 'esp_netif_sntp.h',
               'esp_timer.h', 'esp_log.h', 'esp_mac.h', 'esp_http_server.h', 'sdkconfig.h', 'nvs.h',
               'freertos/FreeRTOS.h', 'freertos/queue.h', 'freertos/semphr.h', 'freertos/task.h',
               'lwip/inet.h', 'lwip/sockets.h']
    for header in headers:
        path = temp / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "sdk.h"\n')
    command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-ffunction-sections', '-fdata-sections',
               '-I', str(temp), '-I', str(root / 'tests/network_host'),
               '-I', str(root / 'components/network_manager/include'),
               '-I', str(root / 'components/app_http_server/include'),
               '-I', str(root / 'components/wifi_tx_power/include'),
               '-I', str(root / 'components/cjson'),
               str(root / 'tests/network_host/test.c'), str(root / 'components/cjson/cJSON.c'),
               '-Wl,--gc-sections', '-lm', '-o', str(temp / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(temp / 'test')], check=True)
