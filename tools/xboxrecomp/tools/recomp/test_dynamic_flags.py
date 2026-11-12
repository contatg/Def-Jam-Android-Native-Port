"""Execute dynamic join predicates against independent width-aware x86 answers."""
from tools.recomp.disasm import Instruction, Operand
from tools.recomp.lifter import Lifter, _dynamic_condition, _make_condition
from tools.recomp.test_lifter_result_clobber import _build_and_run

PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
uint32_t eax,ecx,edx,esi,edi,esp;
int g_df;
uint8_t mem[256];
unsigned reads;
#define MEM8(a) (*((uint8_t*)(mem+(a))))
#define MEM16(a) (*((uint16_t*)(mem+(a))))
#define MEM32(a) (*((uint32_t*)(mem+(a))))
#define LO8(r) ((uint8_t)(r))
#define LO16(r) ((uint16_t)(r))
#define ZX8(r) ((uint32_t)(uint8_t)(r))
#define SET_LO8(r,v) ((r)=((r)&0xffffff00u)|(uint8_t)(v))
#define SET_LO16(r,v) ((r)=((r)&0xffff0000u)|(uint16_t)(v))
#define RECOMP_DF_STEP(s) (g_df ? -(s) : (s))
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_NE(a,b) ((a)!=(b))
#define TEST_Z(a,b) (((a)&(b))==0)
#define RECOMP_UNIMPL(a,m) ((void)0)
#define RECOMP_PARITY8(v) ((0x9669u >> (((v) ^ ((v) >> 4)) & 15u)) & 1u)
'''
LOCALS = 'uint32_t _fa=0,_fb=0; int32_t _fas=0,_fbs=0; int _cf=c,_flags=0,_sf=0,_of=0,_pf=0; unsigned _fv=0;'
CONDITIONS = ('je','jne','js','jns','jo','jno','jl','jge','jle','jg','jb','jae','jbe','ja','jp','jnp')


def emitter():
    lifter = Lifter()
    lifter.needs_cf = lifter.needs_dynamic_flags = True
    lifter.func_start, lifter.func_end = 0, 0x200
    return lifter


def signed(n, bits):
    return n - (1 << bits) if n & (1 << (bits-1)) else n


def answer(m, a, b, c, bits):
    mask = (1 << bits)-1
    a, b = a & mask, b & mask
    if m in ('cmp','sub','sbb'):
        n = a-b-(c if m=='sbb' else 0)
        sn = signed(a,bits)-signed(b,bits)-(c if m=='sbb' else 0)
        carry = n < 0
    elif m in ('add','adc'):
        n = a+b+(c if m=='adc' else 0)
        sn = signed(a,bits)+signed(b,bits)+(c if m=='adc' else 0)
        carry = n > mask
    elif m == 'inc':
        n, sn, carry = a+1, signed(a,bits)+1, c
    elif m == 'dec':
        n, sn, carry = a-1, signed(a,bits)-1, c
    elif m == 'neg':
        n, sn, carry = -a, -signed(a,bits), a != 0
    else:
        n, carry = a & b, False
        sn = signed(n,bits)
    zf, sf = (n & mask)==0, bool(n & (1 << (bits-1)))
    of = not (-(1 << (bits-1)) <= sn < (1 << (bits-1)))
    pf = (n & 255).bit_count() % 2 == 0
    answers = (zf,not zf,sf,not sf,of,not of,sf!=of,sf==of,
               zf or sf!=of,not zf and sf==of,carry,not carry,
               carry or zf,not carry and not zf,pf,not pf)
    return sum(int(v) << i for i,v in enumerate(answers))


def test_mixed_join_predicates_after_register_clobber():
    sources, checks = [], []
    lifter = emitter()
    for bits, dst, src in ((8,'al','cl'),(16,'ax','cx'),(32,'eax','ecx')):
        vals = (0,1,(1 << (bits-1))-1,1 << (bits-1),(1 << bits)-1)
        for m in ('cmp','test','add','sub','inc','dec','neg','adc','sbb'):
            ops = [Operand(type='reg', reg=dst)]
            if m not in ('inc','dec','neg'): ops.append(Operand(type='reg', reg=src))
            insn = Instruction(0,3,m,'','',operands=ops)
            stmts = ' '.join(lifter.lift_instruction(insn))
            name = f'case_{m}_{bits}'
            predicates = '|'.join(f'((unsigned)({_dynamic_condition(cc)}) << {i})'
                                  for i,cc in enumerate(CONDITIONS))
            sources.append(f'unsigned {name}(uint32_t a,uint32_t b,int c){{ {LOCALS} eax=a;ecx=b;'
                           + stmts + ' eax=ecx=0x99; return '+predicates+'; }')
            for a in vals:
                for b in vals:
                    for c in (0,1):
                        want = answer(m,a,b,c,bits)
                        checks.append(f'if({name}(0x{a:X}u,0x{b:X}u,{c})!={want}u){{printf("{name} {a:X} {b:X} {c}\\n");return 1;}}')
    ran = _build_and_run(PRELUDE+'\n'.join(sources)+'\nint main(void){'+'\n'.join(checks)+'return 0;}')
    assert ran.returncode == 0, ran.stdout+ran.stderr


def test_signed_rep_compares_every_width_and_zero_count():
    sources, checks = [], []
    lifter = emitter()
    for size in (1,2,4):
        bits, mask = size*8, (1 << (size*8))-1
        vals = (0,1,(1 << (bits-1))-1,1 << (bits-1),mask)
        for kind in ('cmps','scas'):
            m = 'repe '+kind+{1:'b',2:'w',4:'d'}[size]
            first = f'MEM{bits}(esi)' if kind=='cmps' else f'(eax & 0x{mask:X}u)'
            stmts = ' '.join(lifter._rep_compare(m, first, f'MEM{bits}(edi)', size, kind=='cmps'))
            predicates = '|'.join(f'((unsigned)({_dynamic_condition(cc)}) << {i})'
                                  for i,cc in enumerate(CONDITIONS))
            name=f'case_{kind}_{bits}'
            sources.append(f'unsigned {name}(uint32_t a,uint32_t b,unsigned n,int df){{int c=0;{LOCALS}'
                           + 'eax=a;esi=32;edi=64;ecx=n;g_df=df;'
                           + f'MEM{bits}(32)=a;MEM{bits}(64)=b;'
                           + '_flags=1;_sf=1;_of=0;_cf=1;_pf=1;_fv=15;'
                           + stmts + 'return '+predicates+'; }')
            previous = sum(int(v)<<i for i,v in enumerate((1,0,1,0,0,1,1,0,1,0,1,0,1,0,1,0)))
            for a in vals:
                for b in vals:
                    for n in (0,1):
                        for df in (0,1):
                            want=answer('cmp',a,b,0,bits) if n else previous
                            checks.append(f'if({name}(0x{a:X}u,0x{b:X}u,{n},{df})!={want}u){{printf("{name} {a:X} {b:X} {n} {df}\\n");return 1;}}')
    ran=_build_and_run(PRELUDE+'\n'.join(sources)+'\nint main(void){'+'\n'.join(checks)+'return 0;}')
    assert ran.returncode == 0,ran.stdout+ran.stderr


def test_zero_count_shifts_preserve_all_dynamic_flags():
    lifter=emitter()
    sources, checks=[],[]
    for m in ('shl','shr','sar'):
        for bits,dst,src in ((8,'al','cl'),(16,'ax','cx'),(32,'eax','ecx')):
            insn=Instruction(0,3,m,'','',operands=[Operand(type='reg',reg=dst),Operand(type='reg',reg=src)])
            stmts=' '.join(lifter.lift_instruction(insn))
            name=f'shift_{m}_{bits}'
            sources.append(f'int {name}(unsigned count){{int c=1;{LOCALS}eax=0xDEADBEEF;ecx=count;'
                           +'_flags=_sf=_of=_pf=1;_fv=15;'+stmts
                           +'return !(_flags==1 && _sf==1 && _of==1 && _pf==1 && _fv==15 && _cf==1 && eax==0xDEADBEEF);}')
            checks.extend(f'if({name}({c}))return 1;' for c in (0,32,64))
    ran=_build_and_run(PRELUDE+'\n'.join(sources)+'int main(void){'+''.join(checks)+'return 0;}')
    assert ran.returncode == 0,ran.stdout+ran.stderr


def test_common_zero_snapshot_survives_both_predecessor_clobbers():
    lifter=emitter()
    eaxop,ecxop=Operand(type='reg',reg='eax'),Operand(type='reg',reg='ecx')
    sub=' '.join(lifter.lift_instruction(Instruction(0,2,'sub','','',operands=[eaxop,ecxop])))
    dec=' '.join(lifter.lift_instruction(Instruction(2,1,'dec','','',operands=[eaxop])))
    cond=_make_condition('je','__zf_from_dest',[eaxop])[0]
    src=PRELUDE+f'int test(uint32_t a,uint32_t b,int edge){{int c=0;{LOCALS}eax=a;ecx=b;'
    src+=f'if(edge){{{sub}}}else{{{dec}}}eax=0x99;return {cond};}}'
    src+='int main(void){for(unsigned a=0;a<4;a++)for(unsigned b=0;b<4;b++)for(int e=0;e<2;e++)if(test(a,b,e)!=(e?a==b:a==1))return 1;return 0;}'
    ran=_build_and_run(src)
    assert ran.returncode == 0,ran.stdout+ran.stderr


def test_actual_cfg_join_jcc_setcc_cmovcc_and_loopne():
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator

    def image_for(consumer):
        image, labels, jumps = bytearray(), {}, []
        def emit(hexcode): image.extend(bytes.fromhex(hexcode))
        def label(name): labels[name]=len(image)
        def jump(op, target):
            emit(op+'00')
            jumps.append((len(image)-1,target))
        emit('83fa00')                     # choose an incoming edge
        jump('74','right')
        emit('39c8 b899000000')            # CMP, then overwrite its destination
        jump('eb','join')
        label('right')
        emit('21c8 b899000000')            # AND, a different producer kind
        label('join')
        if consumer == 'setl': emit('0f9cc0 0fb6c0 c3')
        elif consumer == 'cmovl': emit('b800000000 b901000000 0f4cc1 c3')
        else:
            if consumer == 'loopne': emit('b902000000')
            jump('e0' if consumer=='loopne' else '7c','yes')
            emit('b800000000 c3')
            label('yes')
            emit('b801000000 c3')
        for offset,target in jumps:
            displacement=labels[target]-(offset+1)
            assert -128<=displacement<128
            image[offset]=displacement&255
        return bytes(image)

    sources,checks=[],[]
    vals=(0,1,0x7fffffff,0x80000000,0xffffffff)
    for consumer in ('jl','setl','cmovl','loopne'):
        image=image_for(consumer)
        base=0x10000
        config._install([config.Section('.text',base,len(image),0,len(image),True)],
                        entry_point=base,kernel_thunk_addr=base,origin='dynamic-cfg-test')
        db={base:{'end':base+len(image),'size':len(image)}}
        code=FunctionTranslator(image,db).translate_function(base,db[base])
        assert '_fv &' in code, code       # the mixed join used the dynamic path
        name='cfg_'+consumer
        sources.append(code.replace('sub_00010000',name))
        for a in vals:
            for b in vals:
                for edge in (0,1):
                    flags=answer('cmp' if edge else 'test',a,b,0,32)
                    want=(flags >> (1 if consumer=='loopne' else 6))&1
                    checks.append(f'eax=0x{a:X}u;ecx=0x{b:X}u;edx={edge};esp=0x1000;{name}();'
                                  +f'if(eax!={want}){{printf("{consumer} {a:X} {b:X} {edge}\\n");return 1;}}')
    ran=_build_and_run(PRELUDE+'\n'.join(sources)+'int main(void){'+''.join(checks)+'return 0;}')
    assert ran.returncode == 0,ran.stdout+ran.stderr


def test_xadd_zero_flag_uses_atomic_result_before_memory_and_register_clobbers():
    from pathlib import Path
    header=(Path(__file__).resolve().parents[2]/'templates/runtime/recomp_types.h').read_text()
    end=header.index('#endif',header.index('#define RECOMP_ATOMIC_ADD32'))+len('#endif')
    start=header.rfind('#if',0,header.index('#define RECOMP_ATOMIC_ADD32'))
    atomics=header[start:end]
    lifter=emitter()
    operands=[Operand(type='mem',mem_base='ecx',mem_size=4),Operand(type='reg',reg='edx')]
    statements=' '.join(lifter.lift_instruction(Instruction(0,3,'lock xadd','','',operands=operands)))
    condition=_make_condition('je','xadd',operands)[0]
    source=PRELUDE+'\n#define XBOX_PTR(a) (mem+(a))\n'+atomics+'\n'
    source+=f'int test(uint32_t a,uint32_t b){{int c=0;{LOCALS}ecx=32;edx=b;MEM32(32)=a;'
    source+=statements+'if(edx!=a || MEM32(32)!=(uint32_t)(a+b))return -1;MEM32(32)=0x99;ecx=64;'
    source+=f'return {condition};}}'
    source+='int main(void){const uint32_t v[]={0,1,0xffffffff,0x80000000};for(unsigned i=0;i<4;i++)for(unsigned j=0;j<4;j++)if(test(v[i],v[j])!=((uint32_t)(v[i]+v[j])==0))return 1;return 0;}'
    ran=_build_and_run(source)
    assert ran.returncode == 0,ran.stdout+ran.stderr



def test_translated_known_arithmetic_and_partial_flag_writers():
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    sources, checks = [], []
    vals = (0, 1, 0x7fffffff, 0x80000000, 0xffffffff)
    cases = [
        ('add32', '01c8', 'add', 32), ('sub32', '29c8', 'sub', 32),
        ('neg32', 'f7d8', 'neg', 32), ('add16', '6601c8', 'add', 16),
        ('sub16', '6629c8', 'sub', 16), ('neg16', '66f7d8', 'neg', 16),
        ('add8', '00c8', 'add', 8), ('sub8', '28c8', 'sub', 8),
        ('neg8', 'f6d8', 'neg', 8),
        ('shift0', '39c8 c1e200', 'cmp', 32),
        ('shift32', '39c8 c1ea20', 'cmp', 32),
        ('shift_cl0', '39c8 b900000000 d3fa', 'cmp', 32),
        ('rdtsc', '39c8 0f31', 'cmp', 32),
        ('cpuid', '39c8 0fa2', 'cmp', 32),
        ('lfence', '39c8 0faee8', 'cmp', 32),
        ('mfence', '39c8 0faef0', 'cmp', 32),
        ('bsf', '0fbcc1', 'bsf', 32), ('bsr', '0fbdc1', 'bsr', 32),
    ]
    for tag, producer, kind, bits in cases:
        for consumer in ('jl', 'setl', 'cmovl', 'je', 'sete', 'cmove', 'loopne'):
            if kind in ('bsf','bsr') and consumer in ('jl','setl','cmovl'):
                continue  # SF/OF are architecturally undefined after bit scans.
            image = bytearray.fromhex(producer)
            # Clobber operands before consuming flags; MOV preserves EFLAGS.
            image.extend(bytes.fromhex('b899000000 b999000000'))
            if consumer.startswith('set'):
                image.extend(bytes.fromhex('0f9cc0' if consumer=='setl' else '0f94c0'))
                image.extend(bytes.fromhex('0fb6c0 c3'))
            elif consumer.startswith('cmov'):
                image.extend(bytes.fromhex('b800000000 b901000000'))
                image.extend(bytes.fromhex('0f4cc1' if consumer=='cmovl' else '0f44c1'))
                image.extend(bytes.fromhex('c3'))
            else:
                if consumer=='loopne': image.extend(bytes.fromhex('b902000000'))
                image.extend(bytes.fromhex({'jl':'7c','je':'74','loopne':'e0'}[consumer]+'06'))
                image.extend(bytes.fromhex('b800000000 c3 b801000000 c3'))
            base=0x10000
            config._install([config.Section('.text',base,len(image),0,len(image),True)],
                            entry_point=base,kernel_thunk_addr=base,origin='known-flags-test')
            db={base:{'end':base+len(image),'size':len(image)}}
            code=FunctionTranslator(bytes(image),db).translate_function(base,db[base])
            name='known_'+tag+'_'+consumer
            sources.append(code.replace('sub_00010000',name))
            for a in vals:
                for b in vals:
                    flags=answer(kind,a,b,0,bits) if kind not in ('bsf','bsr') else int(b==0)
                    bit=6 if consumer in ('jl','setl','cmovl') else 1 if consumer=='loopne' else 0
                    want=(flags>>bit)&1
                    if kind in ('bsf','bsr') and consumer=='loopne': want=not (b==0)
                    checks.append(f'eax=0x{a:X}u;ecx=0x{b:X}u;edx=0x99;esp=0x1000;{name}();'
                        +f'if(eax!={int(want)}){{printf("{tag} {consumer} {a:X} {b:X}\\n");return 1;}}')
    prelude=PRELUDE+'\nuint64_t xbox_ReadTimeStampCounter(void){return 123;}\n'
    ran=_build_and_run(prelude+'\n'.join(sources)+'int main(void){'+''.join(checks)+'return 0;}')
    assert ran.returncode == 0, ran.stdout+ran.stderr


def test_translated_double_shifts_and_rotations_publish_carry_and_overflow():
    from pathlib import Path
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    header=(Path(__file__).resolve().parents[2]/'templates/runtime/recomp_types.h').read_text(encoding='utf-8')
    helpers=header[header.index('static inline uint32_t ROL32'):header.index('/* ================================================================',header.index('static inline uint16_t ROR16'))]
    sources, checks=[],[]
    vals=(0,1,0x7fffffff,0x80000000,0xffffffff)
    for bits,prefix in ((16,'66'),(32,'')):
        mask=(1<<bits)-1
        for kind,opcode in (('shld','0fa4d0'),('shrd','0facd0'),('rol','c1c0'),('ror','c1c8'),('rcl','c1d0'),('rcr','c1d8')):
            for count in (0,1,2,16,32):
                for carry in (0,1):
                    for cc,consumer in (('cf','0f92c0'),('of','0f90c0')):
                        if cc=='of' and count not in (0,1,32): continue
                        image=bytes.fromhex('39c8 '+('f9' if carry else 'f8')+' '+prefix+opcode+f'{count:02x}'+' '+consumer+' 0fb6c0 c3')
                        base=0x10000
                        config._install([config.Section('.text',base,len(image),0,len(image),True)],entry_point=base,kernel_thunk_addr=base,origin='partial-flags-test')
                        db={base:{'end':base+len(image),'size':len(image)}}
                        name=f'partial_{kind}_{bits}_{count}_{carry}_{cc}'
                        code=FunctionTranslator(image,db).translate_function(base,db[base])
                        sources.append(code.replace('sub_00010000',name))
                        for a in vals:
                            for b in vals:
                                n=count&31
                                v=a&mask
                                cf=carry
                                of=(answer('cmp',a,b,0,32)>>4)&1
                                if n:
                                    if kind in ('shld','shrd'):
                                        if kind=='shld':
                                            cf=(v>>(bits-n))&1
                                            out=((v<<n)|((b&mask)>>(bits-n)))&mask
                                        else:
                                            cf=(v>>(n-1))&1
                                            out=((v>>n)|((b&mask)<<(bits-n)))&mask
                                        if n==1: of=((out^v)>>(bits-1))&1
                                    else:
                                        out=v
                                        for _ in range(n):
                                            if kind in ('rol','rcl'):
                                                incoming=cf if kind=='rcl' else (out>>(bits-1))&1
                                                cf=(out>>(bits-1))&1
                                                out=((out<<1)|incoming)&mask
                                            else:
                                                incoming=cf if kind=='rcr' else out&1
                                                cf=out&1
                                                out=(out>>1)|(incoming<<(bits-1))
                                        if n==1:
                                            of=((out>>(bits-1))^(cf if kind in ('rol','rcl') else (out>>(bits-2))))&1
                                want=cf if cc=='cf' else of
                                checks.append(f'eax=0x{a:X}u;ecx=0x{b:X}u;edx=ecx;esp=0x1000;{name}();if(eax!={want}){{printf("{name} {a:X} {b:X}\\n");return 1;}}')
    ran=_build_and_run(PRELUDE+'\n'+helpers+'\n'.join(sources)+'int main(void){'+''.join(checks)+'return 0;}')
    assert ran.returncode==0,ran.stdout+ran.stderr


def test_unsupported_atomics_and_bit_tests_clear_undefined_validity():
    from tools.recomp.disasm import BasicBlock
    from tools.recomp.lifter import lift_basic_block
    for m in ('xadd','cmpxchg','lock xadd','lock cmpxchg'):
        for dst in (Operand(type='reg',reg='eax'),Operand(type='mem',mem_base='esi',mem_size=1)):
            instruction=Instruction(0,3,m,'','',operands=[dst,Operand(type='reg',reg='ecx')])
            block=BasicBlock(0,[instruction])
            code,state=lift_basic_block(emitter(),block,('cmp',[]))
            assert state is None
            assert '_fv = 0;' in '\n'.join(code)
    for m in ('bt','bts','btr','btc'):
        instruction=Instruction(0,3,m,'','',operands=[Operand(type='reg',reg='eax'),Operand(type='imm',imm=3)])
        code,_=lift_basic_block(emitter(),BasicBlock(0,[instruction]))
        assert '_fv &= 1u;' in '\n'.join(code)
