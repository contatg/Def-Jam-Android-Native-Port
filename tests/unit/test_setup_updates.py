"""Game-free updater policy, network/archive integrity and native crash recovery."""
import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile

import pytest

ROOT = Path(__file__).resolve().parents[2]


def load(name):
    spec = importlib.util.spec_from_file_location('update_' + name, ROOT / 'setup' / (name + '.py'))
    obj = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(obj)
    return obj


releases, transaction, engine = (load(name) for name in ('releases', 'transaction', 'engine'))


def identity(v, capable=True):
    return dict(version=v, **({'updater': releases.CONTRACT} if capable else {}))


@pytest.mark.parametrize('operation,target,old', [
    ('install', identity('0.8.0'), None), ('update', identity('0.8.0'), identity('0.7.2', False)),
    ('update', identity('0.9.0'), identity('0.8.0')), ('downgrade', identity('0.8.0'), identity('0.9.0')),
    ('repair', identity('0.8.0'), identity('0.8.0')), ('rebuild', identity('0.8.0'), identity('0.8.0'))])
def test_supported_operation_pairs(operation, target, old):
    assert releases.policy(operation, target, old) == operation


@pytest.mark.parametrize('operation,target,old', [
    ('downgrade', identity('0.7.2', False), identity('0.9.0')), ('auto', identity('0.8.0'), identity('0.9.0')),
    ('downgrade', identity('0.8.0'), identity('0.9.0', False)), ('update', identity('0.9.0', False), identity('0.8.0')),
    ('repair', identity('0.8.0'), identity('0.9.0')), ('update', identity('0.8.0'), identity('0.8.0')),
    ('rebuild', identity('0.8.0'), None), ('install', identity('0.8.0'), identity('0.8.0')),
    ('downgrade', dict(version='0.8.0', updater={'protocol': 1, 'data_schema': 2}), identity('0.9.0'))])
def test_unsafe_operation_pairs_are_refused(operation, target, old):
    with pytest.raises(ValueError): releases.policy(operation, target, old)


def asset(tag, name, idx=1):
    return dict(name=name, id=idx, state='uploaded', size=10, digest='sha256:'+'a'*64,
                browser_download_url=f'https://github.com/{releases.REPO}/releases/download/{tag}/{name}')


def row(v, platform='macos-arm64', **extra):
    tag='v'+v;stem=f'DefJamSetup-{v}-{platform}'
    return dict(tag_name=tag, body='notes', assets=[asset(tag,stem+ext,i) for i,ext in enumerate((releases.EXTENSIONS[platform],'.sha256','.provenance.json'))], **extra)


def test_catalog_numeric_order_filters_and_unavailable(tmp_path, monkeypatch):
    rows=[row('0.9.0'),row('0.10.0'),row('0.11.0',draft=True),row('0.12.0',prerelease=True),dict(tag_name='v0.1.0',assets=[]),row('0.9.0')]
    monkeypatch.setattr(releases,'read_url',lambda *a:json.dumps(rows).encode())
    result=releases.catalog('macos-arm64',tmp_path)
    assert [r['tag'] for r in result]==['v0.10.0','v0.9.0','v0.1.0']
    assert not result[-1]['available'] and result[-1]['reason']


def test_catalog_pagination_and_ambiguous_assets(tmp_path, monkeypatch):
    first=[row(f'0.8.{i}') for i in range(100)]
    second=row('0.9.0');second['assets'].append(second['assets'][0])
    calls=[]
    def read(url,*a):
        calls.append(url);return json.dumps(first if len(calls)==1 else [second]).encode()
    monkeypatch.setattr(releases,'read_url',read)
    result=releases.catalog('macos-arm64',tmp_path)
    assert len(result)==101 and len(calls)==2 and not result[0]['available']


@pytest.mark.parametrize('url',['http://github.com/x','https://evil.invalid/x','https://user:secret@github.com/x','https://github.com:8443/x'])
def test_download_url_boundary(url):
    with pytest.raises(ValueError):releases.validate_url(url)


