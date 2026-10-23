"""Stage the owner's generated C/H on an ephemeral private Actions runner.

Game data and generated sources never enter the repository. The download URL
is a repository secret and is never printed. No compilation is attempted if
the required input is missing or its pinned digest does not match.
"""
from pathlib import Path, PurePosixPath
import hashlib
import io
import os
import re
import stat
import urllib.parse
import urllib.request
import zipfile

MAX_ZIP = 128 * 1024 * 1024
MAX_EXPANDED = 512 * 1024 * 1024


def extract_sources(data, destination):
    destination = Path(destination)
    if len(data) > MAX_ZIP:
        raise ValueError('Arquivo de fontes maior que 128 MiB.')
    selected, seen, expanded = [], set(), 0
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for item in archive.infolist():
            path = PurePosixPath(item.filename)
            if '\\' in item.filename or path.is_absolute() or '..' in path.parts:
                raise ValueError('Caminho invalido no ZIP de fontes.')
            if item.is_dir():
                continue
            mode = item.external_attr >> 16
            if stat.S_IFMT(mode) not in (0, stat.S_IFREG):
                raise ValueError('Tipo de arquivo invalido no ZIP.')
            parts = path.parts
            if parts[:3] == ('src', 'recomp', 'gen'):
                parts = parts[3:]
            if len(parts) != 1 or not re.fullmatch(r'[A-Za-z0-9_.-]+\.(?:c|h)', parts[0]):
                raise ValueError('O ZIP deve conter apenas os .c/.h de src/recomp/gen.')
            name = parts[0]
            if name.casefold() in seen:
                raise ValueError('Nome repetido no ZIP de fontes.')
            seen.add(name.casefold())
            expanded += item.file_size
            if expanded > MAX_EXPANDED:
                raise ValueError('Fontes descompactados excedem 512 MiB.')
            selected.append((name, archive.read(item)))
    if 'recomp_dispatch.c' not in {name for name, _ in selected}:
        raise ValueError('Falta recomp_dispatch.c: nao e um pacote completo de fontes gerados.')
    destination.mkdir(parents=True, exist_ok=True)
    for name, contents in selected:
        (destination / name).write_bytes(contents)
    return len(selected)


def main():
    url = os.environ.get('DEFJAM_GUEST_SOURCES_URL', '')
    digest = os.environ.get('DEFJAM_GUEST_SOURCES_SHA256', '').lower()
    if not url or not digest:
        raise SystemExit('APK bloqueado: faltam DEFJAM_GUEST_SOURCES_URL e DEFJAM_GUEST_SOURCES_SHA256. '
                         'Forneca um ZIP privado dos .c/.h de src/recomp/gen; nao envie ISO/XBE ao repositorio.')
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password:
        raise SystemExit('A origem dos fontes deve usar HTTPS sem credenciais no endereco.')
    if not re.fullmatch('[0-9a-f]{64}', digest):
        raise SystemExit('SHA256 de fontes invalido.')
    try:
        with urllib.request.urlopen(url, timeout=90) as response:
            data = response.read(MAX_ZIP + 1)
    except Exception:
        raise SystemExit('Nao foi possivel baixar o pacote privado de fontes gerados.') from None
    if len(data) > MAX_ZIP or hashlib.sha256(data).hexdigest() != digest:
        raise SystemExit('Pacote de fontes maior que o limite ou SHA256 diferente.')
    count = extract_sources(data, Path(__file__).resolve().parents[1] / 'src/recomp/gen')
    print(f'Entradas privadas verificadas: {count} arquivos C/H para esta compilacao.')


if __name__ == '__main__':
    main()
