"""Wrap the USA cutscene setup in the player's generated C, never the XBE.

The original translation remains intact as a separately named function.  Direct
and indirect calls continue to the public name. Run again after regeneration.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile

MARK = "DEFJAM_SUBTITLES37_GENERATED_WRAPPER"
NAME = "sub_000DA8A0"
SIGNATURE = r"void[ \t]+(?P<symbol>[A-Za-z_][A-Za-z_0-9]*)[ \t]*\([ \t]*void[ \t]*\)[ \t]*(?:\r?\n[ \t]*)?\{"
DEFINITION = re.compile(r"(?m)^[ \t]*" + SIGNATURE.replace("[A-Za-z_][A-Za-z_0-9]*", NAME))
ANNOTATED = re.compile(
    r"(?mi)^/\*\*\r?\n[^\n]*\r?\n[ \t]*\*[ \t]+Original:[ \t]+0x000DA8A0[ \t]+-"
    r"(?:(?!\*/)[\s\S])*\*/\r?\n[ \t]*" + SIGNATURE
)
WRAPPER = r'''

/* DEFJAM_SUBTITLES37_GENERATED_WRAPPER: guest calls retain their original name. */
extern void defjam_subtitles_after_nis_setup(uint32_t state);
void {symbol}(void)
{
    uint32_t state = g_ecx;
    sub_000DA8A0_body37();
    defjam_subtitles_after_nis_setup(state);
}
'''

def prepare(folder):
    if not folder.is_dir():
        raise ValueError('Pasta de C gerado nao existe: ' + str(folder))
    found = []
    for path in sorted(folder.glob('*.c')):
        raw = path.read_bytes()
        try: text = raw.decode('utf-8')
        except UnicodeDecodeError: raise ValueError('Fonte gerado nao e UTF-8: ' + str(path))
        if MARK in text:
            body = re.findall(r'(?m)^void\s+sub_000DA8A0_body37\s*\(\s*void\s*\)\s*\{', text)
            wrapper = re.search(r"(?m)^/\* " + MARK + r"[^\n]*\r?\n[^\n]*\r?\n" + SIGNATURE, text)
            if text.count(MARK) != 1 or len(body) != 1 or not wrapper:
                raise ValueError('Gancho de legendas alterado: ' + str(path))
            found.append((path, raw, raw)); continue
        matches = {m.start("symbol"): m for m in DEFINITION.finditer(text)}
        matches.update({m.start("symbol"): m for m in ANNOTATED.finditer(text)})
        if len(matches) > 1: raise ValueError('Multiplas definicoes em ' + str(path))
        if not matches: continue
        m = next(iter(matches.values()))
        symbol = m.group("symbol")
        if symbol.endswith("_gen"):
            raise ValueError('Funcao de legenda ja possui um wrapper: ' + symbol)
        if 'translation failed' in text[m.start():m.end()+80]:
            raise ValueError('A funcao de legenda esta incompleta no C gerado')
        if 'sub_000DA8A0_body37' in text:
            raise ValueError('Nome reservado ja existe: ' + str(path))
        newline = '\r\n' if '\r\n' in text else '\n'
        signature_start = text.rfind('\n', 0, m.start("symbol")) + 1
        rewritten = (text[:signature_start] + f"void {symbol}(void);" + newline
                     + text[signature_start:m.start("symbol")] + NAME + '_body37'
                     + text[m.end("symbol"):])
        rewritten += WRAPPER.replace('{symbol}', symbol).replace('\n', newline)
        found.append((path, raw, rewritten.encode('utf-8')))
    if len(found) != 1:
        candidates = [p.name for p in sorted(folder.glob('*.c'))
                      if re.search(rb'(?i)(?<![a-f0-9])(?:0x|sub_|loc_)?0*da8a0(?![a-f0-9])', p.read_bytes())]
        raise ValueError('Esperada uma unica funcao no endereco 0x000DA8A0; encontradas %d. Arquivos com o endereco: %s'
                         % (len(found), ', '.join(candidates[:12]) or 'nenhum'))
    return found[0]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('folder', type=Path)
    parser.add_argument('--preview', type=Path, help='Write prepared source and its record without modifying the project')
    a = parser.parse_args()
    try:
        path, before, after = prepare(a.folder)
        if a.preview:
            a.preview.mkdir(parents=True, exist_ok=True)
            out = a.preview / path.name
            out.write_bytes(after)
            record = {'name': path.name, 'prepared_source': str(out.resolve()),
                      'old_sha256': hashlib.sha256(before).hexdigest().upper(),
                      'sha256': hashlib.sha256(after).hexdigest().upper()}
            (a.preview / 'subtitles37.json').write_text(json.dumps(record), encoding='utf-8')
        elif before != after:
            fd, temp = tempfile.mkstemp(prefix=path.name+'.', suffix='.tmp', dir=path.parent)
            try:
                with os.fdopen(fd, 'wb') as stream: stream.write(after)
                if path.read_bytes() != before: raise ValueError('Fonte gerado mudou durante a verificacao')
                os.replace(temp, path)
            finally:
                if os.path.exists(temp): os.unlink(temp)
        print('[SUBTITLE37] fonte gerado verificado: ' + path.name)
    except (OSError, UnicodeError, ValueError) as exc:
        raise SystemExit('[SUBTITLE37] nao foi possivel integrar sem risco: ' + str(exc))

if __name__ == '__main__': main()