@pytest.mark.parametrize('name',['../escape','/escape','C:/escape','a\\b','a/../b','a//b','a.','a:stream'])
def test_archive_rejects_unsafe_paths_before_writing(tmp_path,name):
    out=io.BytesIO()
    info=zipfile.ZipInfo('raw');info.filename=info.orig_filename=name
    with zipfile.ZipFile(out,'w') as package:package.writestr(info,b'data')
    with pytest.raises(ValueError):releases.safe_zip(out.getvalue(),tmp_path/'dest')
    assert not (tmp_path/'escape').exists()


def test_archive_rejects_link_and_duplicate(tmp_path):
    for names in ([('file',0o120777)],[('file',0o100644),('FILE',0o100644)]):
        out=io.BytesIO()
        with zipfile.ZipFile(out,'w') as package:
            for name,mode in names:
                item=zipfile.ZipInfo(name);item.external_attr=mode<<16;package.writestr(item,b'other')
        with pytest.raises(ValueError):releases.safe_zip(out.getvalue(),tmp_path/'dest')


def test_download_checksum_truncation_and_cache(tmp_path, monkeypatch):
    import hashlib
    contents=b'verified setup';a=asset('v0.8.0','setup.exe')
    a.update(size=len(contents),digest='sha256:'+hashlib.sha256(contents).hexdigest())
    class Response(io.BytesIO):
        url=a['browser_download_url']
    class Opener:
        def open(self,*args,**kwargs):return Response(contents)
    monkeypatch.setattr(releases.urllib.request,'build_opener',lambda *a:Opener())
    cached=releases.download(a,tmp_path)
    assert cached.read_bytes()==contents
    monkeypatch.setattr(releases.urllib.request,'build_opener',lambda *a:None)
    assert releases.download(a,tmp_path)==cached
    cached.unlink()
    contents=b'partial'
    monkeypatch.setattr(releases.urllib.request,'build_opener',lambda *a:Opener())
    with pytest.raises(ValueError,match='mismatch'):releases.download(a,tmp_path)
    assert not list(tmp_path.glob('*.partial'))


def test_handoff_preserves_choices_and_prevents_recursion(tmp_path):
    args=argparse.Namespace(install_dir='/literal & % path',data_dir='/data',dump=None,operation='downgrade',toolchain='container',
                           hd_textures=True,no_shortcuts=True,steam_shortcut=True,log='/logs',status_file='/state',cancel_file='/cancel')
    argv=releases.child_arguments(args,tmp_path)
    assert '--bundled-only' in argv and '--release' not in argv
    assert argv[argv.index('--install-dir')+1]=='/literal & % path'
    for flag in ('--hd-textures','--no-shortcuts','--steam-shortcut'):assert flag in argv


@pytest.mark.parametrize('point',['prepared','launcher','settings','receipt-json','receipt-ini','committed'])
@pytest.mark.parametrize('repeat',range(3))
def test_native_hard_exit_recovery_launches_consistent_version(tmp_path,point,repeat):
    root=tmp_path/'app';data=tmp_path/'data';root.mkdir();data.mkdir()
    launcher=root/'Def Jam Recompiled';receipt=root/'installed.json';settings=data/'settings.ini'
    launcher.write_text("print('old')\n");receipt.write_text('old');settings.write_bytes(b'; user settings\r\n')
    script=tmp_path/'worker.py'
    script.write_text('''import importlib.util,os,json,sys
+from pathlib import Path
+spec=importlib.util.spec_from_file_location('txn',sys.argv[1]);t=importlib.util.module_from_spec(spec);spec.loader.exec_module(t)
+root,data,point=Path(sys.argv[2]),Path(sys.argv[3]),sys.argv[4]
+t.HOOK=lambda name:os._exit(91) if name==point else None
+tx=t.Activation(root,data);tx.begin([root/'Def Jam Recompiled',root/'installed.json',data/'settings.ini'])
+(root/'Def Jam Recompiled').write_text("print('new')\\n");t.checkpoint('launcher')
+(data/'settings.ini').write_text('new settings');t.checkpoint('settings')
+(root/'installed.json').write_text('new');t.checkpoint('receipt-json');t.checkpoint('receipt-ini');tx.commit()
+'''.replace('\n+','\n'))
    result=subprocess.run([sys.executable,str(script),str(ROOT/'setup/transaction.py'),str(root),str(data),point])
    assert result.returncode==91
    tx=transaction.Activation(root,data);tx.recover();tx.recover()
    assert subprocess.check_output([sys.executable, str(launcher)], text=True).strip()==receipt.read_text()==('new' if point=='committed' else 'old')
    assert settings.read_bytes()==(b'new settings' if point=='committed' else b'; user settings\r\n')
    assert not (root/transaction.NAME).exists()


