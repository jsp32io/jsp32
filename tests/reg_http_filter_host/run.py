#!/usr/bin/env python3
"""Exercise register SSE subscription parsing without ESP hardware."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="jsp32-reg-filter-") as directory:
    temp = Path(directory)
    (temp / "esp_err.h").write_text("""#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
""")
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-I", str(temp), "-I", str(root / "components/app_registers/include"),
               "-I", str(root / "components/reg_http"),
               str(root / "components/reg_http/reg_http_filter.c"),
               str(root / "tests/reg_http_filter_host/test.c"),
               "-o", str(temp / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(temp / "test")], check=True)
