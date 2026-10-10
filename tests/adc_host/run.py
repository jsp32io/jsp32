#!/usr/bin/env python3
"""Run ADC JS API and driver failure tests without ESP hardware."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
tests = root / "tests/adc_host"
quickjs = root / "components/quickjs/upstream"
with tempfile.TemporaryDirectory(prefix="ost-adc-test-") as directory:
    temp = Path(directory)
    for name in ("sdkconfig.h", "esp_err.h", "js_runtime.h", "driver/gpio.h",
                 "esp_adc/adc_oneshot.h", "esp_adc/adc_cali.h",
                 "esp_adc/adc_cali_scheme.h", "esp_private/esp_gpio_reserve.h"):
        header = temp / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "stubs.h"\n')
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-O1",
               "-D_GNU_SOURCE", '-DCONFIG_VERSION="2025-09-13"',
               "-fno-strict-aliasing", "-Wall", "-Wno-unused-parameter"]
    for include in (temp, tests, quickjs, root / "components/js_adc/include"):
        command += ["-I", str(include)]
    command += [str(tests / "test.c"), str(root / "components/js_adc/js_adc.c")]
    command += [str(quickjs / name) for name in
                ("quickjs.c", "cutils.c", "libregexp.c", "libunicode.c", "dtoa.c")]
    command += ["-lm", "-ldl", "-lpthread", "-o", str(temp / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(temp / "test")], check=True)
