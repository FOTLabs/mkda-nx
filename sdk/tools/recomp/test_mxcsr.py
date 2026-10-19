import unittest
from .disasm import Instruction, Operand
from .lifter import Lifter


class MxcsrTest(unittest.TestCase):
    def test_control_word_accesses_guest_memory(self):
        for name, expected in (
            ("stmxcsr", "MEM32(ebp + -12) = recomp_get_mxcsr();"),
            ("ldmxcsr", "recomp_set_mxcsr(MEM32(ebp + -12));"),
        ):
            insn = Instruction(0x191346, 4, name, "dword ptr [ebp - 0xc]", "")
            insn.operands = [Operand(type="mem", mem_base="ebp", mem_disp=-12,
                                     mem_size=4)]
            self.assertEqual(Lifter().lift_instruction(insn), [expected])
