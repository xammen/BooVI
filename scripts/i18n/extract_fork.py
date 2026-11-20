"""Collect the fork's French interface strings into i18n/fork.json, keeping existing translations.

usage: python -B scripts/i18n/extract_fork.py [--check]

The fork writes its QML in French: qsTr("réglages") has no Chinese in it, so extract.py (keyed
on Simplified Chinese) never sees it. This catalog is keyed on the French source text instead.
Each entry: {"fr": source, "ctx": disambiguation (optional), "src": [files], "en": ...}, plus
"zh-CN", "zh-TW" or "ja" where a translation exists. A language without one shows the English,
so the interface never falls back to French outside French (UiLanguage.cpp).

Entries no source uses any more are dropped. --check lists missing English and placeholder
mismatches (%1..%9) and writes nothing; it exits 1 when there is any.
"""
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract import HAN, qml_strings  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / 'i18n/fork.json'
LANGS = ('en', 'zh-CN', 'zh-TW', 'ja')


def collect():
    found = {}
    for f in sorted((ROOT / 'qml/Veyra').glob('*.qml')):
        for s, ctx in qml_strings(f.read_text(encoding='utf-8')):
            if s.strip() and not HAN.search(s) and re.search('[A-Za-zÀ-ÿ]', s):
                found.setdefault((s, ctx), set()).add('qml/' + f.name)
    return found


def placeholders(s):
    return sorted(re.findall(r'%\d', s))


def main():
    check = '--check' in sys.argv
    old = {}
    if CATALOG.exists():
        for e in json.loads(CATALOG.read_text(encoding='utf-8'))['entries']:
            old[(e['fr'], e.get('ctx', ''))] = e
    found = collect()
    entries, missing, problems = [], [], []
    for (s, ctx), where in sorted(found.items(), key=lambda kv: (kv[0][0].lower(), kv[0][1])):
        e = {'fr': s}
        if ctx:
            e['ctx'] = ctx
        e['src'] = sorted(where)
        prev = old.get((s, ctx), {})
        for lang in LANGS:
            if prev.get(lang):
                e[lang] = prev[lang]
                if placeholders(prev[lang]) != placeholders(s):
                    problems.append((s, lang))
        if not e.get('en'):
            missing.append(s)
        entries.append(e)
    dropped = sorted(k[0] for k in set(old) - set(found))
    print(f'entries={len(entries)} missing en={len(missing)} dropped={len(dropped)} placeholder problems={len(problems)}')
    for s in missing[:40]:
        print('  missing:', s)
    for s, lang in problems:
        print(f'  placeholders differ ({lang}):', s)
    if check:
        return 1 if missing or problems else 0
    CATALOG.write_text(json.dumps({'entries': entries}, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
