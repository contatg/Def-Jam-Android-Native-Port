"""Only original, proven table ordinals may rescue a corrupted runtime slot."""
from .test_switch_table_offset_index import _image, _translate, BASE, TABLE, ARMS
from .test_lifter_result_clobber import _build_and_run
from .lifter import Lifter
from .disasm import Operand, Instruction
from . import config
from .test_icall_guarded_runtime import _macro, HARNESS


def test_compiled_original_positive_and_negative_slots():
    prelude=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint32_t eax,ecx,edx,esp,g_seh_ebp,ebp,g_ebp;
uint8_t image[512];
unsigned failed,site;
#define MEM32(a) (*(uint32_t*)(image+(uint32_t)(a)-0x10000u))
#define MEM16(a) (*(uint16_t*)(image+(uint32_t)(a)-0x10000u))
#define MEM8(a) (*(uint8_t*)(image+(uint32_t)(a)-0x10000u))
#define LO8(r) ((uint8_t)(r))
#define HI8(r) ((uint8_t)((r)>>8))
#define LO16(r) ((uint16_t)(r))
#define SET_LO8(r,v) ((r)=((r)&0xffffff00u)|(uint8_t)(v))
#define SET_LO16(r,v) ((r)=((r)&0xffff0000u)|(uint16_t)(v))
#define RECOMP_ITAIL_AT(a,s) do{failed++;site=s;}while(0)
'''
    functions, checks=[],[]
    cases=(('normal',bytes.fromhex('83e003'),TABLE,0),
           ('positive',bytes.fromhex('83e003'),TABLE-4,1),
           ('negative',bytes.fromhex('83e904'),TABLE+12,-3),
           ('last',bytes.fromhex('83e902'),TABLE+8,-2))
    for tag,prefix,disp,first in cases:
        body=_image(prefix,disp)
        functions.append(_translate(body).replace('sub_00010000','switch_'+tag))
        initializer=','.join(str(b) for b in body)
        setup=f'{{const uint8_t original[]={{{initializer}}};memcpy(image,original,sizeof original);}}'
        for ordinal in range(3):
            index=first+ordinal
            incoming=index+4 if tag=='negative' else (index+2 if tag=='last' else index)
            # A corrupted target may use a proven index. A valid runtime arm wins.
            for runtime,want in ((0xdeadbeef,ordinal+1),(ARMS[2],3)):
                checks.append(setup+f'MEM32(0x{TABLE+ordinal*4:X})=0x{runtime:X}u;'
                    +f'eax=ecx={incoming}u;esp=0x1000;failed=0;switch_{tag}();'
                    +f'if(eax!={want}||failed||esp!=0x1004)return 1;')
        # Outside the proven ordinal set must retain the tail diagnostic path.
        outside=3 if tag in ('normal', 'last') else 0
        checks.append(setup+f'eax=ecx={outside};esp=0x1000;failed=0;switch_{tag}();'
            +f'if(failed!=1||site!=0x{BASE+3:X})return 2;')
    ran=_build_and_run(prelude+'\n'.join(functions)+'int main(void){'+''.join(checks)+'return 0;}')
    assert ran.returncode==0,ran.stdout+ran.stderr


def test_ambiguous_shapes_and_changed_census_refuse_index_rescue():
    lifter=Lifter()
    for op in (Operand(type='mem',mem_index='eax',mem_scale=2,mem_disp=TABLE,mem_size=4),
               Operand(type='mem',mem_base='eax',mem_disp=TABLE,mem_size=4),
               Operand(type='mem',mem_index='eax',mem_scale=4,mem_disp=TABLE,mem_size=4,mem_seg='fs'),
               Operand(type='mem',mem_index='eax',mem_scale=4,mem_disp=TABLE,mem_size=2)):
        assert lifter._switch_index_pairs(op,ARMS)==[]
    op=Operand(type='mem',mem_index='eax',mem_scale=4,mem_disp=TABLE,mem_size=4)
    assert lifter._switch_index_pairs(op,ARMS*22)==[]


def test_compiled_foreign_arms_keep_value_first_and_guest_tail_frame():
    image = _image(bytes.fromhex('83e003'), TABLE)
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='foreign-slot-test')
    lifter = Lifter(func_db={a: {'name': f'sub_{a:08X}'} for a in ARMS}, xbe_data=image,
                    manual_functions={ARMS[1]})
    lifter.func_start, lifter.func_end = BASE, BASE + 10
    bodies, checks = [], []
    for tag, disp, first, step in (('normal', TABLE, 0, 1), ('positive', TABLE-4, 1, 1),
                                   ('negative', TABLE+12, -1, -1),
                                   ('last', TABLE+8, 0, -1)):
        op = Operand(type='mem', mem_index='ecx', mem_scale=4, mem_disp=disp, mem_size=4)
        insn = Instruction(BASE+3, 7, 'jmp', '', '', operands=[op])
        body = '\n'.join(lifter._lift_jmp(insn, [op]))
        assert 'foreign switch: 3 proven slots' in body
        bodies.append(f'void jump_{tag}(void){{{body}}}')
        ordered = ARMS if step == 1 else list(reversed(ARMS))
        for i, arm in enumerate(ordered):
            index = first + i * step
            for runtime, want in ((0xdeadbeef, arm), (ARMS[1], ARMS[1])):
                checks.append(f'ecx=(uint32_t){index};MEM32(0x{disp+index*4:X})=0x{runtime:X};'
                              f'esp=0x1000;ebp=0x1234;jump_{tag}();'
                              f'if(target!=0x{want:X}||site!=0x{BASE+3:X}||esp!=0x1004||g_seh_ebp!=0x1234)return 1;')
        # A live target outside the proven ordinal set retains generic dispatch.
        outside = 3 if tag == 'normal' else (1 if tag == 'last' else 0)
        checks.append(f'ecx=(uint32_t){outside};MEM32(0x{disp+outside*4:X})=0xdeadbeef;'
                      f'esp=0x1000;jump_{tag}();if(target!=0xdeadbeef||esp!=0x1004)return 2;')
    prelude = r'''
