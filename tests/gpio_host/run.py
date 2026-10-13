#!/usr/bin/env python3
"""Exercise the GPIO JavaScript binding against mocked ESP-IDF drivers."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
tests = root / "tests/gpio_host"
quickjs = root / "components/quickjs/upstream"
with tempfile.TemporaryDirectory(prefix="ost-gpio-test-") as directory:
    temp = Path(directory)
    for name in ("sdkconfig.h", "esp_err.h", "js_runtime.h", "driver/gpio.h",
                 "esp_private/esp_gpio_reserve.h", "esp_timer.h",
                 "freertos/FreeRTOS.h", "soc/soc_caps.h"):
        header = temp / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "stubs.h"\n')
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-O1",
               "-D_GNU_SOURCE", '-DCONFIG_VERSION="2025-09-13"',
               "-fno-strict-aliasing", "-Wall", "-Wno-unused-parameter"]
    for include in (temp, tests, quickjs, root / "components/js_gpio/include",
                    root / "components/js_runtime/include"):
        command += ["-I", str(include)]
    command += [str(tests / "test.c")]
    command += [str(quickjs / name) for name in
                ("quickjs.c", "cutils.c", "libregexp.c", "libunicode.c", "dtoa.c")]
    command += ["-lm", "-ldl", "-lpthread", "-o", str(temp / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(temp / "test")], check=True)
