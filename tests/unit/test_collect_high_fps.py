import runpy,zipfile,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def test_timing_collection_without_assets(tmp_path):
    tool=runpy.run_path(str(ROOT/'scripts/collect-high-fps.py'))
    gen=tmp_path/'src/recomp/gen';gen.mkdir(parents=True)
    code='''void sub_00011CD0(void) {
/* } */ const char *s="}"; // }
if (s) { sub_000D75E0(); }
}
void sub_000D75E0(void) { sub_000ABCDE(); }
void sub_000ABCDE(void) { /* private fixture */ }
'''
    (gen/'recomp_0000.c').write_text(code)
    (tmp_path/'game').mkdir();(tmp_path/'game/default.xbe').write_bytes(b'NOT GAME DATA')
    output=tmp_path/'diagnostic.zip';found,missing=tool['collect'](tmp_path,output)
    assert len(found)==3 and missing
    with zipfile.ZipFile(output) as z:
        names=z.namelist();assert not any(n.endswith('.xbe') for n in names)
        assert 'sub_000D75E0();' in z.read('functions/sub_00011CD0.c').decode()
        assert json.loads(z.read('manifest.json'))['targets']
