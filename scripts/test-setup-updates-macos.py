#!/usr/bin/env python3
"""Native macOS setup/launcher acceptance with game-free synthetic future releases.

Usage: python scripts/test-setup-updates-macos.py --payload VERIFIED_MACOS_PAYLOAD --work logs/setup-updates/native
Uses the supplied packaged Python/tools/libs/launcher; builds tiny native C marker
programs instead of game code. No real dump, saves or shortcuts are touched.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import time


def inventory(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in folder.rglob('*') if p.is_file()}


def payload(base, target, version):
    shutil.copytree(base, target)
    shutil.rmtree(target/'source')
    source=target/'source';(source/'scripts').mkdir(parents=True);(source/'config').mkdir()
    (source/'src/recomp/gen').mkdir(parents=True)
    (source/'config/dump-manifest.json').write_text('{}')
    (source/'scripts/verify-dump.py').write_text("def verify(root, manifest):\n from pathlib import Path\n return [] if (Path(root)/'default.xbe').read_bytes()==b'synthetic setup fixture' else ['wrong fixture']\n")
    (source/'scripts/pipeline.py').write_text("import sys\nfrom pathlib import Path\nPath(sys.argv[1]+'.state').write_text('verified')\n")
    (source/'scripts/pipeline-state.py').write_text("from pathlib import Path\ndef verify_analysis(root, toolkit):\n assert (root/'analyze.state').read_text()=='verified'\ndef verify_lift(root, toolkit):\n assert (root/'recomp.state').read_text()=='verified'\ndef verify_build(root, preset):\n if not (root/'build'/preset/'defjam_recomp').is_file(): raise ValueError('unbuilt')\n")
    (source/'CMakePresets.json').write_text(json.dumps(dict(version=2,configurePresets=[dict(name='posix-release',generator='Ninja',binaryDir='${sourceDir}/build/posix-release',cacheVariables={'CMAKE_BUILD_TYPE':'Release'})],buildPresets=[dict(name='posix-release',configurePreset='posix-release')])))
    (source/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(setup_marker C)\nadd_executable(defjam_recomp marker.c)\n')
    (source/'marker.c').write_text('/* Game-free updater acceptance marker, not game code. */\n#include <stdio.h>\n#include <stdlib.h>\n#include <unistd.h>\nint main(void) { char path[4096],cwd[4096]; const char *data=getenv("DEFJAM_DATA");\n snprintf(path,sizeof(path),"%s/launcher-marker.txt",data); FILE *f=fopen(path,"w");\n if(!f)return 3; getcwd(cwd,sizeof(cwd)); fprintf(f,"'+version+'\\n%s\\n%s\\n",data,cwd); fclose(f); return 0; }\n')
    manifest=json.loads((target/'manifest.json').read_text())
    manifest.update(development=True,version=version,source_commit=hashlib.sha1(version.encode()).hexdigest(),updater={'protocol':1,'data_schema':1})
    manifest['files']=inventory(target);manifest['files'].pop('manifest.json',None)
    (target/'manifest.json').write_text(json.dumps(manifest))
    return target


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--payload',type=Path,required=True);parser.add_argument('--work',type=Path,required=True)
    args=parser.parse_args();work=args.work.resolve()
    if sys.platform!='darwin':parser.error('Native macOS required')
    if work.exists():parser.error('Choose a new disposable work folder')
    work.mkdir(parents=True)
    app,data,dump=(work/name for name in ('app','data','dump'))
    dump.mkdir();(dump/'default.xbe').write_bytes(b'synthetic setup fixture')
    data.mkdir();(data/'save').mkdir();(data/'save/profile').write_bytes(b'preserve current progress')
    (data/'settings.ini').write_bytes(b'; user comments\r\n[controls]\r\ncustom=keep\r\n')
    sentinels=inventory(data)
    (work/'sentinel-before.json').write_text(json.dumps(sentinels,indent=2))
    base_manifest=json.loads((args.payload/'manifest.json').read_text())
    metadata=dict(platform='macos-arm64',host=platform.platform(),architecture=platform.machine(),
                  synthetic=True,python=sys.version,toolkit_commit=base_manifest['toolkit_commit'],
                  source_commit=base_manifest['source_commit'],
                  base_manifest_sha256=hashlib.sha256((args.payload/'manifest.json').read_bytes()).hexdigest())
    a=payload(args.payload,work/'payload-a','0.8.0');b=payload(args.payload,work/'payload-b','0.9.0')
    reports=[]
    for label,package,op,input_dump in [('install',a,'install',True),('update',b,'update',False),('downgrade',a,'downgrade',False),('repair',a,'repair',False),('rebuild',a,'rebuild',False)]:
        log=work/(label+'.log')
        argv=[package/'python/bin/python3','-B',package/'engine/engine.py','--payload',package,'--bundled-only','--operation',op,'--install-dir',app,'--data-dir',data,'--no-shortcuts','--log',log]
        if input_dump:argv += ['--dump',dump]
        started=time.time()
        with (work/(label+'.out')).open('w') as out:
            result=subprocess.run(list(map(str,argv)),stdout=out,stderr=subprocess.STDOUT)
        if result.returncode:raise RuntimeError(label+' failed: '+str(result.returncode)+'; '+str(log))
        for name,sha in sentinels.items():assert hashlib.sha256((data/name).read_bytes()).hexdigest()==sha,name
        receipt=json.loads((app/'installed.json').read_text());expected=json.loads((package/'manifest.json').read_text())['version']
        assert receipt['version']==expected and not (app/'.setup-transaction').exists()
        marker=data/'launcher-marker.txt';marker.unlink(missing_ok=True)
        launcher=app/'Def Jam Recompiled.app/Contents/MacOS/DefJamLauncher'
        subprocess.run([launcher],check=True)
        lines=marker.read_text().splitlines();assert lines==[expected,str(data),receipt['source']],lines
        reports.append(dict(case=label,version=expected,exit_code=result.returncode,seconds=round(time.time()-started,2),actual_launcher=lines,source=receipt['source']))
        (work/'results.json').write_text(json.dumps(dict(**metadata,results=reports),indent=2))
    assert len({r['source'] for r in reports})==5,'Every attempt must be isolated'
    # Faults run through the real engine, signed app staging and receipt publication.
    for point in ('prepared', 'launcher', 'receipt-json'):
        before=json.loads((app/'installed.json').read_text())
        worker=work/('fault-'+point+'.py')
        worker.write_text("import importlib.util,os,sys\nspec=importlib.util.spec_from_file_location('engine',sys.argv[1]);e=importlib.util.module_from_spec(spec);spec.loader.exec_module(e)\ne.transaction.HOOK=lambda name:os._exit(91) if name==sys.argv[2] else None\nsys.exit(e.main(sys.argv[3:]))\n")
        command=[a/'python/bin/python3','-B',worker,a/'engine/engine.py',point,'--payload',a,'--operation','rebuild','--install-dir',app,'--data-dir',data,'--no-shortcuts']
        with (work/('fault-'+point+'.out')).open('w') as out:
            stopped=subprocess.run(list(map(str,command)),stdout=out,stderr=subprocess.STDOUT)
        assert stopped.returncode==91 and (app/'.setup-transaction').exists()
        recovery="import importlib.util,sys;spec=importlib.util.spec_from_file_location('engine',sys.argv[1]);e=importlib.util.module_from_spec(spec);spec.loader.exec_module(e);e.transaction.Activation(sys.argv[2],sys.argv[3]).recover()"
        subprocess.run(list(map(str,[a/'python/bin/python3','-B','-c',recovery,a/'engine/engine.py',app,data])),check=True)
        assert json.loads((app/'installed.json').read_text())==before
        for name,sha in sentinels.items():assert hashlib.sha256((data/name).read_bytes()).hexdigest()==sha
        subprocess.run([app/'Def Jam Recompiled.app/Contents/MacOS/DefJamLauncher'],check=True)
        assert (data/'launcher-marker.txt').read_text().splitlines()==['0.8.0',str(data),before['source']]
        reports.append(dict(case='hard-exit-'+point,recovered=True,exit_code=91,actual_source=before['source']))
    # Exercise a real parent→selected-engine handoff with the target's bundled Python.
    spec=importlib.util.spec_from_file_location('selected_releases',args.payload/'engine/releases.py')
    release_module=importlib.util.module_from_spec(spec);spec.loader.exec_module(release_module)
    for label,package,op in [('selected-update',b,'update'),('selected-downgrade',a,'downgrade')]:
        chosen=argparse.Namespace(install_dir=str(app),data_dir=str(data),dump=None,operation=op,toolchain='auto',
                                 log=str(work/(label+'.log')),status_file=None,cancel_file=None,no_shortcuts=True)
        code=release_module.run_selected(chosen,'macos-arm64',package,lambda:None)
        assert code==0
        receipt=json.loads((app/'installed.json').read_text());expected=json.loads((package/'manifest.json').read_text())['version']
        assert receipt['version']==expected
        for name,sha in sentinels.items():assert hashlib.sha256((data/name).read_bytes()).hexdigest()==sha
        subprocess.run([app/'Def Jam Recompiled.app/Contents/MacOS/DefJamLauncher'],check=True)
        lines=(data/'launcher-marker.txt').read_text().splitlines();assert lines==[expected,str(data),receipt['source']]
        reports.append(dict(case=label,version=expected,exit_code=code,actual_launcher=lines,source=receipt['source']))
    (work/'results.json').write_text(json.dumps(dict(**metadata,results=reports),indent=2))
    print(json.dumps(reports,indent=2))


if __name__=='__main__':main()
