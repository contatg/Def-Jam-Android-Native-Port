"""Add a host handoff to taken backedges of the local, player-generated SDK function.

No original-disc code is bundled. Re-run after generation; unchanged files are
not rewritten. Validate the one supported function and all labels before writes.
"""
from pathlib import Path
import argparse,re,shutil

MARKER="DEFJAM_MAKESPACE_BACKOFF122"
SIGNATURE=re.compile(r"\bvoid\s+sub_0021EC50\s*\(\s*void\s*\)\s*\{")
TOKEN=re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',re.S)

def masked(text):
    return TOKEN.sub(lambda m:''.join('\n' if c=='\n' else ' ' for c in m.group()),text)

def patch(text):
    clean=masked(text);matches=list(SIGNATURE.finditer(clean))
    if len(matches)!=1:raise ValueError("Expected one exact sub_0021EC50(void) definition")
    start=matches[0].end();level=1;end=start
    while level and end<len(clean):
        level+=(clean[end]=='{')-(clean[end]=='}');end+=1
    if level:raise ValueError("Unbalanced MakeSpace body; no write")
    before=text[:start];body=text[start:end-1];after=text[end-1:]
    if MARKER in body:
        clean_body=masked(body)
        labels={m.group(1):m.start() for m in re.finditer(r'\b(loc_[0-9A-Fa-f]{8})\s*:',clean_body)}
        jumps=list(re.finditer(r'\bgoto\s+(loc_[0-9A-Fa-f]{8})\s*;',clean_body))
        if any(m.group(1) not in labels for m in jumps):raise ValueError("Incomplete MakeSpace labels")
        backward=sum(labels[m.group(1)]<m.start() for m in jumps)
        hooks=len(re.findall(r'(?m)^\s*defjam_make_space_poll\(\);',clean_body))
        if not backward or hooks!=backward:raise ValueError("Incomplete existing MakeSpace patch")
        return text,0
    code=masked(body)
    labels={m.group(1):m.start() for m in re.finditer(r'\b(loc_[0-9A-Fa-f]{8})\s*:',code)}
    edits=[]
    for m in re.finditer(r'\bgoto\s+(loc_[0-9A-Fa-f]{8})\s*;',code):
        target=m.group(1)
        if target not in labels:raise ValueError("Unresolved MakeSpace label: "+target)
        if labels[target]<m.start():
            edits.append((m.start(),m.end(),'{\n#if defined(__ANDROID__)\n        defjam_make_space_poll();\n#endif\n        goto '+target+';\n    }'))
    if not edits:raise ValueError("No verified backward branch in MakeSpace; no write")
    for a,b,value in reversed(edits):body=body[:a]+value+body[b:]
    declaration='\n/* '+MARKER+' */\n#if defined(__ANDROID__)\n    extern void defjam_make_space_poll(void);\n#endif\n'
    return before+declaration+body+after,len(edits)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('generated',type=Path);args=parser.parse_args()
    hits=[]
    for file in args.generated.glob('*.c'):
        text=file.read_text(encoding='utf-8-sig')
        if 'sub_0021EC50' in text and SIGNATURE.search(masked(text)):hits.append((file,text))
    if len(hits)!=1:raise SystemExit('Expected one supported generated MakeSpace definition; found '+str(len(hits))+'. No generated file changed.')
    file,text=hits[0];updated,count=patch(text)
    if count:
        backup=file.with_suffix(file.suffix+'.before122');
        if not backup.exists():shutil.copyfile(file,backup)
        temp=file.with_suffix(file.suffix+'.stage122');temp.write_text(updated,encoding='utf-8');temp.replace(file)
    print('MakeSpace122:',str(count)+' backedge(s) instrumented' if count else 'already patched', 'in',file.name)

if __name__=='__main__':main()