def test_recovery_refuses_changed_backup_and_escaping_journal(tmp_path):
    root=tmp_path/'app';data=tmp_path/'data';root.mkdir();data.mkdir()
    target=root/'installed.json';target.write_text('old')
    tx=transaction.Activation(root,data);tx.begin([target]);target.write_text('new')
    (tx.folder/'0').write_text('tampered')
    with pytest.raises(ValueError,match='backup changed'):tx.recover()
    assert target.read_text()=='new' and tx.folder.exists()
    (tx.folder/'0').write_text('old')
    journal=json.loads((tx.folder/'journal.json').read_text());journal['entries'][0]['path']=str(tmp_path/'foreign')
    (tx.folder/'journal.json').write_text(json.dumps(journal))
    with pytest.raises(ValueError,match='Unsafe'):tx.recover()
    assert target.read_text()=='new'


def test_activation_backup_of_an_executable_is_recognised_on_recovery(tmp_path):
    # Regression: a Windows .exe backup is stored without its extension, which changes
    # the mode bits Windows reports; recovery then refused the committed journal.
    root=tmp_path/'app';data=tmp_path/'data';root.mkdir();data.mkdir()
    launcher=root/'DefJamLauncher.exe';launcher.write_bytes(b'MZ old')
    tx=transaction.Activation(root,data);tx.begin([launcher]);launcher.write_bytes(b'MZ new')
    tx.commit();tx.recover()
    assert launcher.read_bytes()==b'MZ new' and not tx.folder.exists()


def test_updater_status_reads_the_provenance_contract(tmp_path, monkeypatch):
    import urllib.error
    release=dict(assets=[asset('v0.7.3','setup.exe',1),asset('v0.7.3','setup.sha256',2),asset('v0.7.3','setup.provenance.json',3)])
    def serve(payload):
        path=tmp_path/'provenance.json';path.write_text(json.dumps(payload))
        monkeypatch.setattr(releases,'download',lambda *a,**k:path)
    contract={'protocol':1,'data_schema':1}
    for payload,expected in ((dict(updater=contract),True),(dict(version='0.7.3'),False),(dict(updater={'protocol':2,'data_schema':1}),False),([],False)):
        serve(payload);assert releases.updater_status(release,tmp_path) is expected
    def offline(*a,**k):raise urllib.error.URLError('offline')
    monkeypatch.setattr(releases,'download',offline)
    assert releases.updater_status(release,tmp_path) is None


def test_list_releases_marks_pre_updater_releases_only_for_existing_installs(tmp_path, monkeypatch):
    from test_setup_engine import installer
    obj=installer(tmp_path)
    rows=[dict(tag='v0.8.0',version='0.8.0',available=True,assets=[]),dict(tag='v0.7.3',version='0.7.3',available=True,assets=[]),
          dict(tag='v0.6.0',version='0.6.0',available=False,assets=[])]
    verdicts={'v0.8.0':True,'v0.7.3':False}
    probed=[]
    monkeypatch.setattr(engine.releases,'catalog',lambda *a,**k:rows)
    def status(row,*a,**k):
        probed.append(row['tag']);return verdicts.get(row['tag'])
    monkeypatch.setattr(engine.releases,'updater_status',status)
    out=tmp_path/'releases.json';args=['--payload',str(obj.payload),'--list-releases','--releases-file',str(out),
                                       '--install-dir',str(obj.install),'--download-cache',str(tmp_path/'cache')]
    assert engine.main(args)==0
    assert out.with_name('releases.json.tags').read_text().splitlines()==['v0.8.0|1|?','v0.7.3|1|?','v0.6.0|0|?'] and probed==[]
    obj.install.mkdir(parents=True,exist_ok=True);(obj.install/'installed.json').write_text('{}')
    assert engine.main(args)==0
    assert out.with_name('releases.json.tags').read_text().splitlines()==['v0.8.0|1|1','v0.7.3|1|0','v0.6.0|0|?']
    assert probed==['v0.8.0','v0.7.3']


