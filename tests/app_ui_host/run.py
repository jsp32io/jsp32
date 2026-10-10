#!/usr/bin/env python3
"""Compile actual UI routes/assets and verify page fallback versus API/resource 404."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='jsp32-ui-test-') as directory:
    temp = Path(directory)
    for header in ('esp_err.h', 'esp_http_server.h'):
        (temp / header).write_text('#include "sdk.h"\n')
    declarations = []
    for name, symbol in [('index.html', 'index_html'), ('style.css', 'style_css'), ('app.js', 'app_js')]:
        data = (root / 'components/app_ui/assets' / name).read_bytes() + b'\0'
        chunks = ['"' + ''.join(f'\\{byte:03o}' for byte in data[i:i + 80]) + '"'
                  for i in range(0, len(data), 80)]
        declarations.append(f'const char asset_{symbol}[] asm("_binary_{symbol}_start") =\n'
                            + '\n'.join(chunks) + ';\n')
    (temp / 'assets.c').write_text(''.join(declarations))
    subprocess.run([
        os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
        '-I', str(temp), '-I', str(root / 'tests/network_host'),
        '-I', str(root / 'components/app_ui/include'),
        '-I', str(root / 'components/app_http_server/include'),
        '-I', str(root / 'components/cjson'),
        str(root / 'components/app_ui/app_ui.c'), str(temp / 'assets.c'),
        str(root / 'tests/app_ui_host/test.c'), '-o', str(temp / 'test')
    ], check=True)
    subprocess.run([str(temp / 'test')], check=True)
