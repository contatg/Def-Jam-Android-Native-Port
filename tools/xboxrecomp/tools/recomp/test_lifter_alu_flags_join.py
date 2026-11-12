"""An ALU op's flags must reach a jcc that starts a later block."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

BASE = 0x10000


def translate(hexcode):
    image = bytes.fromhex(hexcode)
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE,
                    origin='alu-flags-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_and_publishes_zero_flag_for_a_join():
    # Def Jam's row padding (sub_001010D0):
    #   and eax, 8000003Fh; jns skip; dec eax; or eax, -40h; inc eax
    #   skip: je done; nop; done: ret
    # "skip" has two predecessors, so its je reads the published _zf, which
    # must come from the AND, not from whatever set it before.
    code = translate('253f000080' '7905' '48' '83c8c0' '40' '7401' '90' 'c3' 'c3')
    and_at = code.index('eax & 0x8000003Fu')
    zf_at = code.index('_flags = (_fa == 0);', and_at)
    assert zf_at < code.index('jns'), code
    assert '(_fv & 1u) == 1u && (_flags)' in code, code


def test_sub_publishes_a_compare():
    # sub ecx, edx; jmp join; join: jb out; ret; out: ret  -- sub's flags are a cmp's.
    code = translate('29d1' 'eb00' '7201' 'c3' 'c3')
    assert code.index('_fb = (uint32_t)(edx)') < code.index('ecx = ecx - edx'), code
    assert code.index('ecx = ecx - edx') < code.index('_fa = (uint32_t)(ecx)'), code
    assert '_cf = (int)((uint32_t)(ecx) < (uint32_t)(edx));' in code, code


def test_later_comparison_replaces_eager_add_snapshot():
    # add eax, 1; cmp eax, edx; je out; ret; out: ret -- the cmp owns the flags.
    code = translate('83c001' '39d0' '7401' 'c3' 'c3')
    assert code.index('/* add result') < code.index('/* cmp eax, edx'), code
    assert 'if (CMP_EQ(_fa, _fb))' in code, code
