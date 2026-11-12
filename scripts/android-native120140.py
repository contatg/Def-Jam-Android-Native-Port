"""Insert one verified frame-clock entry hook into locally generated sources."""
from pathlib import Path
import argparse,hashlib,re,shutil
EXPECTED="bfd8bb3c4e32e2ba27a32184ca4269e9c0052ac7bb2c69bfdbec7b5e06d57bbb"
SIG=re.compile(r"(?m)^void sub_000D7DE0\(void\)\n\{")
ADD="""
/* DEFJAM_NATIVE120140: opt-in offline experiment */
#if defined(__ANDROID__)
    extern void android_native120_clock140(uint32_t self);
    android_native120_clock140(g_ecx);
#endif
"""
def patch(text,remove=False):
    matches=list(SIG.finditer(text))
    if len(matches)!=1:raise ValueError("Expected one D7DE0 function")
    a=matches[0].start();b=text.find("\n}\n",matches[0].end())
    if b<0:raise ValueError("Unsupported D7DE0 boundary")
    b+=2;body=text[a:b];marked="DEFJAM_NATIVE120140" in body
    if marked:
        if body.count(ADD)!=1:raise ValueError("Incomplete native120 hook")
        body=body.replace(ADD,"",1)
    if hashlib.sha256(body.encode()).hexdigest()!=EXPECTED:raise ValueError("Unsupported generated D7DE0; no source changed")
    if remove:return (text[:a]+body+text[b:],True) if marked else (text,False)
    if marked:return text,False
    return text[:a]+body.replace("{","{"+ADD,1)+text[b:],True
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument("generated",type=Path);ap.add_argument("--check",action="store_true");ap.add_argument("--remove",action="store_true");a=ap.parse_args();hits=[]
    for f in a.generated.glob("*.c"):
        raw=f.read_bytes();text=raw.decode("utf-8-sig").replace("\r\n","\n")
        if SIG.search(text):hits.append((f,raw,text))
    if len(hits)!=1:raise SystemExit("Expected one generated D7DE0 definition; no source changed")
    f,raw,text=hits[0]
    try:out,changed=patch(text,a.remove)
    except ValueError as e:raise SystemExit(str(e))
    if changed and not a.check:
        backup=f.with_suffix(f.suffix+".before140")
        if not backup.exists():shutil.copyfile(f,backup)
        nl="\r\n" if b"\r\n" in raw else "\n"
        tmp=f.with_suffix(f.suffix+".stage140");tmp.write_bytes((b"\xef\xbb\xbf" if raw.startswith(b"\xef\xbb\xbf") else b"")+out.replace("\n",nl).encode());tmp.replace(f)
    print("Native120140:",("removal verified" if a.check else "removed" if changed else "already disabled") if a.remove else ("verified" if a.check else "patched" if changed else "already patched"),"; experiment disabled in patch144" if a.remove else "; runtime opt-in,not certified120 gameplay")
if __name__=="__main__":main()
