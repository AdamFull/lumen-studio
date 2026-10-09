"""Checks translations/*.ts: reports unfinished entries and translations whose placeholders (%1, %n),
rich-text tags or mnemonic ampersands differ from the source. Exit code 1 when any are found.

Usage: python scripts/check-translations.py [--allow-unfinished]
Refresh the files first with: cmake --build <build-dir> --target update_translations
"""
import glob, os, re, sys
import xml.etree.ElementTree as ET

PH = re.compile(r'%(?:L?\d|n)')
TAG = re.compile(r'<[^>]+>')
MNEMONIC = re.compile(r'(?<!&)&(?=[^&\s])')


def problems(source, text):
    out = []
    if sorted(PH.findall(source)) != sorted(PH.findall(text)):
        out.append('placeholders')
    if sorted(TAG.findall(source)) != sorted(TAG.findall(text)):
        out.append('tags')
    if len(MNEMONIC.findall(source)) != len(MNEMONIC.findall(text)):
        out.append('mnemonic')
    return out


def main():
    allow_unfinished = '--allow-unfinished' in sys.argv
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    failed = False
    for path in sorted(glob.glob(os.path.join(root, 'translations', '*.ts'))):
        unfinished, bad, total = 0, [], 0
        for ctx in ET.parse(path).getroot().findall('context'):
            for msg in ctx.findall('message'):
                total += 1
                source = msg.findtext('source') or ''
                tr = msg.find('translation')
                if tr.get('type') == 'unfinished':
                    unfinished += 1
                    continue
                forms = [f.text or '' for f in tr.findall('numerusform')] or [tr.text or '']
                for form in forms:
                    issues = problems(source, form)
                    if issues:
                        bad.append(f'{ctx.findtext("name")}: {source!r} -> {form!r} ({", ".join(issues)})')
        name = os.path.basename(path)
        print(f'{name}: {total} messages, {unfinished} unfinished, {len(bad)} invalid')
        for line in bad:
            print('  ' + line)
        failed = failed or bool(bad) or (unfinished > 0 and not allow_unfinished)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