#include <stdint.h>
uint32_t ecx,esp,ebp,g_seh_ebp,target,site;
uint8_t image[512];
#define MEM32(a) (*(uint32_t*)(image+(uint32_t)(a)-0x10000u))
#define RECOMP_ITAIL_AT(a,s) do{target=(a);site=(s);esp+=4;}while(0)
'''
    ran = _build_and_run(prelude+'\n'.join(bodies)+'int main(void){'+''.join(checks)+'return 0;}')
    assert ran.returncode == 0, ran.stdout+ran.stderr


def test_foreign_table_requires_full_bounded_known_census():
    op = Operand(type='mem', mem_index='eax', mem_scale=4, mem_disp=TABLE, mem_size=4)
    lifter = Lifter(func_db={a: {} for a in ARMS})
    lifter.func_start, lifter.func_end = BASE, BASE + 10
    # Rejected first census must not be trimmed into an apparently valid table.
    lifter._read_jump_table = lambda va, max_entries=None: [0x10001, *ARMS] if va == TABLE else ARMS
    assert lifter._foreign_switch_index_pairs(op) == []
    lifter._read_jump_table = lambda va, max_entries=None: ARMS * 22 if va == TABLE else ARMS
    assert lifter._foreign_switch_index_pairs(op) == []
    lifter._read_jump_table = lambda va, max_entries=None: ARMS
    lifter.jump_table_targets = {TABLE: []}
    assert lifter._foreign_switch_index_pairs(op) == []
    lifter.jump_table_targets = {}
    op.mem_seg = 'fs'
    assert lifter._foreign_switch_index_pairs(op) == []


def test_tail_site_shared_across_translation_units_and_cleared_after_dispatch():
    # Compile the actual macros in one unit and observe the TLS variable from another.
    source=HARNESS.split('IS_CODE')[0].replace('uint32_t g_esp, eax;', 'uint32_t g_esp, eax;\n#define g_eax eax')
    source+='\nextern uint32_t read_site(void);\n'
    source+=_macro('RECOMP_ITAIL')+'\n'+_macro('RECOMP_ITAIL_AT')+'\n'
    source=source.replace('void recomp_icall_fail_log(uint32_t va) { (void)va; fails++; }',
        'void recomp_icall_fail_log(uint32_t va) { (void)va; fails++; if(read_site()!=0x10040) fails+=100; }')
    source='''#if defined(_MSC_VER)
#define TLS __declspec(thread)
#else
#define TLS __thread
#endif
'''+source
    source=source.replace('extern uint32_t read_site(void);','extern uint32_t read_site(void);\nextern TLS uint32_t g_itail_site;')
    # The earlier callback also needs the accessor prototype.
    source=source.replace('#include <stdio.h>','#include <stdio.h>\nuint32_t read_site(void);')
    source+='''
int main(void){g_esp=0x1000; RECOMP_ITAIL_AT(0x11000,0x10040);
if(g_esp!=0x1004||called_a!=1||read_site())return 1;
g_esp=0x1000; RECOMP_ITAIL_AT(0x11300,0x10040);
return g_esp!=0x1004||fails!=1||read_site()!=0;}
'''
    other='''#include <stdint.h>
#if defined(_MSC_VER)
#define TLS __declspec(thread)
#else
#define TLS __thread
#endif
TLS uint32_t g_itail_site;
uint32_t read_site(void){return g_itail_site;}
'''
    ran=_build_and_run(source,{'site.c':other})
    assert ran.returncode==0,ran.stdout+ran.stderr
