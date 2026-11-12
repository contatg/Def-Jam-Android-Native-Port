"""Instrument only the locally generated, verified main update loop. No XBE code bundled."""
from pathlib import Path
import argparse,hashlib,re,shutil
EXPECTED="52ccd0b88ac7b82903d64b0507e8dceff2b2a4bbd59759ee243f2efcaf0ec1b3"
DECL="""
/* DEFJAM_ONLINE_BATCH130: optional, off by default */
#if defined(__ANDROID__)
    extern void android_online_batch130_begin(uint32_t queue);
    extern void android_online_batch130_step(void);
    extern int android_online_batch130_yield(void);
#endif
"""
EDITS=(
 ("loc_00011D40: ;", "\n#if defined(__ANDROID__)\n    android_online_batch130_begin(ebx);\n#endif"),
 ("loc_00011D55: ;", "\n#if defined(__ANDROID__)\n    android_online_batch130_step();\n#endif"),
 ("loc_00011D95: ;", "\n#if defined(__ANDROID__)\n    if(android_online_batch130_yield()) goto loc_00011DA7;\n#endif"))
SIG=re.compile(r"(?m)^void sub_00011CD0\(void\)\n\{")
def patch(text):
    matches=list(SIG.finditer(text))
    if len(matches)!=1:raise ValueError("Expected exactly one sub_00011CD0 definition")
    a=matches[0].start();end=text.find("\n}\n",matches[0].end())
    if end<0:raise ValueError("Unsupported generated function boundary")
    end+=2;original=body=text[a:end]
    marked="DEFJAM_ONLINE_BATCH130" in body
    if marked:
        if body.count(DECL)!=1:raise ValueError("Incomplete batch declaration")
        body=body.replace(DECL,"",1)
        for label,addition in EDITS:
            if body.count(label+addition)!=1:raise ValueError("Incomplete existing batch hook: "+label)
            body=body.replace(label+addition,label,1)
    if hashlib.sha256(body.encode()).hexdigest()!=EXPECTED:raise ValueError("Unsupported sub_00011CD0 version; no generated file changed")
    if marked:return text,False
    body=body.replace("{","{"+DECL,1)
    for label,addition in EDITS:
        if body.count(label)!=1:raise ValueError("Unsupported label: "+label)
        body=body.replace(label,label+addition,1)
    return text[:a]+body+text[end:],True

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("generated",type=Path);p.add_argument("--check",action="store_true");args=p.parse_args()
    hits=[]
    for file in args.generated.glob("*.c"):
        data=file.read_bytes();bom=data.startswith(b"\xef\xbb\xbf");text=data.decode("utf-8-sig");nl="\r\n" if "\r\n" in text else "\n";text=text.replace("\r\n","\n")
        if SIG.search(text):hits.append((file,data,bom,nl,text))
    if len(hits)!=1:raise SystemExit("Expected one generated main update function; no file changed")
    file,data,bom,nl,text=hits[0]
    try:updated,changed=patch(text)
    except ValueError as e:raise SystemExit(str(e))
    if changed and not args.check:
        backup=file.with_suffix(file.suffix+".before130")
        if not backup.exists():shutil.copyfile(file,backup)
        temp=file.with_suffix(file.suffix+".stage130")
        temp.write_bytes((b"\xef\xbb\xbf" if bom else b"")+updated.replace("\n",nl).encode());temp.replace(file)
    print("OnlineBatch130:","verified" if args.check else "patched" if changed else "already patched","(OFF unless [vksched] online_batch130=1)")
if __name__=="__main__":main()
