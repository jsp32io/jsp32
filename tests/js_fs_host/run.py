#!/usr/bin/env python3
"""Exercise the JS filesystem binding with a host QuickJS runtime."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
tests = root / "tests/js_fs_host"
quickjs = root / "components/quickjs/upstream"
with tempfile.TemporaryDirectory(prefix="ost-js-fs-test-") as directory:
    temp = Path(directory)
    for name in ("esp_err.h", "js_runtime.h", "app_filesystem.h"):
        (temp / name).write_text('#include "stubs.h"\n')
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-O1",
               "-D_GNU_SOURCE", '-DCONFIG_VERSION="2025-09-13"',
               "-fno-strict-aliasing", "-Wall", "-Wno-unused-parameter"]
    for include in (temp, tests, quickjs, root / "components/js_fs/include"):
        command += ["-I", str(include)]
    command += [str(tests / "test.c"), str(root / "components/js_fs/js_fs.c")]
    command += [str(quickjs / name) for name in
                ("quickjs.c", "cutils.c", "libregexp.c", "libunicode.c", "dtoa.c")]
    command += ["-lm", "-ldl", "-lpthread", "-o", str(temp / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(temp / "test")], check=True)
