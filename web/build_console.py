#!/usr/bin/env python3
"""Bundle the offline Web BLE console; no network access is needed."""
from pathlib import Path
ROOT = Path(__file__).resolve().parent

def build():
    html = (ROOT / 'index.template.html').read_text()
    assets = {
        '<!-- XTERM_CSS -->': ('style', 'vendor/xterm.css'),
        '<!-- APP_CSS -->': ('style', 'console.css'),
        '<!-- XTERM_JS -->': ('script', 'vendor/xterm.js'),
        '<!-- FIT_JS -->': ('script', 'vendor/addon-fit.js'),
        '<!-- APP_JS -->': ('script', 'console.js'),
    }
    for marker, (tag, path) in assets.items():
        if html.count(marker) != 1:
            raise ValueError(f'Expected exactly one {marker}')
        text = (ROOT / path).read_text().replace(f'</{tag}', f'<\\/{tag}')
        html = html.replace(marker, f'<{tag}>\n{text}\n</{tag}>')
    licenses = '\n\n'.join((ROOT / 'vendor' / name).read_text() for name in ('LICENSE-xterm', 'LICENSE-addon-fit'))
    html = html.replace('</head>', '<!-- Third-party MIT licenses\n' + licenses.replace('-->', '-- >') + '\n-->\n</head>')
    (ROOT / 'index.html').write_text(html)
    print(f'Built {ROOT / "index.html"}: {len(html.encode())} bytes')

if __name__ == '__main__':
    build()
