"""HD installer publication/rollback boundaries with synthetic textures only."""
import importlib.util
import json
from pathlib import Path
import sys

import pytest

pytest.importorskip('PIL')
pytest.importorskip('numpy')
ROOT = Path(__file__).parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


builder = load('hd_builder', ROOT / 'scripts/build-hd-pack.py')
engine = load('hd_setup_engine', ROOT / 'setup/engine.py')


@pytest.fixture(autouse=True)
def hd_enabled_on_every_host(monkeypatch):
    # The generation and activation logic is host-independent; only the renderer
    # that draws the packs differs, and setup refuses the option where it cannot.
    monkeypatch.setattr(engine, 'HD_SUPPORTED', True)


def corpus(tmp_path):
    from PIL import Image
    folder = tmp_path / 'corpus'
    (folder / 'textures').mkdir(parents=True)
    assets = []
    for ident, category, alpha in [('a'*64,'UI and menus',[0,255]),('b'*64,'Fighters',[0,255])]:
        p = folder / 'textures' / (ident+'.png')
        Image.new('RGBA',(8,4),(80,120,240,128)).save(p)
        assets.append(dict(id=ident, upscaled='textures/'+p.name, upscaled_hash=builder.file_hash(p),
                           width=2,height=1,category=category,alpha=alpha))
    (folder / 'corpus.json').write_text(json.dumps(dict(corpus_id='c'*64,assets=assets,totals={'unresolved':1})))
    return folder


def test_assemble_preserves_pixels_and_separates_material_mips_and_runtime_aliases(tmp_path):
    original = corpus(tmp_path)
    result = builder.assemble(original,tmp_path/'mods',{'d'*64:'a'*64})
    assert result['images']==2 and result['unresolved']==1
    opacity, channels = [tmp_path/'mods'/name for name in result['packs']]
    assert 'png_mips=opacity' in (opacity/'manifest.ini').read_text()
    assert ('d'*64+'=textures/'+'a'*64+'.png') in (opacity/'manifest.ini').read_text()
    assert 'png_mips=channels' in (channels/'manifest.ini').read_text()
    assert (opacity/'textures'/('a'*64+'.png')).read_bytes()==(original/'textures'/('a'*64+'.png')).read_bytes()
    assert builder.assemble(original,tmp_path/'mods',{'d'*64:'a'*64})==result


def test_corrupt_output_does_not_publish_manifest(tmp_path):
    original=corpus(tmp_path)
    (original/'textures'/('a'*64+'.png')).write_bytes(b'broken')
    with pytest.raises(ValueError,match='checksum'):
        builder.assemble(original,tmp_path/'mods')
    assert not list((tmp_path/'mods').glob('*/manifest.ini'))


def test_unowned_pack_is_not_overwritten_and_invalid_alias_is_rejected(tmp_path):
    original=corpus(tmp_path)
    destination=tmp_path/'mods'/('faithful-hd-'+'c'*12+'-opacity')
    destination.mkdir(parents=True)
    sentinel=destination/'user-file'
    sentinel.write_bytes(b'keep')
    with pytest.raises(ValueError,match='not owned'):
        builder.assemble(original,tmp_path/'mods')
    assert sentinel.read_bytes()==b'keep'
    with pytest.raises(ValueError,match='correlation'):
        builder.assemble(original,tmp_path/'another',{'unsafe/path':'a'*64})


@pytest.mark.parametrize('redirect', ['pack','textures'])
def test_owned_pack_with_redirected_directory_cannot_publish_outside_mods(tmp_path,redirect):
    original=corpus(tmp_path)
    mods=tmp_path/'mods';mods.mkdir()
    pack=mods/('faithful-hd-'+'c'*12+'-opacity')
    outside=tmp_path/'outside';outside.mkdir()
    owner=outside if redirect=='pack' else pack
    owner.mkdir(exist_ok=True)
    (owner/'builder.json').write_text(json.dumps(dict(schema=1,product='DefJamFaithfulHD',recipe_id='c'*64,png_mips='opacity')))
    link=pack if redirect=='pack' else pack/'textures'
    if sys.platform=='win32':
        import _winapi
        _winapi.CreateJunction(str(outside),str(link))
    else:
        link.symlink_to(outside,target_is_directory=True)
    before={p.name:p.read_bytes() for p in outside.iterdir() if p.is_file()}
    with pytest.raises(ValueError,match='not owned|must not be a link'):
        builder.assemble(original,mods)
    assert {p.name:p.read_bytes() for p in outside.iterdir() if p.is_file()}==before
    assert not list(outside.glob('**/*.png'))