def test_installed_dump_without_original_is_verified_and_clean_candidate_is_unique(tmp_path):
    from test_setup_engine import installer
    obj=installer(tmp_path);obj.extract()
    previous=dict(version='0.4.1',source=str(obj.source),data=str(obj.data),executable=str(obj.source/'game.exe'))
    engine.write_json(obj.install/'installed.json',previous)
    manifest=json.loads((obj.payload/'manifest.json').read_text());manifest['updater']=releases.CONTRACT
    engine.write_json(obj.payload/'manifest.json',manifest)
    obj.args.dump=None;obj.args.operation='rebuild'
    first=engine.Engine(obj.args);second=engine.Engine(obj.args)
    assert first.source!=second.source and first.source!=obj.source
    first.prepare_source();first.extract()
    (first.data/'extracted/default.xbe').write_bytes(b'corrupt')
    with pytest.raises(engine.SetupError,match='changed'):first.extract()


def test_recovery_before_receipt_requirements_needs_no_original_dump(tmp_path, monkeypatch):
    from test_setup_engine import installer
    obj=installer(tmp_path);obj.extract()
    receipt=dict(schema=1,version='0.4.1',source=str(obj.source),data=str(obj.data),executable=str(obj.source/'fake'))
    engine.write_json(obj.install/'installed.json',receipt)
    tx=engine.transaction.Activation(obj.install,obj.data);tx.begin([obj.install/'installed.json'])
    (obj.install/'installed.json').unlink()  # Simulate interruption of an older restore implementation.
    called=[]
    monkeypatch.setattr(engine.Engine,'execute',lambda self:called.append(json.loads((self.install/'installed.json').read_text())))
    code=engine.main(['--payload',str(obj.payload),'--install-dir',str(obj.install),'--operation','repair','--bundled-only'])
    assert code==0 and called==[receipt] and not tx.folder.exists()


def test_failure_during_recovery_can_be_retried_without_losing_backup(tmp_path, monkeypatch):
    root=tmp_path/'app';data=tmp_path/'data';root.mkdir();data.mkdir()
    target=root/'installed.json';target.write_text('old')
    tx=transaction.Activation(root,data);tx.begin([target]);target.write_text('new')
    def fail(name):
        if name=='restore:installed.json':raise OSError('recovery interrupted')
    monkeypatch.setattr(transaction,'HOOK',fail)
    with pytest.raises(OSError):tx.recover()
    assert tx.folder.exists() and target.read_text()=='new'
    monkeypatch.setattr(transaction,'HOOK',None);tx.recover()
    assert target.read_text()=='old'


