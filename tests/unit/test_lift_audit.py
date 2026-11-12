"""Unit tests for scripts/lift-audit.py using synthetic lifted C only (no game files)."""
import importlib.util, os
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
spec = importlib.util.spec_from_file_location("lift_audit", os.path.join(ROOT, "scripts", "lift-audit.py"))
la = importlib.util.module_from_spec(spec); spec.loader.exec_module(la)

# The old rep-compare lift: equality only, and the carry the sbb pair reads is
# the stale one from the xor before the loop.
OLD_MEMCMP = """void sub_00018890(void)
{
    int _flags = 0; /* fallback flag var */
loc_00018890: ;
    _cf = 0; /* xor clears CF */
    { int32_t _st = RECOMP_DF_STEP(1);
    while (ecx != 0) {
        _flags = (MEM8(esi) == MEM8(edi));
        esi += _st; edi += _st; ecx--;
        if (!_flags) break;
    } } /* repe cmpsb */
    if ((_flags != 0)) goto loc_000188AD; /* je: equal / zero */

loc_000188A8: ;
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
    if (_flags /* jp: parity */) goto loc_000188AD;
    if ((_fcmp && _fa > _fb) /* ja: above (unsigned >) */) goto loc_000188AD;
loc_000188AD: ;
}
"""

# The fixed lift: the compare writes _cf itself, inside its own block.
NEW_MEMCMP = """void sub_00018890(void)
{
    int _flags = 0; /* fallback flag var */
loc_00018890: ;
    _cf = 0; /* xor clears CF */
    { int32_t _st = RECOMP_DF_STEP(1); uint32_t _ra = 0, _rb = 0; int _ran = 0;
    while (ecx != 0) {
        _ra = (uint32_t)(MEM8(esi)) & 0xFFu; _rb = (uint32_t)(MEM8(edi)) & 0xFFu; _ran = 1;
        esi += _st; edi += _st; ecx--;
        if (_ra != _rb) break;
    }
    if (_ran) {
        _zf = (int)(_fa == _fb); _cf = (int)(_fa < _fb);
    }
    _flags = _zf; } /* repe cmpsb */
    if (_zf) goto loc_000188AD; /* je: equal / zero */

loc_000188A8: ;
    eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */
loc_000188AD: ;
}
"""


def _gen(tmp_path, text):
    (tmp_path / "recomp_0000.c").write_text(text)
    return str(tmp_path)


def test_old_lift_is_flagged(tmp_path):
    counts, where = la.audit(_gen(tmp_path, OLD_MEMCMP))
    assert counts["cmps_then_sbb"] == 1
    assert counts["unassigned_flags"] == 1      # the jp at a block boundary
    assert counts["fcmp_fallback"] == 1
    assert where["sub_00018890"]["cmps_then_sbb"] == 1


def test_fixed_lift_is_clean(tmp_path):
    counts, _ = la.audit(_gen(tmp_path, NEW_MEMCMP))
    assert counts["cmps_then_sbb"] == 0
    assert counts["unassigned_flags"] == 0


def test_limits_set_the_exit_status(tmp_path):
    d = _gen(tmp_path, OLD_MEMCMP)
    assert la.main([d, "--max", "cmps_then_sbb=0"]) == 1
    assert la.main([d, "--max", "cmps_then_sbb=1"]) == 0


def test_dynamic_flag_input_cannot_pass_historical_gate(tmp_path, capsys):
    source = '''void sub_00018890(void) {
    int _flags=0; unsigned _fv=0;
loc_00018890: ;
    if ((_fv & 1u) && _flags) goto loc_00018890;
}'''
    d = _gen(tmp_path, source)
    counts, _ = la.audit(d)
    assert counts['dynamic_flags_functions'] == 1
    assert counts['unassigned_flags'] == 0
    assert la.main([d, '--max', 'unassigned_flags=999']) == 2
    assert 'REFUSED' in capsys.readouterr().out


def test_unknown_metric_cannot_silently_pass(tmp_path):
    d = _gen(tmp_path, NEW_MEMCMP)
    assert la.main([d, '--max', 'misspelled_flags=0']) == 2
