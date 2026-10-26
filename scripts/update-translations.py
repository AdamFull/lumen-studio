"""Refreshes translations/*.ts from the sources (the update_translations target) and keeps
translations/lumen_en.ts in its own style, so the diff holds only real changes.

Usage: python scripts/update-translations.py [build-dir]   (default build/dev)
cmake is taken from PATH, or from the CMAKE environment variable.

lumen_en.ts only holds the English plural forms. Qt's tooling updates a copy of it in
<build>/src/app/.lupdate and writes it back with lconvert, so its locations come out relative to
that copy ("../../../../../src/...") and its layout differs from the file's. Both are put back here;
lumen_de.ts is written by lupdate directly and needs nothing.
"""
import os, re, shutil, subprocess, sys


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build = os.path.join(root, sys.argv[1] if len(sys.argv) > 1 else os.path.join('build', 'dev'))
    cmake = os.environ.get('CMAKE') or shutil.which('cmake')
    if not cmake:
        sys.exit('cmake is not on PATH; set CMAKE to its path')
    subprocess.run([cmake, '--build', build, '--target', 'update_translations'], check=True)

    path = os.path.join(root, 'translations', 'lumen_en.ts')
    with open(path, encoding='utf-8', newline='') as f:
        text = f.read()
    text = re.sub(r'filename="(?:\.\./)+src/', 'filename="../src/', text)
    text = re.sub(r'(<location [^>]*?)"/>', r'\1" />', text)
    # The plural forms on the translation's line; only the whitespace between the tags goes (a form
    # can hold line breaks of its own).
    text = re.sub(r'<translation( type="unfinished")?>\n\s*((?:<numerusform>.*?</numerusform>\n\s*)+)</translation>',
                  lambda m: '<translation%s>%s</translation>'
                  % (m.group(1) or '', re.sub(r'</numerusform>\s*<numerusform>', '</numerusform><numerusform>', m.group(2).strip())),
                  text, flags=re.S)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)
    print('translations/lumen_en.ts: locations and layout kept')


if __name__ == '__main__':
    main()
