#!/usr/bin/env python3
"""Compile the real manual handler with HTTP stubs; compare responses to source files."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='ost-man-test-') as directory:
    temp = Path(directory)
    subprocess.run([sys.executable, str(root / 'tools/embed_man.py'),
                    str(root / 'man'), str(temp / 'man_pages.h')], check=True)
    (temp / 'app_http_server.h').write_text('''#pragma once
#include <stddef.h>
#include <stdbool.h>
typedef int esp_err_t;
typedef struct app_http_server app_http_server_t;
typedef struct {const char *uri;} httpd_req_t;
typedef struct {const char *uri; int method; esp_err_t (*handler)(httpd_req_t *);} httpd_uri_t;
#define HTTP_GET 1
#define HTTPD_404_NOT_FOUND 404
esp_err_t app_http_server_register(app_http_server_t *, const char *, const httpd_uri_t *, size_t);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, size_t);
esp_err_t httpd_resp_send_err(httpd_req_t *, int, const char *);
''')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(temp), str(root / 'components/app_http_server/man.c'),
                    str(root / 'tests/man_host/test.c'), '-o', str(temp / 'test')], check=True)
    args = [str(temp / 'test')]
    for page in sorted((root / 'man').rglob('*.md')):
        topic = '.'.join(page.relative_to(root / 'man').with_suffix('').parts)
        args += ['/api/v1/man/' + topic, str(page)]
        if page.name == 'index.md' and page.parent != root / 'man':
            args += ['/api/v1/man/' + topic.removesuffix('.index'), str(page)]
    for uri, page in (
        ('/api/v1/man', 'index.md'),
        ('/api/v1/man/', 'index.md'),
        ('/api/v1/man?x=1', 'index.md'),
        ('/api/v1/man/js?x=1', 'js/index.md'),
        ('/api/v1/man/js.index?x=1', 'js/index.md'),
    ):
        args += [uri, str(root / 'man' / page)]
    subprocess.run(args, check=True)