def test_cancelled_handoff_lets_child_clean_separate_stage_session(tmp_path):
    import time
    folder=tmp_path/'payload';(folder/'engine').mkdir(parents=True);(folder/'python/bin').mkdir(parents=True)
    interpreter=folder/'python/bin/python3'
    if os.name=='nt':pytest.skip('POSIX separate sessions; Windows taskkill exercised by native CI')
    interpreter.symlink_to(sys.executable)
    (folder/'manifest.json').write_text(json.dumps({'updater':releases.CONTRACT}))
    cancel=tmp_path/'cancel';ready=tmp_path/'ready';done=tmp_path/'done'
    (folder/'engine/engine.py').write_text('''import subprocess,sys,time,os,signal
from pathlib import Path
args=sys.argv
cancel=Path(args[args.index('--cancel-file')+1]);root=Path(args[args.index('--install-dir')+1])
stage=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'],start_new_session=True)
(root/'ready').write_text(str(stage.pid))
while not cancel.exists():time.sleep(.02)
os.killpg(stage.pid,signal.SIGTERM);stage.wait();(root/'done').write_text('cleaned');sys.exit(6)
''')
    args=argparse.Namespace(install_dir=str(tmp_path),data_dir=None,dump=None,operation='update',toolchain=None,
                           log=None,status_file=None,cancel_file=str(cancel))
    def cancelled():
        if ready.exists():cancel.touch();raise ValueError('cancelled')
    with pytest.raises(ValueError):releases.run_selected(args,'macos-arm64',folder,cancelled)
    assert done.read_text()=='cleaned'
    with pytest.raises(ProcessLookupError):os.kill(int(ready.read_text()),0)


def test_optional_shortcut_failure_does_not_undo_committed_core(tmp_path, monkeypatch):
    from test_setup_engine import installer
    obj=installer(tmp_path)
    obj.args.no_shortcuts=False
    (obj.payload/'DefJamLauncher.exe').write_bytes(b'synthetic')
    if engine.IS_MACOS:
        bundle=obj.payload/'launcher'/engine.LAUNCHER_BUNDLE
        (bundle/'Contents/MacOS').mkdir(parents=True)
    if engine.IS_LINUX:
        (obj.payload/'launcher').mkdir(exist_ok=True)
        (obj.payload/'launcher/launcher.sh').write_text('#!/bin/sh\nexec @EXECUTABLE@\n')
    obj.run=lambda *a,**k:None
    if engine.IS_WINDOWS:
        obj.run=lambda *a,**k:(_ for _ in ()).throw(OSError('shortcut denied'))
    else:
        method='install_launcher_bundle' if engine.IS_MACOS else 'install_launcher_linux'
        original=getattr(obj,method)
        def launch(receipt,core=True):
            if not core:raise OSError('shortcut denied')
            return original(receipt,core)
        monkeypatch.setattr(obj,method,launch)
    obj.activate()
    assert (obj.install/'installed.json').is_file() and not (obj.install/transaction.NAME).exists()


def test_offline_catalog_is_explicit_and_requires_existing_valid_cache(tmp_path,monkeypatch):
    monkeypatch.setattr(releases,'read_url',lambda *a:json.dumps([row('0.8.0')]).encode())
    releases.catalog('macos-arm64',tmp_path)
    def offline(*a):raise releases.urllib.error.URLError('offline')
    monkeypatch.setattr(releases,'read_url',offline)
    assert releases.catalog('macos-arm64',tmp_path,allow_cached=True)[0]['cached']
    with pytest.raises(releases.urllib.error.URLError):releases.catalog('macos-arm64',tmp_path)
    (tmp_path/'releases.json').write_text('[{"tag":"bad"}]')
    with pytest.raises(ValueError):releases.catalog('macos-arm64',tmp_path,allow_cached=True)


def test_latest_uses_github_endpoint_instead_of_numeric_guess(monkeypatch):
    calls=[]
    monkeypatch.setattr(releases,'read_url',lambda url,*a:calls.append(url) or b'{"tag_name":"v0.8.0"}')
    assert releases.latest_tag()=='v0.8.0' and calls==[releases.API+'/latest']


