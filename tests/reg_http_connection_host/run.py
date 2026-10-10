#!/usr/bin/env python3
"""Verify idle SSE sockets can be released without a heartbeat write."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="jsp32-reg-connection-") as directory:
    executable = Path(directory) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root / "components/reg_http"),
                    str(root / "components/reg_http/reg_http_connection.c"),
                    str(root / "tests/reg_http_connection_host/test.c"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=5)
