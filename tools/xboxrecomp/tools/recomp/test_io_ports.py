"""Execute every IN/OUT encoding through the production translator."""
from . import config
from .translator import FunctionTranslator
from .test_dynamic_flags import PRELUDE
from .test_lifter_result_clobber import _build_and_run


def test_io_width_port_truncation_register_preservation_and_flags():
    helpers=r'''
unsigned calls,last_width,last_port,last_value;
#define IO(N,T) T xbox_IoRead##N(uint16_t p){calls++;last_width=N;last_port=p;return 0;} \
void xbox_IoWrite##N(uint16_t p,T v){calls++;last_width=N;last_port=p;last_value=v;}
IO(8,uint8_t) IO(16,uint16_t) IO(32,uint32_t)
'''
    sources,checks=[],[]
    for width,prefix,read,write in ((8,'','e4','e6'),(16,'66','e5','e7'),(32,'','e5','e7')):
        for direction,opcode in (('in',read),('out',write)):
            for immediate in (True,False):
                op=prefix+opcode+'7f' if immediate else prefix+{'e4':'ec','e5':'ed','e6':'ee','e7':'ef'}[opcode]
                # ZF=1 before I/O; copy EAX to ECX, consume ZF in DL.
                image=bytes.fromhex('39c0 '+op+' 89c1 0f94c2 0fb6d2 c3')
                base=0x10000
                config._install([config.Section('.text',base,len(image),0,len(image),True)],entry_point=base,kernel_thunk_addr=base,origin='io-port-test')
                db={base:{'end':base+len(image),'size':len(image)}}
                name=f'io_{direction}_{width}_{int(immediate)}'
                code=FunctionTranslator(image,db).translate_function(base,db[base])
                assert 'xbox_Io'+('Read' if direction=='in' else 'Write')+str(width) in code
                sources.append(code.replace('sub_00010000',name))
                mask=(1<<width)-1
                value=0xa5b6c7d8
                want=value&~mask if direction=='in' else value
                checks.append(f'eax=0x{value:X}u;edx=0x1234abcd;calls=0;esp=0x1000;{name}();'
                    +f'if(calls!=1||last_port!={0x7f if immediate else 0xabcd}||last_width!={width}||ecx!=0x{want:X}u||edx!=1)return 1;'
                    +(f'if(last_value!=0x{value&mask:X}u)return 1;' if direction=='out' else ''))
    ran=_build_and_run(PRELUDE+helpers+'\n'.join(sources)+'int main(void){'+''.join(checks)+'return 0;}')
    assert ran.returncode==0,ran.stdout+ran.stderr