def test_shared_cache_parallel_metadata_and_downloads_use_unique_staging(tmp_path,monkeypatch):
    from concurrent.futures import ThreadPoolExecutor
    import hashlib,time
    body=b'shared verified installer'*1000
    a=asset('v0.8.0','setup.exe');a.update(size=len(body),digest='sha256:'+hashlib.sha256(body).hexdigest())
    class Response(io.BytesIO):
        url=a['browser_download_url']
        def read(self,n=-1):time.sleep(.01);return super().read(n)
    class Opener:
        def open(self,*a,**k):return Response(body)
    monkeypatch.setattr(releases.urllib.request,'build_opener',lambda *a:Opener())
    monkeypatch.setattr(releases,'read_url',lambda *a:json.dumps([row('0.8.0')]).encode())
    def fetch(_):
        releases.catalog('macos-arm64',tmp_path)
        return releases.download(a,tmp_path).read_bytes()
    with ThreadPoolExecutor(max_workers=4) as pool:assert list(pool.map(fetch,range(4)))==[body]*4
    assert not list(tmp_path.glob('*.partial')) and not list(tmp_path.glob('*.pending'))


def test_setup_analysis_does_not_replace_data_cache(tmp_path,monkeypatch):
    spec=importlib.util.spec_from_file_location('setup_test_pipeline',ROOT/'scripts/pipeline.py')
    pipeline=importlib.util.module_from_spec(spec);spec.loader.exec_module(pipeline)
    data=tmp_path/'data';(data/'extracted').mkdir(parents=True);(data/'extracted/default.xbe').write_bytes(b'fixture')
    (data/'analysis').mkdir();sentinel=data/'analysis/default_analysis.json';sentinel.write_bytes(b'user cache')
    tk=tmp_path/'toolkit';tk.mkdir()
    monkeypatch.setattr(pipeline,'run',lambda *a:None)
    pipeline.analyze(argparse.Namespace(data=str(data),toolkit=tk,no_archive=True))
    assert sentinel.read_bytes()==b'user cache' and list((data/'analysis').iterdir())==[sentinel]


def test_selection_lock_is_independent_of_download_cache(tmp_path,monkeypatch):
    from test_setup_engine import installer
    obj=installer(tmp_path)
    locks=[]
    import contextlib
    @contextlib.contextmanager
    def lock(path):
        locks.append(Path(path));yield
    monkeypatch.setattr(engine,'install_lock',lock)
    monkeypatch.setattr(engine.Engine,'execute',lambda self:None)
    for cache in (tmp_path/'cache-a',tmp_path/'cache-b'):
        assert engine.main(['--payload',str(obj.payload),'--install-dir',str(obj.install),'--data-dir',str(obj.data),
                            '--dump',str(obj.dump),'--download-cache',str(cache),'--bundled-only'])==0
    assert locks[0]==locks[1] and locks[0].parent==releases.default_cache().parent/'locks'


@pytest.mark.parametrize('reused', [False, True])
def test_failed_handoff_only_stops_same_stage_process_identity(tmp_path,monkeypatch,reused):
    if os.name=='nt':pytest.skip('POSIX process identity; Windows uses taskkill tree')
    folder=tmp_path/'payload';folder.mkdir();(folder/'manifest.json').write_text(json.dumps({'updater':releases.CONTRACT}))
    class Child:
        pid=42;returncode=4
        polls=0
        def poll(self):
            self.polls+=1
            return None if self.polls==1 else 4
    child=Child();calls=[];scans=[]
    monkeypatch.setattr(releases.subprocess,'Popen',lambda *a,**k:child)
    def ps(*a,**k):
        scans.append(1)
        stamp='Mon Oct 12 12:00:01 2026' if reused and len(scans)>1 else 'Mon Oct 12 12:00:00 2026'
        return argparse.Namespace(stdout='42 1 42 Mon Oct 12 12:00:00 2026\n43 42 43 '+stamp+'\n')
    monkeypatch.setattr(releases.subprocess,'run',ps)
    monkeypatch.setattr(releases.os,'killpg',lambda pid,sig:calls.append(pid))
    args=argparse.Namespace(dump=None)
    monkeypatch.setattr(releases,'child_arguments',lambda *a:[])
    assert releases.run_selected(args,'macos-arm64',folder,lambda:None)==4
    assert calls==([] if reused else [43])


