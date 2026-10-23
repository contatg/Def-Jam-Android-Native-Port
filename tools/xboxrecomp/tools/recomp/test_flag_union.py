"""Compile the upstream/fork flag union and compare it with independent x86 answers."""
import math
from pathlib import Path

from . import config
from .translator import FunctionTranslator
from .test_dynamic_flags import PRELUDE
from .test_lifter_result_clobber import _build_and_run

EXTRA = r'''
#include <math.h>
#define HI8(r) (((r)>>8)&255u)
#define SET_HI8(r,v) ((r)=((r)&0xffff00ffu)|((uint32_t)(uint8_t)(v)<<8))
typedef union { float f[4]; double d[2]; } XMM;
XMM xmm0,xmm1;
int g_fp_cmp;
'''


def translated(image, name):
    base = 0x10000
    image = bytes.fromhex(image)
    config._install([config.Section('.text',base,len(image),0,len(image),True)],
                    entry_point=base,kernel_thunk_addr=base,origin='flag-union-test')
    db={base:{'end':base+len(image),'size':len(image)}}
    return FunctionTranslator(image,db).translate_function(base,db[base]).replace('sub_00010000',name)


def test_mixed_float_integer_join_multiple_consumers_and_lahf():
    # Select UCOMISS or CMP, then join at SETB/SETE/SETP, CMOVP and LAHF.
    # MOV clobbers original operands while leaving EFLAGS untouched.
    prefix='83fa00 7405 0f2ec1 eb02 39c8 b899000000 b999000000 '
    code=translated(prefix+'0f92c2 0f94c6 0f9ac1 0fb6f1 b901000000 0f4ac1 9f c3','mixed')
    checks=[]
    vals=(0.0,1.0,-1.0,float('nan'))
    for edge in (0,1):
        for a in vals if edge else (0,1,0x80000000,0xffffffff):
            for b in vals if edge else (0,1,0x80000000,0xffffffff):
                if edge:
                    unordered=math.isnan(a) or math.isnan(b)
                    cf,zf,pf,sf=(a<b or unordered,a==b or unordered,unordered,False)
                else:
                    out=(a-b)&0xffffffff
                    cf,zf,pf,sf=(a<b,out==0,(out&255).bit_count()%2==0,bool(out>>31))
                ah=2+int(cf)+4*int(pf)+64*int(zf)+128*int(sf)
                literal=lambda v: 'NAN' if isinstance(v,float) and math.isnan(v) else repr(v)
                checks.append(f'eax={(int(a) if not edge else 99)}u;ecx={(int(b) if not edge else 99)}u;edx={edge};'
                              f'xmm0.f[0]={literal(a)};xmm1.f[0]={literal(b)};esi=0x99;esp=0x1000;mixed();'
                              f'if(HI8(eax)!={ah}u || (edx&255u)!={int(cf)}u || ((edx>>8)&255u)!={int(zf)}u'
                              f' || esi!={int(pf)}u || (eax&255u)!={(1 if pf else 0x99)}u) return 1;')
    source=PRELUDE+EXTRA+code+'int main(void){'+''.join(checks)+'return 0;}'
    ran=_build_and_run(source)
    assert ran.returncode==0,ran.stdout+ran.stderr
    # Removing float publication must be caught by the independent checks.
    broken=source.replace('_fv = 15u;', '_fv = 0u;')
    assert broken != source
    assert _build_and_run(broken).returncode != 0


def test_double_register_comparison_and_sahf_lahf():
    code=translated('660f2ec1 9f c3','doubles')
    sahf=translated('9e b899000000 9f c3','loaded')
    preserve_of=translated('39c8 9e 0f90c0 0fb6c0 c3','sahf_of')
    checks=[]
    for a in (1.0,2.0,-1.0,float('nan')):
        for b in (1.0,2.0,-1.0,float('nan')):
            unordered=math.isnan(a) or math.isnan(b)
            ah=2+int(a<b or unordered)+64*int(a==b or unordered)+4*int(unordered)
            literal=lambda v:'NAN' if math.isnan(v) else repr(v)
            checks.append(f'xmm0.d[0]={literal(a)};xmm1.d[0]={literal(b)};eax=0;esp=0x1000;doubles();if(HI8(eax)!={ah})return 1;')
    # AF is outside the model; pin only the supported SF/ZF/PF/CF and bit 1.
    for ah in range(256):
        checks.append(f'eax={ah<<8}u;g_fp_cmp=2;esp=0x1000;loaded();if(HI8(eax)!={(ah&0xc5)|2})return 2;')
    for a in (0,1,0x7fffffff,0x80000000,0xffffffff):
        for b in (0,1,0x7fffffff,0x80000000,0xffffffff):
            signed=lambda v:v if v<0x80000000 else v-0x100000000
            of=int(not -0x80000000<=signed(a)-signed(b)<=0x7fffffff)
            checks.append(f'eax={a}u;ecx={b}u;esp=0x1000;sahf_of();if(eax!={of})return 3;')
    source=PRELUDE+EXTRA+code+sahf+preserve_of+'int main(void){'+''.join(checks)+'return 0;}'
    ran=_build_and_run(source)
    assert ran.returncode==0,ran.stdout+ran.stderr
    broken=source.replace('xmm0.d[0]; _fcb = xmm1.d[0];','xmm0.f[0]; _fcb = xmm1.f[0];')
    assert broken != source
    assert _build_and_run(broken).returncode != 0
    broken=source.replace('_fv |= 11u;','_fv = 11u;')
    assert broken != source
    assert _build_and_run(broken).returncode != 0


