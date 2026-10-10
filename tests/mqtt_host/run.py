#!/usr/bin/env python3
from pathlib import Path
import os
import subprocess
import tempfile
import json

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="jsp32-mqtt-test-") as directory:
    temp = Path(directory)
    (temp / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 1\n#define ESP_ERR_NO_MEM 2\n")
    (temp / "app_http_server.h").write_text("#pragma once\ntypedef struct app_http_server app_http_server_t;\n")
    (temp / "sdkconfig.h").write_text("#pragma once\n#define CONFIG_APP_REGISTERS_MAX_TRANSACTION 256\n")
    declaration = subprocess.check_output(["node", "--input-type=module", "-e",
        "import {readFile} from 'node:fs/promises';"
        "const source=await readFile('rootfs/plugins/io/index.js','utf8');"
        "const {default:plugin}=await import('data:text/javascript,'+encodeURIComponent(source));"
        "console.log(JSON.stringify(plugin.telemetry));"], cwd=root, text=True).strip()
    (temp / "io_telemetry.h").write_text("static const char IO_TELEMETRY_JSON[] = " + json.dumps(declaration) + ";\n")
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror"]
    for include in (temp, root / "components/app_mqtt", root / "components/app_mqtt/include",
                    root / "components/app_registers/include", root / "components/cjson"):
        command += ["-I", str(include)]
    command += [str(root / "tests/mqtt_host/test_report.c"),
                str(root / "components/app_mqtt/mqtt_report.c"),
                str(root / "components/cjson/cJSON.c"), "-lm", "-o", str(temp / "report")]
    subprocess.run(command, check=True)
    subprocess.run([str(temp / "report")], check=True)

    service = temp / "service"
    service.mkdir()
    for name in ("esp_err.h", "app_http_server.h", "mqtt_client.h", "esp_crt_bundle.h",
                 "esp_log.h", "esp_mac.h", "esp_timer.h", "nvs.h", "js_runtime.h"):
        (service / name).write_text('#include "stubs.h"\n')
    (service / "sdkconfig.h").write_text("#define CONFIG_APP_REGISTERS_MAX_TRANSACTION 256\n")
    (service / "freertos").mkdir()
    for name in ("semphr.h", "task.h"):
        (service / "freertos" / name).write_text('#include "stubs.h"\n')
    quickjs = root / "components/quickjs/upstream"
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-O1", "-D_GNU_SOURCE",
               '-DCONFIG_VERSION="2025-09-13"', "-fno-strict-aliasing", "-Wall",
               "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
               "-Wno-missing-field-initializers", "-Wno-implicit-fallthrough"]
    for include in (service, root / "tests/mqtt_host", quickjs,
                    root / "components/app_mqtt", root / "components/app_mqtt/include",
                    root / "components/js_telemetry/include",
                    root / "components/app_registers/include", root / "components/cjson"):
        command += ["-I", str(include)]
    command += [str(root / name) for name in (
        "tests/mqtt_host/test_service.c", "components/app_mqtt/app_mqtt.c",
        "components/app_mqtt/mqtt_report.c", "components/app_mqtt/mqtt_http.c",
        "components/js_telemetry/js_telemetry.c", "components/cjson/cJSON.c")]
    # Apply strict warnings to our code; compile vendored QuickJS separately.
    for name in ("quickjs.c", "cutils.c", "libregexp.c", "libunicode.c", "dtoa.c"):
        obj = service / (name + ".o")
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-D_GNU_SOURCE",
                        '-DCONFIG_VERSION="2025-09-13"', "-fno-strict-aliasing", "-w",
                        "-I", str(quickjs), "-c", str(quickjs / name), "-o", str(obj)], check=True)
        command.append(str(obj))
    command += ["-lm", "-ldl", "-lpthread", "-o", str(service / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(service / "test")], check=True)