def test_enable_keeps_unrelated_settings_comments_and_user_packs(tmp_path):
    p=tmp_path/'settings.ini'
    p.write_text('; keep comment\n[display]\nscale=2\n[textures]\n; textures comment [with brackets]\nenabled=0\npacks=custom;old\ncache_mb=512\n[input]\nrumble=1\n')
    name='faithful-hd-'+'f'*12+'-opacity'
    engine.enable_hd_packs(tmp_path,[name],['old'])
    result=p.read_text()
    assert '; keep comment' in result and '; textures comment' in result
    assert 'packs=custom;'+name in result and 'enabled=1' in result
    assert 'cache_mb=512' in result and '[input]\nrumble=1' in result
    assert '[display]\nscale=2' in result


def test_hd_step_unselected_is_noop_and_failure_does_not_enable(tmp_path):
    from types import SimpleNamespace
    obj=object.__new__(engine.Engine)
    obj.args=SimpleNamespace(hd_textures=False)
    obj.build_hd_textures()
    obj.args.hd_textures=True
    obj.source=tmp_path/'source';obj.data=tmp_path/'data';obj.version_root=tmp_path/'version';obj.install=tmp_path/'install'
    obj.python=Path('python');obj.event=lambda message:None
    def fail(*args,**kwargs):raise engine.SetupError('cancelled',6)
    obj.run=fail
    with pytest.raises(engine.SetupError):obj.build_hd_textures()
    assert not (obj.data/'settings.ini').exists() and not hasattr(obj,'hd_packs')


def test_successful_hd_step_prepares_without_changing_active_settings(tmp_path):
    from types import SimpleNamespace
    obj=object.__new__(engine.Engine)
    obj.args=SimpleNamespace(hd_textures=True)
    obj.source=tmp_path/'source';obj.data=tmp_path/'data';obj.version_root=tmp_path/'version';obj.install=tmp_path/'install'
    obj.data.mkdir();obj.version_root.mkdir();obj.python=Path('python');obj.event=lambda message:None
    name='faithful-hd-'+'a'*12+'-channels'
    calls=[]
    def run(argv,**kwargs):
        calls.append((argv,kwargs))
        engine.write_json(obj.version_root/'hd-pack-build.json',{'packs':[name],'images':1})
    obj.run=run
    obj.build_hd_textures()
    assert calls[0][1]['progress'] and obj.hd_packs==[name]
    assert not (obj.data/'settings.ini').exists()


def test_padded_and_duplicate_texture_sections_keep_effective_user_packs(tmp_path):
    path=tmp_path/'settings.ini'
    path.write_text('[ textures ]\nenabled=0\npacks=obsolete\ncache_mb=128\n[display]\nscale=4\n[textures]\nenabled=0\npacks=custom\ncache_mb=512\n')
    name='faithful-hd-'+'b'*12+'-opacity'
    engine.enable_hd_packs(tmp_path,[name])
    result=path.read_text()
    assert result.count('enabled=1')==2
    assert result.count('packs=custom;'+name)==2
    assert 'cache_mb=128' in result and 'cache_mb=512' in result
    assert '[display]\nscale=4' in result