def test_nonzero_shifts_publish_parity_and_zero_counts_preserve_it():
    sources,checks=[],[]
    vals=(0,1,3,0x80,0x81,0x80000000,0xffffffff)
    for tag,op in (('left','c1e0'),('right','c1e8'),('signed','c1f8')):
        for count in (0,1,2,7,31,32):
            name=f'pf_{tag}_{count}'
            # CMP seeds PF independently, then a shift and SETP consume it.
            sources.append(translated('39c8 '+op+f'{count:02x}'+' 0f9ac0 0fb6c0 c3',name))
            for a in vals:
                for b in vals:
                    n=count&31
                    if not n: out=(a-b)&0xffffffff
                    elif tag=='left': out=(a<<n)&0xffffffff
                    elif tag=='right': out=a>>n
                    else: out=(a if a<0x80000000 else a-0x100000000)>>n
                    want=int((out&255).bit_count()%2==0)
                    checks.append(f'eax={a}u;ecx={b}u;esp=0x1000;{name}();if(eax!={want})return 1;')
    ran=_build_and_run(PRELUDE+EXTRA+'\n'.join(sources)+'int main(void){'+''.join(checks)+'return 0;}')
    assert ran.returncode==0,ran.stdout+ran.stderr


def test_x87_status_compare_preserves_eflags_and_pop_count():
    header=(Path(__file__).resolve().parents[2]/'templates/runtime/recomp_types.h').read_text(encoding='utf-8')
    macros='\n'.join(line for line in header.splitlines()
                     if line.startswith(('#define RECOMP_FCMP(', '#define RECOMP_FCMP_CC(')))
    # FCOMIP compares and pops. The following FCOM sees a different ordering
    # but must only update x87 status, leaving the published EFLAGS untouched.
    code=translated('dff1 d8d1 9f c3','x87_flags')
    checks=[]
    for a in (-1.0,0.0,1.0,float('nan')):
        for b in (-1.0,0.0,1.0,float('nan')):
            unordered=math.isnan(a) or math.isnan(b)
            ah=2+int(a<b or unordered)+64*int(a==b or unordered)+4*int(unordered)
            literal=lambda v:'NAN' if math.isnan(v) else repr(v)
            status=0x4500 if math.isnan(b) else 0
            comparison=2 if math.isnan(b) else 1
            checks.append(f'g_fp_cmp=99;g_fp_cc=0xaaaa;g_fp_top=0;g_fp_stack[0]={literal(a)};g_fp_stack[1]={literal(b)};'
                          f'g_fp_stack[2]=-123.0;eax=0;esp=0x1000;x87_flags();'
                          f'if(HI8(eax)!={ah} || g_fp_top!=1 || g_fp_cc!={status} || g_fp_cmp!={comparison})return 1;')
    source=PRELUDE+EXTRA+'\n'+macros+'\nunsigned g_fp_top;double g_fp_stack[8];uint16_t g_fp_cc;\n'
    source+=code+'int main(void){'+''.join(checks)+'return 0;}'
    ran=_build_and_run(source)
    assert ran.returncode==0,ran.stdout+ran.stderr
    status_compare='g_fp_cmp = RECOMP_FCMP(fp_top(), fp_st1()); g_fp_cc = RECOMP_FCMP_CC(g_fp_cmp);'
    broken=source.replace(status_compare,'/* status compare omitted */')
    assert broken != source
    assert _build_and_run(broken).returncode != 0
