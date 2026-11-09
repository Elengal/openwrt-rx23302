#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Собирает статические HTML-страницы документации для GitHub Pages.

GitHub Pages отдаёт только статику в /docs; markdown-файлы без Jekyll
не превращаются в страницы. Этот скрипт конвертирует docs/*.md в HTML
и кладёт рядом. Запуск: python3 tools/build-pages.py
"""
import pathlib
import re
import sys
import markdown

ROOT = pathlib.Path(__file__).resolve().parent.parent
DOCS = ROOT / 'docs'
PAGES = ['index', 'install-tftp', 'install-uart', 'recovery', 'sources']

NAV = [
    ('index', 'Главная'),
    ('install-tftp', 'Установка (TFTP)'),
    ('install-uart', 'Установка (UART)'),
    ('recovery', 'Восстановление'),
    ('sources', 'Исходники'),
]

TEMPLATE = """<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<link rel="stylesheet" href="style.css">
</head>
<body>
<div class="wrap">
<nav>{nav}</nav>
<main>
{body}
</main>
<footer>© Elengal · GPL-2.0 · <a href="https://github.com/Elengal/openwrt-rx23302">GitHub</a></footer>
</div>
</body>
</html>
"""

CSS = """\
:root { color-scheme: dark; }
* { box-sizing: border-box; }
body { margin: 0; background: #0d1117; color: #c9d1d9;
  font: 16px/1.65 -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif; }
.wrap { max-width: 820px; margin: 0 auto; padding: 24px 20px 64px; }
nav { font-size: 14px; padding-bottom: 16px; border-bottom: 1px solid #30363d; margin-bottom: 28px; }
nav a { margin-right: 4px; }
a { color: #58a6ff; text-decoration: none; }
a:hover { text-decoration: underline; }
h1 { font-size: 28px; line-height: 1.25; margin: 24px 0 16px; color: #e6edf3; }
h2 { font-size: 21px; margin: 32px 0 12px; color: #e6edf3; border-bottom: 1px solid #21262d; padding-bottom: 6px; }
h3 { font-size: 17px; margin: 24px 0 8px; color: #e6edf3; }
code { background: #161b22; padding: 2px 5px; border-radius: 4px; font-size: 14px; }
pre { background: #161b22; padding: 14px; border-radius: 6px; overflow-x: auto; }
pre code { background: none; padding: 0; }
blockquote { margin: 16px 0; padding: 8px 16px; border-left: 3px solid #30363d; color: #8b949e; }
table { border-collapse: collapse; margin: 16px 0; width: 100%; }
th, td { border: 1px solid #30363d; padding: 7px 10px; text-align: left; vertical-align: top; }
th { background: #161b22; }
img { max-width: 100%; }
footer { margin-top: 48px; padding-top: 16px; border-top: 1px solid #30363d;
  font-size: 13px; color: #8b949e; }
"""


def strip_front_matter(text: str) -> str:
    lines = text.split('\n')
    if lines and lines[0].strip() == '---':
        for i in range(1, len(lines)):
            if lines[i].strip() == '---':
                return '\n'.join(lines[i + 1:]).lstrip('\n')
    return text


def convert(name: str) -> None:
    src = DOCS / f'{name}.md'
    text = strip_front_matter(src.read_text(encoding='utf-8'))
    md = markdown.Markdown(extensions=['tables', 'fenced_code', 'sane_lists', 'toc'])
    body = md.convert(text)
    # ссылки на markdown -> html
    body = re.sub(r'(href="[^"]*?)\.md(#[^"]*)?"', r'\1.html\2"', body)
    m = re.search(r'<h1[^>]*>(.*?)</h1>', body)
    title = re.sub(r'<[^>]+>', '', m.group(1)) if m else 'OpenWrt для RX-23302'
    nav = ' · '.join(
        f'<a href="{href}.html">{label}</a>' for href, label in NAV
    )
    out = TEMPLATE.format(title=title, nav=nav, body=body)
    (DOCS / f'{name}.html').write_text(out, encoding='utf-8')
    print(f'{name}.md -> {name}.html')


def main() -> int:
    for name in PAGES:
        if not (DOCS / f'{name}.md').exists():
            print(f'нет {name}.md', file=sys.stderr)
            return 1
        convert(name)
    (DOCS / 'style.css').write_text(CSS, encoding='utf-8')
    (DOCS / '.nojekyll').write_text('', encoding='utf-8')
    print('style.css, .nojekyll')
    return 0


if __name__ == '__main__':
    sys.exit(main())