def test_windows_job_closing_reaps_separate_child_tree_before_engine_can_spawn(tmp_path,monkeypatch):
    if os.name!='nt':pytest.skip('Native Windows job object')
    ready=tmp_path/'ready'
    code="import subprocess,sys,time;from pathlib import Path;p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']);Path(sys.argv[1]).write_text(str(p.pid));time.sleep(60)"
    script=tmp_path/'engine.py';script.write_text(code)
    original=releases.WindowsJob
    def delayed_job(child):
        import time
        time.sleep(.5)
        assert not ready.exists(), 'Engine ran before its process job was assigned'
        return original(child)
    monkeypatch.setattr(releases,'WindowsJob',delayed_job)
    child,job=releases.start_selected(Path(sys.executable),['-B',str(script),str(ready)])
    import time,ctypes
    try:
        deadline=time.monotonic()+10
        while not ready.exists() and time.monotonic()<deadline:time.sleep(.02)
        assert ready.exists()
        api=ctypes.WinDLL('kernel32');api.OpenProcess.argtypes=[ctypes.c_uint32,ctypes.c_int,ctypes.c_uint32];api.OpenProcess.restype=ctypes.c_void_p
        api.WaitForSingleObject.argtypes=[ctypes.c_void_p,ctypes.c_uint32];api.CloseHandle.argtypes=[ctypes.c_void_p]
        handle=api.OpenProcess(0x100000,False,int(ready.read_text()));assert handle
        job.close();child.wait(timeout=10)
        assert api.WaitForSingleObject(handle,10000)==0
        api.CloseHandle(handle)
    finally:
        job.close()
        if child.poll() is None:child.kill();child.wait()


def test_download_reuses_concurrently_promoted_cache_with_open_reader(tmp_path,monkeypatch):
    import hashlib
    body=b'identical verified bytes';digest=hashlib.sha256(body).hexdigest()
    a=asset('v0.8.0','setup.exe');a.update(size=len(body),digest='sha256:'+digest)
    target=tmp_path/(digest+'-'+str(a['id']));held=[]
    class Response(io.BytesIO):url=a['browser_download_url']
    class Opener:
        def open(self,*args,**kwargs):
            target.write_bytes(body);held.append(target.open('rb'))
            return Response(body)
    monkeypatch.setattr(releases.urllib.request,'build_opener',lambda *a:Opener())
    def locked_replace(*a):raise PermissionError('cache reader holds Windows file')
    monkeypatch.setattr(releases.os,'replace',locked_replace)
    try:assert releases.download(a,tmp_path)==target
    finally:
        for stream in held:stream.close()
    assert not list(tmp_path.glob('*.partial'))


@pytest.mark.parametrize('contents', ['b'*64+'  setup.dmg', 'a'*64+'  wrong.dmg', 'short  setup.dmg', 'a'*64+'  setup.dmg\nextra'])
def test_release_checksum_file_must_match_provenance_and_asset(tmp_path,contents):
    path=tmp_path/'checksum';path.write_text(contents)
    with pytest.raises(ValueError):releases.verify_checksum(path,'setup.dmg','a'*64)
    path.write_text('a'*64+'  setup.dmg\n');releases.verify_checksum(path,'setup.dmg','a'*64)


def test_failed_attempt_directory_is_discarded_but_active_one_is_kept(tmp_path):
    install = tmp_path / 'App'
    failed, active = (install / 'versions' / name for name in ('0.7.2-a-b-attempt-11111111', '0.7.2-a-b-attempt-22222222'))
    for folder in (failed, active):
        (folder / 'source').mkdir(parents=True)
    (install / 'installed.json').write_text(json.dumps({'source': str(active / 'source')}))
    fake = lambda root: argparse.Namespace(install=install, version_root=root)
    engine.discard_failed_attempt(fake(active))
    assert active.is_dir()
    engine.discard_failed_attempt(fake(failed))
    assert not failed.exists()
    (install / '.setup-transaction').mkdir()
    again = install / 'versions' / '0.7.2-a-b-attempt-33333333'
    again.mkdir()
    engine.discard_failed_attempt(fake(again))
    assert again.is_dir()
    engine.discard_failed_attempt(None)