@pytest.mark.parametrize('failure', ['shortcuts', 'cancel', 'receipt', None])
def test_activation_publishes_hd_with_receipts_or_preserves_previous_bytes(tmp_path, monkeypatch, failure):
    from types import SimpleNamespace
    if failure=='shortcuts' and engine.IS_LINUX:
        pytest.skip('Linux activation runs no command; a shortcut it cannot write is reported, not raised')
    obj=object.__new__(engine.Engine)
    obj.args=SimpleNamespace(no_shortcuts=False,no_desktop_shortcut=False,cancel_file=None)
    obj.install=tmp_path/'install';obj.data=tmp_path/'data';obj.payload=tmp_path/'payload'
    for path in (obj.install,obj.data,obj.payload):path.mkdir()
    obj.source=obj.install/'versions/new/source'
    obj.manifest=dict(version='0.6.0',source_commit='a'*40,toolkit_commit='b'*40)
    obj.env={};obj.event=lambda message:None
    obj.hd_packs=['faithful-hd-'+'c'*12+'-opacity']
    (obj.payload/'DefJamLauncher.exe').write_bytes(b'synthetic launcher')
    if not engine.IS_WINDOWS:
        template=obj.payload/'launcher'/engine.LAUNCHER_BUNDLE
        (template/'Contents/MacOS').mkdir(parents=True)
        (obj.payload/'launcher/launcher.sh').write_text('#!/bin/sh\nexec @EXECUTABLE@\n')
        monkeypatch.setenv('HOME',str(tmp_path/'home'));(tmp_path/'home/Desktop').mkdir(parents=True)
        monkeypatch.setenv('XDG_DATA_HOME',str(tmp_path/'home/.local/share'))
    previous=dict(hd_packs=['faithful-hd-'+'d'*12+'-opacity'],version='0.5.1')
    (obj.install/'installed.json').write_text(json.dumps(previous))
    (obj.install/'installed.ini').write_bytes(b'old receipt')
    (obj.data/'settings.ini').write_bytes(b'; keep\r\n[textures]\r\nenabled=0\r\npacks=custom\r\n')
    paths=[obj.install/'installed.json',obj.install/'installed.ini',obj.data/'settings.ini']
    before={p:p.read_bytes() for p in paths}
    def run(*args,**kwargs):
        if failure=='shortcuts':raise engine.SetupError('shortcut failure')
    def cancelled():
        if failure=='cancel':raise engine.SetupError('cancelled',6)
    obj.run=run;obj.cancelled=cancelled
    real_write=engine.write_json
    def write(path,value):
        if failure=='receipt':raise OSError('receipt publication failed')
        real_write(path,value)
    monkeypatch.setattr(engine,'write_json',write)
    if failure=='shortcuts' and engine.IS_WINDOWS:
        obj.activate()
        assert json.loads(paths[0].read_text())['hd_packs']==obj.hd_packs
    elif failure:
        with pytest.raises((engine.SetupError,OSError)):obj.activate()
        assert {p:p.read_bytes() for p in paths}==before
    else:
        obj.activate()
        assert json.loads(paths[0].read_text())['hd_packs']==obj.hd_packs
        assert 'packs=custom;'+obj.hd_packs[0] in paths[2].read_text()


def test_corrupt_linked_pack_edit_survives_corpus_resume_and_is_rejected(tmp_path,monkeypatch):
    from PIL import Image
    import struct
    import hd_corpus
    original=Image.new('RGBA',(2,1),(30,60,90,255))
    header=bytearray(16);header[0]=0x7d;struct.pack_into('<HH',header,4,2,1)
    blob=b'SHPX'+struct.pack('<II',48,1)+bytes(4)+b'test'+struct.pack('<I',24)+header+original.tobytes('raw','BGRA')
    request=next(hd_corpus.hd.shpx_images(blob,decode=False))[0]
    request['source']='screens/synthetic.xsh'
    work=tmp_path/'work'
    for name in ('originals','textures','thumbs','records'):(work/name).mkdir(parents=True)
    monkeypatch.setattr(hd_corpus.hd,'unswizzle',hd_corpus.hd.unswizzle)
    monkeypatch.setattr(hd_corpus.hd,'padded',hd_corpus.hd.padded)
    record=hd_corpus.process_leaf(blob,[request],str(work),{'id':'synthetic'})[0]
    (work/'corpus.json').write_text(json.dumps({'corpus_id':'c'*64,'assets':[record],'totals':{'unresolved':0}}))
    result=builder.assemble(work,tmp_path/'mods')
    installed=tmp_path/'mods'/result['packs'][0]/'textures'/(record['id']+'.png')
    # A link is expected on NTFS; skip the linked-repair case on other filesystems.
    import os
    if not os.path.samefile(installed,work/record['upscaled']):pytest.skip('filesystem has no hard links')
    installed.write_bytes(b'owner-edited texture')
    repaired=hd_corpus.process_leaf(blob,[request],str(work),{'id':'synthetic'})[0]
    assert builder.file_hash(work/repaired['upscaled'])==record['upscaled_hash']
    assert installed.read_bytes()==b'owner-edited texture'
    with pytest.raises(ValueError,match='changed'):builder.assemble(work,tmp_path/'mods')
