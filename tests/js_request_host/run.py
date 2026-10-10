#!/usr/bin/env python3
"""Test the shared request lifecycle against the project's actual QuickJS."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
quickjs = root / "components/quickjs/upstream"
with tempfile.TemporaryDirectory(prefix="ost-js-request-test-") as directory:
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-O1", "-D_GNU_SOURCE",
               '-DCONFIG_VERSION="2025-09-13"', "-fno-strict-aliasing",
               "-isystem", str(quickjs), "-I", str(root / "components/js_runtime/include")]
    if os.environ.get("SANITIZE"):
        command += ["-fsanitize=" + os.environ["SANITIZE"], "-fno-omit-frame-pointer"]
    # Keep strict warnings on our component and tests, independently of upstream.
    objects = []
    for name in ("tests/js_request_host/test.c", "components/js_runtime/js_request.c"):
        output = Path(directory) / (Path(name).stem + ".o")
        subprocess.run(command + ["-Wall", "-Wextra", "-Werror", "-c", str(root / name),
                                  "-o", str(output)], check=True)
        objects.append(str(output))
    binary = Path(directory) / "test"
    subprocess.run(command + objects + [str(quickjs / name) for name in
        ("quickjs.c", "cutils.c", "libregexp.c", "libunicode.c", "dtoa.c")] +
        ["-lm", "-ldl", "-lpthread", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
