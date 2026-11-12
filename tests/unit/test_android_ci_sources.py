"""Private generated-source ZIP boundary; no game bytes in the fixture."""
from pathlib import Path
import importlib.util
import io
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ci_guest_sources', ROOT/'android/ci_guest_sources.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def test_generated_sources_archive_preflight(tmp_path):
    def packed(names):
        data=io.BytesIO()
        with zipfile.ZipFile(data,'w') as archive:
            for name in names:archive.writestr(name,'/* synthetic fixture */\n')
        return data.getvalue()
    good=packed(['src/recomp/gen/recomp_dispatch.c','src/recomp/gen/recomp_types.h'])
    assert module.extract_sources(good,tmp_path/'valid')==2
    for extra in ['../outside.c','C:/outside.c','src/recomp/gen/default.xbe','recomp_dispatch.C','a\\b.c']:
        target=tmp_path/'rejected'
        try:module.extract_sources(packed(['recomp_dispatch.c',extra]),target)
        except ValueError:pass
        else:raise AssertionError(extra)
        assert not target.exists()
