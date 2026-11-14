"""Hook the player's own verified generated code. No original XBE code is bundled."""
from pathlib import Path
import hashlib,re,argparse,shutil
HOOKS={'0002C530': 'extern void netplay_on_update(void); netplay_on_update();', '0003C6E0': 'extern void rollback_audio_init_begin(void); rollback_audio_init_begin();', '00012CC0': 'extern void rollback_audio_init_end(void); rollback_audio_init_end();', '0010AC20': 'extern void rollback_note_dynamic_load(uint32_t,uint32_t); rollback_note_dynamic_load(MEM32(g_esp),g_ecx);', '00029500': 'extern int rollback_route_alloc(void); if(rollback_route_alloc())return;'}
HASHES={'0002C530': '8f17b94ba8e947eedc3121ca56b32a2ef26c0abe18fd8242fb6f35ac4edceeab', '0003C6E0': '97051c44557234d1461817d4c9a11d4085813e05c28c5e549fc165e1b41a7838', '00012CC0': '07e1d4ee1fa55c32b84a641d41a271f01ccb5dc58287d66d8ccb84d5f1b0a83a', '0010AC20': '69e59d34bc8d04888046c5ed2ee91e6a7ebab475ed4e12dc450f1da0c1b098ff', '00029500': '30d6252fa9d9f3150d243f7e2aba5817cde1e0dac066766ab5dc5b5678532765'}
# The current lifter emits entry hooks for both audio functions. Their
# implementations in recomp_manual.c already call the rollback handlers.
ENTRY_HOOK_HASHES = {
    '0003C6E0': '07a7352499cfab2aaa6002da0970a253e51abab52d0b35072623d1bf71df712a',
    '00012CC0': '4d9cca4b76da31028c3ceb36b557f920ba9922d85bcb3fe9eec9bc3586eb9a2a',
}

def patch(text):
    changes=0
    for va,call in HOOKS.items():
        sig='void sub_'+va+'(void)\n{'
        if sig not in text:continue
        if text.count(sig)!=1:raise ValueError('Duplicate generated function '+va)
        a=text.index(sig);b=text.index('\n}\n',a)+2;body=text[a:b]
        label='loc_'+va+': ;'
        addition='\n#if defined(__ANDROID__)\n    /* DEFJAM_ROLLBACK131_'+va+' */\n    '+call+'\n#endif'
        marked='DEFJAM_ROLLBACK131_'+va in body
        if marked:
            if body.count(label+addition)!=1:raise ValueError('Incomplete existing rollback hook '+va)
            plain=body.replace(label+addition,label,1)
        else:plain=body
        digest = hashlib.sha256(plain.encode()).hexdigest()
        if not marked and digest == ENTRY_HOOK_HASHES.get(va):
            continue
        if digest != HASHES[va]:raise ValueError('Unsupported generated function '+va+'; no file written')
        if not marked:
            if body.count(label)!=1:raise ValueError('Unsupported label '+va)
            text=text[:a]+body.replace(label,label+addition,1)+text[b:];changes+=1
    return text,changes

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('generated',type=Path);ap.add_argument('--check',action='store_true');args=ap.parse_args()
    plan=[];counts={va:0 for va in HOOKS}
    for file in args.generated.glob('*.c'):
        data=file.read_bytes();bom=data.startswith(b'\xef\xbb\xbf');text=data.decode('utf-8-sig');nl='\r\n' if '\r\n' in text else '\n';text=text.replace('\r\n','\n')
        present=[va for va in HOOKS if 'void sub_'+va+'(void)\n{' in text]
        if not present:continue
        for va in present:counts[va]+=1
        try:updated,n=patch(text)
        except (ValueError,IndexError) as e:raise SystemExit(str(e))
        if n:plan.append((file,data,(b'\xef\xbb\xbf' if bom else b'')+updated.replace('\n',nl).encode()))
    if any(n!=1 for n in counts.values()):raise SystemExit('Missing/duplicate generated rollback hooks: '+str(counts)+'; no file written')
    if not args.check:
        # Back up ALL affected files before the first mutation.
        for file,data,_ in plan:
            backup=file.with_suffix(file.suffix+'.before131')
            if not backup.exists():backup.write_bytes(data)
        written=[]
        try:
            for file,data,updated in plan:
                temp=file.with_suffix(file.suffix+'.stage131');temp.write_bytes(updated);temp.replace(file);written.append((file,data))
        except BaseException:
            for file,data in written:file.write_bytes(data)
            raise
    print('Rollback131:', 'verified five generated functions' if args.check else str(sum(patch(data.decode('utf-8-sig').replace('\r\n','\n'))[1] for _,data,_ in plan))+' hook(s) instrumented; runtime mode remains opt-in')
if __name__=='__main__':main()
