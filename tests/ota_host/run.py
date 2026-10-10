#!/usr/bin/env python3
"""Exercise the production OTA buffer and progress logic with mocked flash/crypto."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
tests = root / 'tests/ota_host'
with tempfile.TemporaryDirectory(prefix='ost-ota-test-') as directory:
    temp = Path(directory)
    for name in ('esp_err.h','esp_app_desc.h','esp_flash_partitions.h','esp_ota_ops.h',
                 'esp_heap_caps.h','freertos/FreeRTOS.h','freertos/semphr.h','psa/crypto.h'):
        path = temp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "stubs.h"\n')
    subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(temp), '-I', str(tests), '-I', str(root/'components/app_ota/include'),
                    str(tests/'test.c'), str(root/'components/app_ota/app_ota.c'),
                    '-o', str(temp/'test')], check=True)
    subprocess.run([str(temp/'test')], check=True)
