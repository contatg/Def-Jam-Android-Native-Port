#!/usr/bin/env python3
"""Collect a bounded, private timing-source diagnostic; no game data or APKs."""
import argparse, hashlib, json, re, zipfile
from collections import deque
from pathlib import Path

TARGETS = '''00011CD0 0002C530 000D7560 000D75E0 000D7700 000D7960
000D7AE0 000D7DA0 000D7DE0 000D7EA0 000D02A0 000D0C20
00077740 00087700 001A4900 001A4A30 0003D960 00044C10 0008BE60
00077040 00079B60 0007A8F0'''.split()
SIGNATURE = re.compile(r'(?m)^[ \t]*(?:static[ \t]+)?void[ \t]+(sub_([0-9a-fA-F]{8})(?:_gen)?)[ \t]*\([ \t]*void[ \t]*\)[ \t\r\n]*\{')
CALL = re.compile(r'\bsub_([0-9a-fA-F]{8})(?:_gen)?\s*\(')

def function_end(source, opening):
    """Count C braces outside comments/quoted literals."""
    level=0; mode='code'; i=opening
    while i<len(source):
        ch=source[i]; nxt=source[i:i+2]
        if mode=='line':
            if ch=='\n': mode='code'
        elif mode=='block':
            if nxt=='*/': mode='code';i+=1
        elif mode in ('"',"'"):
            if ch=='\\': i+=1
            elif ch==mode: mode='code'
        elif nxt=='//': mode='line';i+=1
        elif nxt=='/*': mode='block';i+=1
        elif ch in ('"',"'"):mode=ch
        elif ch=='{':level+=1
        elif ch=='}':
            level-=1
            if level==0:return i+1
        i+=1
    raise ValueError('Unclosed C function')

def collect(root,output):
    gen=root/'src/recomp/gen'
    files=[root/'src/recomp_manual.c']+sorted(gen.glob('recomp_[0-9]*.c'))
    files=[p for p in files if p.is_file()]
    if not gen.is_dir() or not files:raise ValueError('src/recomp/gen ausente; execute no projeto do PC que ja gerou os fontes.')
    index={}; sources={}
    for p in files:
        text=p.read_text(encoding='utf-8-sig',errors='replace'); rel=p.relative_to(root).as_posix()
        sources[rel]=text
        for m in SIGNATURE.finditer(text):index.setdefault(m[2].upper(),[]).append((rel,m.start(),m.end()-1,m[1]))
    requested=deque((key,0) for key in TARGETS);seen=set();found=[];missing=[];size=0
    output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED) as z:
        while requested and len(seen)<80:
            key,depth=requested.popleft()
            if key in seen:continue
            seen.add(key)
            entries=index.get(key,[])
            if not entries:missing.append(key);continue
            for rel,start,opening,name in entries:
                code=sources[rel][start:function_end(sources[rel],opening)]+'\n'
                raw=code.encode('utf-8');size+=len(raw)
                if size>8*1024*1024:raise ValueError('Coleta excedeu8MiB; reduza as dependencias.')
                target='functions/'+name+'.c'
                z.writestr(target,raw)
                found.append({'name':name,'source':rel,'sha256':hashlib.sha256(raw).hexdigest()})
                if depth==0:
                    for callee in sorted(set(CALL.findall(code))):requested.append((callee.upper(),1))
        types=gen/'recomp_types.h'
        if types.is_file() and types.stat().st_size<4*1024*1024:z.write(types,'types/recomp_types.h')
        z.writestr('manifest.json',json.dumps({'targets':TARGETS,'functions':found,'missing':missing,
                     'purpose':'Private diagnostic for real frame rate/simulation decoupling; no game data/APK'},indent=2))
    if not found:output.unlink();raise ValueError('Nenhuma funcao localizada; envie a estrutura de src/recomp/gen.')
    print(f'Coleta FPS: {len(found)} funcoes, {len(missing)} ausentes -> {output}')
    return found,missing

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--project-root',type=Path,default=Path.cwd())
    parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    try:collect(args.project_root.resolve(),args.output.resolve())
    except (OSError,ValueError) as exc:parser.exit(1,f'Coleta nao concluida: {exc}\n')
if __name__=='__main__':main()
