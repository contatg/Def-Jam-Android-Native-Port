"""A jcc after an ALU op tests the op's result, even when a flag-preserving
instruction has overwritten the destination in between."""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _insn(addr, mnemonic, op_str, operands):
    insn = Instruction(addr, 2, mnemonic, op_str, "9090")
    insn.operands = operands
    return insn


def _jump(mnemonic, address, target=0x40):
    jump = Instruction(address, 2, mnemonic, hex(target), "7400")
    jump.jump_target = target
    return jump


def _lifter():
    lifter = Lifter()
    lifter.func_start = 0
    lifter.func_end = 0x80
    lifter.has_flag_readers = True
    return lifter


def _reg(name):
    return Operand(type="reg", reg=name)


class FlagResultSavedTest(unittest.TestCase):
    def _lift(self, insns):
        lifted, _ = lift_basic_block(_lifter(), BasicBlock(start=0, instructions=insns))
        return "\n".join(lifted)

    def test_destination_overwritten_before_the_branch(self):
        # and eax, 400h / mov eax, [esp+29Ch] / je
        generated = self._lift([
            _insn(0, "and", "eax, 0x400", [_reg("eax"), Operand(type="imm", imm=0x400)]),
            _insn(2, "mov", "eax, dword ptr [esp + 0x29c]",
                  [_reg("eax"), Operand(type="mem", mem_base="esp", mem_disp=0x29C, mem_size=4)]),
            _jump("je", 4),
        ])
        save = generated.index("_fa = (uint32_t)(eax)")
        load = generated.index("eax = MEM32(esp + 0x29C);")
        self.assertLess(save, load)
        self.assertIn("if ((_fa == 0)) goto loc_00000040;", generated)

    def test_untouched_destination_also_uses_the_eager_snapshot(self):
        # and eax, 400h / mov ecx, edx / je: nothing to save
        generated = self._lift([
            _insn(0, "and", "eax, 0x400", [_reg("eax"), Operand(type="imm", imm=0x400)]),
            _insn(2, "mov", "ecx, edx", [_reg("ecx"), _reg("edx")]),
            _jump("je", 4),
        ])
        self.assertNotIn("_fres", generated)
        self.assertIn("if ((_fa == 0)) goto loc_00000040;", generated)

    def test_sub_register_write_counts(self):
        # add ecx, edx / mov cl, 1 / jne
        generated = self._lift([
            _insn(0, "add", "ecx, edx", [_reg("ecx"), _reg("edx")]),
            _insn(2, "mov", "cl, 1", [_reg("cl"), Operand(type="imm", imm=1)]),
            _jump("jne", 4),
        ])
        self.assertIn("_fa = (uint32_t)(ecx)", generated)
        self.assertIn("(_fa != 0)", generated)


if __name__ == "__main__":
    unittest.main()
