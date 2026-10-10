#!/usr/bin/env python3
"""Exercise the actual QuickJS net binding with POSIX TCP and pthread workers."""
from pathlib import Path
import os
import json
import subprocess
import tempfile
from tls_fixture import fixtures

root = Path(__file__).resolve().parents[2]
tests = root / "tests/js_net_host"
quickjs = root / "components/quickjs/upstream"
idf = Path(os.environ.get("IDF_PATH", "/opt/esp/v6.1/esp-idf"))
mbedtls = idf / "components/mbedtls"
with tempfile.TemporaryDirectory(prefix="ost-js-net-test-") as directory:
    temp = Path(directory)
    for name in ("esp_err.h", "esp_check.h", "esp_log.h", "sdkconfig.h", "esp_timer.h", "js_runtime.h", "js_event.h",
                 "freertos/FreeRTOS.h", "freertos/queue.h", "freertos/semphr.h",
                 "freertos/task.h", "lwip/inet.h", "lwip/netdb.h", "lwip/sockets.h"):
        header = temp / name
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "stubs.h"\n')
    # ESP-IDF adds two compatibility headers to its mbedTLS 4 PSA sources.
    shims = temp / "shims/mbedtls"
    shims.mkdir(parents=True)
    for name in ("bignum", "ecp"):
        (shims / (name + ".h")).write_text(
            '#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS\n#include "mbedtls/private/' + name + '.h"\n')
    tls_build = temp / "mbedtls-build"
    tls_flags = "-I" + str(shims.parent) + " -DMBEDTLS_SSL_OUT_CONTENT_LEN=4096"
    if os.environ.get("SANITIZE"):
        tls_flags += " -fsanitize=" + os.environ["SANITIZE"] + " -fno-omit-frame-pointer"
    with (temp / "mbedtls.log").open("w") as log:
        subprocess.run(["cmake", "-S", str(mbedtls / "mbedtls"), "-B", str(tls_build),
                        "-DENABLE_TESTING=OFF", "-DENABLE_PROGRAMS=OFF", "-DGEN_FILES=OFF",
                        "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_C_FLAGS=" + tls_flags],
                       check=True, stdout=log, stderr=log)
        subprocess.run(["cmake", "--build", str(tls_build), "-j8"], check=True, stdout=log, stderr=log)
    servers, fixture = fixtures(temp)
    # The real ESP-IDF bundle verifier uses this test system root, independently
    # of the custom roots supplied through JavaScript.
    subprocess.run([os.environ.get("PYTHON", "python3"),
                    str(mbedtls / "esp_crt_bundle/gen_crt_bundle.py"),
                    "--input", str(temp / "system-ca.pem"), "-q"], check=True, cwd=temp)
    (temp / "bundle.S").write_text(
        '.section .rodata\n.balign 4\n.global _binary_x509_crt_bundle_start\n'
        '_binary_x509_crt_bundle_start:\n.incbin "' + str(temp / "x509_crt_bundle") + '"\n'
        '.global _binary_x509_crt_bundle_end\n_binary_x509_crt_bundle_end:\n'
        '.section .note.GNU-stack,"",@progbits\n')
    (temp / "bundle.c").write_text(
        '#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS\n#include "mbedtls/private/ecdsa.h"\n#include "' +
        str(mbedtls / "esp_crt_bundle/esp_crt_bundle.c") + '"\n')
    (temp / "tls.test.js").write_text(
        "globalThis.fixture=" + json.dumps(fixture) + ";\n" + (tests / "tls.test.js").read_text())
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-O1",
               "-D_GNU_SOURCE", '-DCONFIG_VERSION="2025-09-13"', "-DMBEDTLS_SSL_OUT_CONTENT_LEN=4096",
               "-fno-strict-aliasing", "-Wall", "-Wno-unused-parameter"]
    if os.environ.get("SANITIZE"):
        command += ["-fsanitize=" + os.environ["SANITIZE"], "-fno-omit-frame-pointer"]
    for include in (temp, tests, quickjs, root / "components/js_net/include",
                    root / "components/js_net", root / "components/js_runtime/include", mbedtls / "esp_crt_bundle/include",
                    mbedtls / "mbedtls/include", mbedtls / "mbedtls/tf-psa-crypto/include",
                    mbedtls / "mbedtls/tf-psa-crypto/drivers/builtin/include"):
        command += ["-I", str(include)]
    command += [str(tests / "test.c")]
    command += [str(root / "components/js_net" / name) for name in ("js_net.c", "socket_worker.c", "socket_tls.c")]
    command += [str(root / "components/js_runtime/js_request.c")]
    command += [str(temp / "bundle.c"), str(temp / "bundle.S")]
    command += [str(quickjs / name) for name in
                ("quickjs.c", "cutils.c", "libregexp.c", "libunicode.c", "dtoa.c")]
    command += [str(tls_build / "library" / name) for name in
                ("libmbedtls.a", "libmbedx509.a", "libtfpsacrypto.a")]
    command += ["-lm", "-ldl", "-lpthread", "-o", str(temp / "test")]
    subprocess.run(command, check=True)
    try:
        subprocess.run([str(temp / "test"), str(temp / "tls.test.js")], check=True, timeout=30)
        assert "service.test" in servers["system"].sni
        assert "service.test" in servers["private"].sni
    finally:
        for server in servers.values():
            server.close()
